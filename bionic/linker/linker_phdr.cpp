/*
 * Copyright (C) 2012 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "linker_phdr.h"

#include <errno.h>
#include <machine/exec.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linker.h"
#include "linker_debug.h"

/**
  TECHNICAL NOTE ON ELF LOADING.

  An ELF file's program header table contains one or more PT_LOAD
  segments, which corresponds to portions of the file that need to
  be mapped into the process' address space.

  Each loadable segment has the following important properties:

    p_offset  -> segment file offset
    p_filesz  -> segment file size
    p_memsz   -> segment memory size (always >= p_filesz)
    p_vaddr   -> segment's virtual address
    p_flags   -> segment flags (e.g. readable, writable, executable)

  We will ignore the p_paddr and p_align fields of ElfW(Phdr) for now.

  The loadable segments can be seen as a list of [p_vaddr ... p_vaddr+p_memsz)
  ranges of virtual addresses. A few rules apply:

  - the virtual address ranges should not overlap.

  - if a segment's p_filesz is smaller than its p_memsz, the extra bytes
    between them should always be initialized to 0.

  - ranges do not necessarily start or end at page boundaries. Two distinct
    segments can have their start and end on the same page. In this case, the
    page inherits the mapping flags of the latter segment.

  Finally, the real load addrs of each segment is not p_vaddr. Instead the
  loader decides where to load the first segment, then will load all others
  relative to the first one to respect the initial range layout.

  For example, consider the following list:

    [ offset:0,      filesz:0x4000, memsz:0x4000, vaddr:0x30000 ],
    [ offset:0x4000, filesz:0x2000, memsz:0x8000, vaddr:0x40000 ],

  This corresponds to two segments that cover these virtual address ranges:

       0x30000...0x34000
       0x40000...0x48000

  If the loader decides to load the first segment at address 0xa0000000
  then the segments' load address ranges will be:

       0xa0030000...0xa0034000
       0xa0040000...0xa0048000

  In other words, all segments must be loaded at an address that has the same
  constant offset from their p_vaddr value. This offset is computed as the
  difference between the first segment's load address, and its p_vaddr value.

  However, in practice, segments do _not_ start at page boundaries. Since we
  can only memory-map at page boundaries, this means that the bias is
  computed as:

       load_bias = phdr0_load_address - PAGE_START(phdr0->p_vaddr)

  (NOTE: The value must be used as a 32-bit unsigned integer, to deal with
          possible wrap around UINT32_MAX for possible large p_vaddr values).

  And that the phdr0_load_address must start at a page boundary, with
  the segment's real content starting at:

       phdr0_load_address + PAGE_OFFSET(phdr0->p_vaddr)

  Note that ELF requires the following condition to make the mmap()-ing work:

      PAGE_OFFSET(phdr0->p_vaddr) == PAGE_OFFSET(phdr0->p_offset)

  The load_bias must be added to any p_vaddr value read from the ELF file to
  determine the corresponding memory address.

 **/

#define MAYBE_MAP_FLAG(x, from, to)  (((x) & (from)) ? (to) : 0)
#define PFLAGS_TO_PROT(x)            (MAYBE_MAP_FLAG((x), PF_X, PROT_EXEC) | \
                                      MAYBE_MAP_FLAG((x), PF_R, PROT_READ) | \
                                      MAYBE_MAP_FLAG((x), PF_W, PROT_WRITE))

ElfReader::ElfReader(const char* name, int fd, off64_t file_offset)
    : name_(name), fd_(fd), file_offset_(file_offset),
      phdr_num_(0), phdr_mmap_(nullptr), phdr_table_(nullptr), phdr_size_(0),
      shdr_table_(nullptr), shdr_num_(0), dynamic_(nullptr), strtab_(nullptr),strtab_size_(0),
      load_start_(nullptr), load_size_(0), load_bias_(0),
      loaded_phdr_(nullptr) {
}

ElfReader::~ElfReader() {
  if (phdr_mmap_ != nullptr) {
    munmap(phdr_mmap_, phdr_size_);
  }
}

bool ElfReader::Load(const android_dlextinfo* extinfo) {
  return ReadElfHeader() &&
         VerifyElfHeader() &&
         ReadProgramHeader() &&
         ReserveAddressSpace(extinfo) &&
         LoadSegments() &&
         FindPhdr();
}

bool ElfReader::read(){

  return ReadSectionHeaders()&&ReadDynamicSection();

}



const char* ElfReader::get_string(ElfW(Word) index) const {
  CHECK(strtab_ != nullptr);
  CHECK(index < strtab_size_);

  return strtab_ + index;
}

bool ElfReader::ReadElfHeader() {
  ssize_t rc = TEMP_FAILURE_RETRY(pread64(fd_, &header_, sizeof(header_), file_offset_));
  if (rc < 0) {
    DL_ERR("can't read file \"%s\": %s", name_, strerror(errno));
    return false;
  }

  if (rc != sizeof(header_)) {
    DL_ERR("\"%s\" is too small to be an ELF executable: only found %zd bytes", name_,
           static_cast<size_t>(rc));
    return false;
  }
  return true;
}

bool safe_add(off64_t* out, off64_t a, size_t b) {
  CHECK(a >= 0);
  if (static_cast<uint64_t>(INT64_MAX - a) < b) {
    return false;
  }

  *out = a + b;
  return true;
}


bool ElfReader::CheckFileRange(ElfW(Addr) offset, size_t size, size_t alignment) {
  off64_t range_start;
  off64_t range_end;

  return safe_add(&range_start, file_offset_, offset) &&
         safe_add(&range_end, range_start, size) &&
         (range_start < file_size_) &&
         (range_end <= file_size_) &&
         ((offset % alignment) == 0);
}


bool ElfReader::VerifyElfHeader() {
  if (memcmp(header_.e_ident, ELFMAG, SELFMAG) != 0) {
    DL_ERR("\"%s\" has bad ELF magic", name_);
    return false;
  }

  // Try to give a clear diagnostic for ELF class mismatches, since they're
  // an easy mistake to make during the 32-bit/64-bit transition period.
  int elf_class = header_.e_ident[EI_CLASS];
#if defined(__LP64__)
  if (elf_class != ELFCLASS64) {
    if (elf_class == ELFCLASS32) {
      DL_ERR("\"%s\" is 32-bit instead of 64-bit", name_);
    } else {
      DL_ERR("\"%s\" has unknown ELF class: %d", name_, elf_class);
    }
    return false;
  }
#else
  if (elf_class != ELFCLASS32) {
    if (elf_class == ELFCLASS64) {
      DL_ERR("\"%s\" is 64-bit instead of 32-bit", name_);
    } else {
      DL_ERR("\"%s\" has unknown ELF class: %d", name_, elf_class);
    }
    return false;
  }
#endif

  if (header_.e_ident[EI_DATA] != ELFDATA2LSB) {
    DL_ERR("\"%s\" not little-endian: %d", name_, header_.e_ident[EI_DATA]);
    return false;
  }

  if (header_.e_type != ET_DYN) {
    DL_ERR("\"%s\" has unexpected e_type: %d", name_, header_.e_type);
    return false;
  }

  if (header_.e_version != EV_CURRENT) {
    DL_ERR("\"%s\" has unexpected e_version: %d", name_, header_.e_version);
    return false;
  }

  if (header_.e_machine != ELF_TARG_MACH) {
    DL_ERR("\"%s\" has unexpected e_machine: %d", name_, header_.e_machine);
    return false;
  }

  return true;
}

// Loads the program header table from an ELF file into a read-only private
// anonymous mmap-ed block.
bool ElfReader::ReadProgramHeader() {
  phdr_num_ = header_.e_phnum;

  // Like the kernel, we only accept program header tables that
  // are smaller than 64KiB.
  if (phdr_num_ < 1 || phdr_num_ > 65536/sizeof(ElfW(Phdr))) {
    DL_ERR("\"%s\" has invalid e_phnum: %zd", name_, phdr_num_);
    return false;
  }

  ElfW(Addr) page_min = PAGE_START(header_.e_phoff);
  ElfW(Addr) page_max = PAGE_END(header_.e_phoff + (phdr_num_ * sizeof(ElfW(Phdr))));
  ElfW(Addr) page_offset = PAGE_OFFSET(header_.e_phoff);

  phdr_size_ = page_max - page_min;

  void* mmap_result = mmap64(nullptr, phdr_size_, PROT_READ, MAP_PRIVATE, fd_, file_offset_ + page_min);
  if (mmap_result == MAP_FAILED) {
    DL_ERR("\"%s\" phdr mmap failed: %s", name_, strerror(errno));
    return false;
  }

  phdr_mmap_ = mmap_result;
  phdr_table_ = reinterpret_cast<ElfW(Phdr)*>(reinterpret_cast<char*>(mmap_result) + page_offset);
  return true;
}

bool ElfReader::ReadSectionHeaders() {
  shdr_num_ = header_.e_shnum;

  if (shdr_num_ == 0) {
    // DL_ERR_AND_LOG("\"%s\" has no section headers", name_);
    return false;
  }

  size_t size = shdr_num_ * sizeof(ElfW(Shdr));
  // if (!CheckFileRange(header_.e_shoff, size, alignof(const ElfW(Shdr)))) {
  //   DL_ERR_AND_LOG("\"%s\" has invalid shdr offset/size: %zu/%zu",
  //                  name_,
  //                  static_cast<size_t>(header_.e_shoff),
  //                  size);
  //   return false;
  // }

  if (!shdr_fragment_.Map(fd_, file_offset_, header_.e_shoff, size)) {
    DL_ERR("\"%s\" shdr mmap failed: %s", name_, strerror(errno));
    return false;
  }

  shdr_table_ = static_cast<const ElfW(Shdr)*>(shdr_fragment_.data());
  return true;
}




bool ElfReader::ReadDynamicSection() {
  // 1. Find .dynamic section (in section headers)
  const ElfW(Shdr)* dynamic_shdr = nullptr;
  for (size_t i = 0; i < shdr_num_; ++i) {
    if (shdr_table_[i].sh_type == SHT_DYNAMIC) {
      dynamic_shdr = &shdr_table_ [i];
      break;
    }
  }

  if (dynamic_shdr == nullptr) {
    // DL_ERR_AND_LOG("\"%s\" .dynamic section header was not found", name_);
    return false;
  }

  if (dynamic_shdr->sh_link >= shdr_num_) {
    // DL_ERR_AND_LOG("\"%s\" .dynamic section has invalid sh_link: %d",
    //                name_,
    //                dynamic_shdr->sh_link);
    return false;
  }

  const ElfW(Shdr)* strtab_shdr = &shdr_table_[dynamic_shdr->sh_link];

  if (strtab_shdr->sh_type != SHT_STRTAB) {
    // DL_ERR_AND_LOG("\"%s\" .dynamic section has invalid link(%d) sh_type: %d (expected SHT_STRTAB)",
    //                name_, dynamic_shdr->sh_link, strtab_shdr->sh_type);
    return false;
  }

  // if (!CheckFileRange(dynamic_shdr->sh_offset, dynamic_shdr->sh_size, alignof(const ElfW(Dyn)))) {
  //   DL_ERR_AND_LOG("\"%s\" has invalid offset/size of .dynamic section", name_);
  //   return false;
  // }

  if (!dynamic_fragment_.Map(fd_, file_offset_, dynamic_shdr->sh_offset, dynamic_shdr->sh_size)) {
    DL_ERR("\"%s\" dynamic section mmap failed: %s", name_, strerror(errno));
    return false;
  }

  dynamic_ = static_cast<const ElfW(Dyn)*>(dynamic_fragment_.data());

  // if (!CheckFileRange(strtab_shdr->sh_offset, strtab_shdr->sh_size, alignof(const char))) {
  //   DL_ERR_AND_LOG("\"%s\" has invalid offset/size of the .strtab section linked from .dynamic section",
  //                  name_);
  //   return false;
  // }

  if (!strtab_fragment_.Map(fd_, file_offset_, strtab_shdr->sh_offset, strtab_shdr->sh_size)) {
    DL_ERR("\"%s\" strtab section mmap failed: %s", name_, strerror(errno));
    return false;
  }

  strtab_ = static_cast<const char*>(strtab_fragment_.data());
  strtab_size_ = strtab_fragment_.size();
  return true;
}



/* Returns the size of the extent of all the possibly non-contiguous
 * loadable segments in an ELF program header table. This corresponds
 * to the page-aligned size in bytes that needs to be reserved in the
 * process' address space. If there are no loadable segments, 0 is
 * returned.
 *
 * If out_min_vaddr or out_max_vaddr are not null, they will be
 * set to the minimum and maximum addresses of pages to be reserved,
 * or 0 if there is nothing to load.
 */
size_t phdr_table_get_load_size(const ElfW(Phdr)* phdr_table, size_t phdr_count,
                                ElfW(Addr)* out_min_vaddr,
                                ElfW(Addr)* out_max_vaddr) {
  ElfW(Addr) min_vaddr = UINTPTR_MAX;
  ElfW(Addr) max_vaddr = 0;

  bool found_pt_load = false;
  for (size_t i = 0; i < phdr_count; ++i) {
    const ElfW(Phdr)* phdr = &phdr_table[i];

    if (phdr->p_type != PT_LOAD) {
      continue;
    }
    found_pt_load = true;

    if (phdr->p_vaddr < min_vaddr) {
      min_vaddr = phdr->p_vaddr;
    }

    if (phdr->p_vaddr + phdr->p_memsz > max_vaddr) {
      max_vaddr = phdr->p_vaddr + phdr->p_memsz;
    }
  }
  if (!found_pt_load) {
    min_vaddr = 0;
  }

  min_vaddr = PAGE_START(min_vaddr);
  max_vaddr = PAGE_END(max_vaddr);

  if (out_min_vaddr != nullptr) {
    *out_min_vaddr = min_vaddr;
  }
  if (out_max_vaddr != nullptr) {
    *out_max_vaddr = max_vaddr;
  }
  return max_vaddr - min_vaddr;
}



struct linker_maps_addr_t {
	uintptr_t prelinker_addr = 0xbc9e0000;
	uintptr_t prelinker_size = 0x30000;
	uintptr_t host_linker_addr = 0xbca20000;
	uintptr_t host_linker_size = 0x100000;
	uintptr_t guest_linker_addr = 0xbcb20000;
	uintptr_t guest_linker_size = 0x140000;
	uintptr_t guest_libc_addr = 0xbcc60000;
	uintptr_t guest_libc_size = 0x130000;
	uintptr_t host_libs_addr = 0xbcd90000;
	uintptr_t host_libs_size = 0;
	uintptr_t linker_maps_last_addr;
};
#if 0
// 	prelinker		0xbc9e0000		0x30000
// 	host linker 	0xbca20000		0x100000
//	guest linker	0xbcb20000		0x140000
//	guest libc		0xbcc60000		0x130000
//	host libs		0xbcd90000

uintptr_t prelinker_addr = 0xbc9e0000;
uintptr_t host_linker_addr = 0xbca20000;
uintptr_t guest_linker_addr = 0xbcb20000;
uintptr_t guest_libc_addr = 0xbcc60000;
uintptr_t host_libs_addr = 0xbcd90000;

uintptr_t prelinker_size = 0x30000;
uintptr_t host_linker_size = 0x100000;
uintptr_t guest_linker_size = 0x140000;
uintptr_t guest_libc_size = 0x130000;
uintptr_t host_libc_size = 0;
#else
#if defined(__LP64__)
#define LINKER_MAPS_ADDR 0x77FEEF0000+0x30000000l-0x4000l
#define ORI_PRELINKER_ADDR 0x77FEEF0000+0x30000000l
#else
#define LINKER_MAPS_ADDR 0xbc9dc000
#define ORI_PRELINKER_ADDR 0xbc9e0000
#endif
#define PRELINKER_ADDR (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->prelinker_addr)
#define PRELINKER_SIZE (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->prelinker_size)
#define HOST_LINKER_ADDR (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->host_linker_addr)
#define HOST_LINKER_SIZE (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->host_linker_size)
#define GUEST_LINKER_ADDR (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->guest_linker_addr)
#define GUEST_LINKER_SIZE (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->guest_linker_size)
#define GUEST_LIBC_ADDR (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->guest_libc_addr)
#define GUEST_LIBC_SIZE (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->guest_libc_size)
#define HOST_LIBS_ADDR (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->host_libs_addr)
#define HOST_LIBS_SIZE (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->host_libs_size)
#define LINKER_MAPS_LAST_ADDR (reinterpret_cast<linker_maps_addr_t*>(LINKER_MAPS_ADDR)->linker_maps_last_addr)
// #define host_linker_addr 0xbca20000
// #define host_libs_addr 0xbcd90000
#endif

#ifndef PAGE_ALIGN
#define PAGE_ALIGN(_v) (((_v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#endif 

#define __AUDIT_ARCH_64BIT 0x80000000
#define __AUDIT_ARCH_LE 0x40000000
#define EM_ARM 40
#define EM_AARCH64 183
#define AUDIT_ARCH_AARCH64 (EM_AARCH64 | __AUDIT_ARCH_64BIT | __AUDIT_ARCH_LE)
#define AUDIT_ARCH_ARM (EM_ARM | __AUDIT_ARCH_LE)

#include "private/kernel_sigset_t.h"
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <asm/unistd.h>
void init_seccomp(void)
{
	struct sock_filter filter[] = 
	{
		// block sigaction(SIGSYS)
		// BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
		// 		offsetof(struct seccomp_data, nr)),
		// BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
		// 		(unsigned long)__NR_sigaction, 0, 3), 
		// BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
		// 		offsetof(struct seccomp_data, args)),
		// BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
		// 		(unsigned long)SIGSYS, 0, 1), 
		// BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

// seccomp 架构判断, 获取pc寄存器地址，判断是否位于白名单内
#if !defined(__LP64__)

		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)AUDIT_ARCH_ARM, 1, 0), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer)),

		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(unsigned long)0x400000, 1, 0),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

		// pass allowed address
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer)),

		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(unsigned long)PRELINKER_ADDR, 0, 2),

		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(unsigned long)LINKER_MAPS_LAST_ADDR, 1, 0),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

#else 

		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)AUDIT_ARCH_AARCH64, 1, 0), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer) + 4),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)0, 0, 3),

    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer)),

		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(unsigned long)0x500000, 1, 0),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

    // compare linker_begin
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K,
				(uint32_t)(PRELINKER_ADDR >> 32), 3, 0),  // ture: compare linker_end false: compare equ
		// compare equ linker_begin
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(uint32_t)(PRELINKER_ADDR >> 32), 0, 8),  // ture: compare linker_begin low 32bit false: next filter
		// compare linker_begin low 32bit
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(uint32_t)(PRELINKER_ADDR & 0xFFFFFFFF), 0, 6),   // ture: compare linker_end false: next filter
		// compare linker_end
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K,
				(uint32_t)((LINKER_MAPS_LAST_ADDR) >> 32), 4, 0),  // true: next filter false: compare equ
		// compare equ linker_end
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(uint32_t)((LINKER_MAPS_LAST_ADDR) >> 32), 0, 3),  // true: compare linker_end low 32bit false: next filter 
		// compare linker_end low 32bit
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
				offsetof(struct seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,
				(uint32_t)((LINKER_MAPS_LAST_ADDR) & 0xFFFFFFFF), 1, 0),  // true: next filter  false: allow

		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

#endif

// 开始拦截系统调用
    // block non-allowed syscall
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
		offsetof(struct seccomp_data, nr)),
	
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_openat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_readlinkat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_faccessat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_unlinkat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_connect, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_execve, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_inotify_add_watch, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_mkdirat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getdents64, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_ptrace, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_clock_settime, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_clock_gettime, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_gettimeofday, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_settimeofday, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

// 32位的系统调用
#if !defined(__LP64__)
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_open, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_readlink, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_access, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getuid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getgid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_fstat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_fstat64, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_statfs64, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_uname, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
        (unsigned long)__NR_ioprio_set, 0, 1), 
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_sysinfo, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_socket, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_ioctl, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_prctl, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),
    
    // BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
    //     (unsigned long)__NR_fork, 0, 1), 
    // BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getuid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getgid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_geteuid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getegid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getresuid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getresgid32, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_inotify_add_watch, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_fstatat64, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),
#else
// 64 位系统调用
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_newfstatat, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getuid, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getgid, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),


    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_geteuid, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getegid, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

      BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getresuid, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,
				(unsigned long)__NR_getresgid, 0, 1), 
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),

#endif



		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),

    // BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP),  
	};

	struct sock_fprog prog = {
		.len = (unsigned short)(sizeof(filter)/sizeof(filter[0])),
		.filter = filter,
	};

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog))
      __libc_fatal_no_abort("blocked syscall failed %d",errno);

    return;
}

// Reserve a virtual address range big enough to hold all loadable
// segments of a program header table. This is done by creating a
// private anonymous mmap() with PROT_NONE.
bool ElfReader::ReserveAddressSpace(const android_dlextinfo* extinfo) {
  ElfW(Addr) min_vaddr;
  load_size_ = phdr_table_get_load_size(phdr_table_, phdr_num_, &min_vaddr);
  if (load_size_ == 0) {
    DL_ERR("\"%s\" has no loadable segments", name_);
    return false;
  }

  uint8_t* addr = reinterpret_cast<uint8_t*>(min_vaddr);
  void* start;
  size_t reserved_size = 0;
  bool reserved_hint = true;

  if (extinfo != nullptr) {
    if (extinfo->flags & ANDROID_DLEXT_RESERVED_ADDRESS) {
      reserved_size = extinfo->reserved_size;
      reserved_hint = false;
    } else if (extinfo->flags & ANDROID_DLEXT_RESERVED_ADDRESS_HINT) {
      reserved_size = extinfo->reserved_size;
    }
  }

  if (load_size_ > reserved_size) {
    if (!reserved_hint) {
      DL_ERR("reserved address space %zd smaller than %zd bytes needed for \"%s\"",
             reserved_size - load_size_, load_size_, name_);
      return false;
    }
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    // start = mmap(addr, load_size_, PROT_NONE, mmap_flags, -1, 0);


    uint8_t* map_addr = addr;
    int isGuestLibc = 0;
    if (NULL==map_addr &&0!=strstr(name_,"libc.so") )
    {
      isGuestLibc = 1;
        //host libc.so
  #if defined(__LP64__)
      map_addr = (uint8_t* )GUEST_LIBC_ADDR;
  #else
      map_addr = (uint8_t* )GUEST_LIBC_ADDR;
  #endif
    }

    start = mmap(map_addr, load_size_, PROT_NONE, mmap_flags, -1, 0);
    if (start == MAP_FAILED) {
      DL_ERR("couldn't reserve %zd bytes of address space for \"%s\"", load_size_, name_);
      return false;
    }
    if (isGuestLibc) {
      GUEST_LIBC_ADDR = reinterpret_cast<uintptr_t>(map_addr);
			GUEST_LIBC_SIZE = load_size_;
			// LINKER_MAPS_LAST_ADDR = PAGE_ALIGN(GUEST_LIBC_ADDR + GUEST_LIBC_SIZE);
      init_seccomp();
    }
  } else {
    start = extinfo->reserved_addr;
  }

  load_start_ = start;
  load_bias_ = reinterpret_cast<uint8_t*>(start) - addr;
  return true;
}

bool ElfReader::LoadSegments() {
  for (size_t i = 0; i < phdr_num_; ++i) {
    const ElfW(Phdr)* phdr = &phdr_table_[i];

    if (phdr->p_type != PT_LOAD) {
      continue;
    }

    // Segment addresses in memory.
    ElfW(Addr) seg_start = phdr->p_vaddr + load_bias_;
    ElfW(Addr) seg_end   = seg_start + phdr->p_memsz;

    ElfW(Addr) seg_page_start = PAGE_START(seg_start);
    ElfW(Addr) seg_page_end   = PAGE_END(seg_end);

    ElfW(Addr) seg_file_end   = seg_start + phdr->p_filesz;

    // File offsets.
    ElfW(Addr) file_start = phdr->p_offset;
    ElfW(Addr) file_end   = file_start + phdr->p_filesz;

    ElfW(Addr) file_page_start = PAGE_START(file_start);
    ElfW(Addr) file_length = file_end - file_page_start;

    if (file_length != 0) {
      void* seg_addr = mmap64(reinterpret_cast<void*>(seg_page_start),
                            file_length,
                            PFLAGS_TO_PROT(phdr->p_flags),
                            MAP_FIXED|MAP_PRIVATE,
                            fd_,
                            file_offset_ + file_page_start);
      if (seg_addr == MAP_FAILED) {
        DL_ERR("couldn't map \"%s\" segment %zd: %s", name_, i, strerror(errno));
        return false;
      }
    }

    // if the segment is writable, and does not end on a page boundary,
    // zero-fill it until the page limit.
    if ((phdr->p_flags & PF_W) != 0 && PAGE_OFFSET(seg_file_end) > 0) {
      memset(reinterpret_cast<void*>(seg_file_end), 0, PAGE_SIZE - PAGE_OFFSET(seg_file_end));
    }

    seg_file_end = PAGE_END(seg_file_end);

    // seg_file_end is now the first page address after the file
    // content. If seg_end is larger, we need to zero anything
    // between them. This is done by using a private anonymous
    // map for all extra pages.
    if (seg_page_end > seg_file_end) {
      void* zeromap = mmap(reinterpret_cast<void*>(seg_file_end),
                           seg_page_end - seg_file_end,
                           PFLAGS_TO_PROT(phdr->p_flags),
                           MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE,
                           -1,
                           0);
      if (zeromap == MAP_FAILED) {
        DL_ERR("couldn't zero fill \"%s\" gap: %s", name_, strerror(errno));
        return false;
      }
    }
  }
  return true;
}

/* Used internally. Used to set the protection bits of all loaded segments
 * with optional extra flags (i.e. really PROT_WRITE). Used by
 * phdr_table_protect_segments and phdr_table_unprotect_segments.
 */
static int _phdr_table_set_load_prot(const ElfW(Phdr)* phdr_table, size_t phdr_count,
                                     ElfW(Addr) load_bias, int extra_prot_flags) {
  const ElfW(Phdr)* phdr = phdr_table;
  const ElfW(Phdr)* phdr_limit = phdr + phdr_count;

  for (; phdr < phdr_limit; phdr++) {
    if (phdr->p_type != PT_LOAD || (phdr->p_flags & PF_W) != 0) {
      continue;
    }

    ElfW(Addr) seg_page_start = PAGE_START(phdr->p_vaddr) + load_bias;
    ElfW(Addr) seg_page_end   = PAGE_END(phdr->p_vaddr + phdr->p_memsz) + load_bias;

    int ret = mprotect(reinterpret_cast<void*>(seg_page_start),
                       seg_page_end - seg_page_start,
                       PFLAGS_TO_PROT(phdr->p_flags) | extra_prot_flags);
    if (ret < 0) {
      return -1;
    }
  }
  return 0;
}

/* Restore the original protection modes for all loadable segments.
 * You should only call this after phdr_table_unprotect_segments and
 * applying all relocations.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 * Return:
 *   0 on error, -1 on failure (error code in errno).
 */
int phdr_table_protect_segments(const ElfW(Phdr)* phdr_table, size_t phdr_count, ElfW(Addr) load_bias) {
  return _phdr_table_set_load_prot(phdr_table, phdr_count, load_bias, 0);
}

/* Change the protection of all loaded segments in memory to writable.
 * This is useful before performing relocations. Once completed, you
 * will have to call phdr_table_protect_segments to restore the original
 * protection flags on all segments.
 *
 * Note that some writable segments can also have their content turned
 * to read-only by calling phdr_table_protect_gnu_relro. This is no
 * performed here.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 * Return:
 *   0 on error, -1 on failure (error code in errno).
 */
int phdr_table_unprotect_segments(const ElfW(Phdr)* phdr_table, size_t phdr_count, ElfW(Addr) load_bias) {
  return _phdr_table_set_load_prot(phdr_table, phdr_count, load_bias, PROT_WRITE);
}

/* Used internally by phdr_table_protect_gnu_relro and
 * phdr_table_unprotect_gnu_relro.
 */
static int _phdr_table_set_gnu_relro_prot(const ElfW(Phdr)* phdr_table, size_t phdr_count,
                                          ElfW(Addr) load_bias, int prot_flags) {
  const ElfW(Phdr)* phdr = phdr_table;
  const ElfW(Phdr)* phdr_limit = phdr + phdr_count;

  for (phdr = phdr_table; phdr < phdr_limit; phdr++) {
    if (phdr->p_type != PT_GNU_RELRO) {
      continue;
    }

    // Tricky: what happens when the relro segment does not start
    // or end at page boundaries? We're going to be over-protective
    // here and put every page touched by the segment as read-only.

    // This seems to match Ian Lance Taylor's description of the
    // feature at http://www.airs.com/blog/archives/189.

    //    Extract:
    //       Note that the current dynamic linker code will only work
    //       correctly if the PT_GNU_RELRO segment starts on a page
    //       boundary. This is because the dynamic linker rounds the
    //       p_vaddr field down to the previous page boundary. If
    //       there is anything on the page which should not be read-only,
    //       the program is likely to fail at runtime. So in effect the
    //       linker must only emit a PT_GNU_RELRO segment if it ensures
    //       that it starts on a page boundary.
    ElfW(Addr) seg_page_start = PAGE_START(phdr->p_vaddr) + load_bias;
    ElfW(Addr) seg_page_end   = PAGE_END(phdr->p_vaddr + phdr->p_memsz) + load_bias;

    int ret = mprotect(reinterpret_cast<void*>(seg_page_start),
                       seg_page_end - seg_page_start,
                       prot_flags);
    if (ret < 0) {
      return -1;
    }
  }
  return 0;
}

/* Apply GNU relro protection if specified by the program header. This will
 * turn some of the pages of a writable PT_LOAD segment to read-only, as
 * specified by one or more PT_GNU_RELRO segments. This must be always
 * performed after relocations.
 *
 * The areas typically covered are .got and .data.rel.ro, these are
 * read-only from the program's POV, but contain absolute addresses
 * that need to be relocated before use.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 * Return:
 *   0 on error, -1 on failure (error code in errno).
 */
int phdr_table_protect_gnu_relro(const ElfW(Phdr)* phdr_table, size_t phdr_count, ElfW(Addr) load_bias) {
  return _phdr_table_set_gnu_relro_prot(phdr_table, phdr_count, load_bias, PROT_READ);
}

/* Serialize the GNU relro segments to the given file descriptor. This can be
 * performed after relocations to allow another process to later share the
 * relocated segment, if it was loaded at the same address.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 *   fd          -> writable file descriptor to use
 * Return:
 *   0 on error, -1 on failure (error code in errno).
 */
int phdr_table_serialize_gnu_relro(const ElfW(Phdr)* phdr_table, size_t phdr_count, ElfW(Addr) load_bias,
                                   int fd) {
  const ElfW(Phdr)* phdr = phdr_table;
  const ElfW(Phdr)* phdr_limit = phdr + phdr_count;
  ssize_t file_offset = 0;

  for (phdr = phdr_table; phdr < phdr_limit; phdr++) {
    if (phdr->p_type != PT_GNU_RELRO) {
      continue;
    }

    ElfW(Addr) seg_page_start = PAGE_START(phdr->p_vaddr) + load_bias;
    ElfW(Addr) seg_page_end   = PAGE_END(phdr->p_vaddr + phdr->p_memsz) + load_bias;
    ssize_t size = seg_page_end - seg_page_start;

    ssize_t written = TEMP_FAILURE_RETRY(write(fd, reinterpret_cast<void*>(seg_page_start), size));
    if (written != size) {
      return -1;
    }
    void* map = mmap(reinterpret_cast<void*>(seg_page_start), size, PROT_READ,
                     MAP_PRIVATE|MAP_FIXED, fd, file_offset);
    if (map == MAP_FAILED) {
      return -1;
    }
    file_offset += size;
  }
  return 0;
}

/* Where possible, replace the GNU relro segments with mappings of the given
 * file descriptor. This can be performed after relocations to allow a file
 * previously created by phdr_table_serialize_gnu_relro in another process to
 * replace the dirty relocated pages, saving memory, if it was loaded at the
 * same address. We have to compare the data before we map over it, since some
 * parts of the relro segment may not be identical due to other libraries in
 * the process being loaded at different addresses.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 *   fd          -> readable file descriptor to use
 * Return:
 *   0 on error, -1 on failure (error code in errno).
 */
int phdr_table_map_gnu_relro(const ElfW(Phdr)* phdr_table, size_t phdr_count, ElfW(Addr) load_bias,
                             int fd) {
  // Map the file at a temporary location so we can compare its contents.
  struct stat file_stat;
  if (TEMP_FAILURE_RETRY(fstat(fd, &file_stat)) != 0) {
    return -1;
  }
  off_t file_size = file_stat.st_size;
  void* temp_mapping = nullptr;
  if (file_size > 0) {
    temp_mapping = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (temp_mapping == MAP_FAILED) {
      return -1;
    }
  }
  size_t file_offset = 0;

  // Iterate over the relro segments and compare/remap the pages.
  const ElfW(Phdr)* phdr = phdr_table;
  const ElfW(Phdr)* phdr_limit = phdr + phdr_count;

  for (phdr = phdr_table; phdr < phdr_limit; phdr++) {
    if (phdr->p_type != PT_GNU_RELRO) {
      continue;
    }

    ElfW(Addr) seg_page_start = PAGE_START(phdr->p_vaddr) + load_bias;
    ElfW(Addr) seg_page_end   = PAGE_END(phdr->p_vaddr + phdr->p_memsz) + load_bias;

    char* file_base = static_cast<char*>(temp_mapping) + file_offset;
    char* mem_base = reinterpret_cast<char*>(seg_page_start);
    size_t match_offset = 0;
    size_t size = seg_page_end - seg_page_start;

    if (file_size - file_offset < size) {
      // File is too short to compare to this segment. The contents are likely
      // different as well (it's probably for a different library version) so
      // just don't bother checking.
      break;
    }

    while (match_offset < size) {
      // Skip over dissimilar pages.
      while (match_offset < size &&
             memcmp(mem_base + match_offset, file_base + match_offset, PAGE_SIZE) != 0) {
        match_offset += PAGE_SIZE;
      }

      // Count similar pages.
      size_t mismatch_offset = match_offset;
      while (mismatch_offset < size &&
             memcmp(mem_base + mismatch_offset, file_base + mismatch_offset, PAGE_SIZE) == 0) {
        mismatch_offset += PAGE_SIZE;
      }

      // Map over similar pages.
      if (mismatch_offset > match_offset) {
        void* map = mmap(mem_base + match_offset, mismatch_offset - match_offset,
                         PROT_READ, MAP_PRIVATE|MAP_FIXED, fd, match_offset);
        if (map == MAP_FAILED) {
          munmap(temp_mapping, file_size);
          return -1;
        }
      }

      match_offset = mismatch_offset;
    }

    // Add to the base file offset in case there are multiple relro segments.
    file_offset += size;
  }
  munmap(temp_mapping, file_size);
  return 0;
}


#if defined(__arm__)

#  ifndef PT_ARM_EXIDX
#    define PT_ARM_EXIDX    0x70000001      /* .ARM.exidx segment */
#  endif

/* Return the address and size of the .ARM.exidx section in memory,
 * if present.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 * Output:
 *   arm_exidx       -> address of table in memory (null on failure).
 *   arm_exidx_count -> number of items in table (0 on failure).
 * Return:
 *   0 on error, -1 on failure (_no_ error code in errno)
 */
int phdr_table_get_arm_exidx(const ElfW(Phdr)* phdr_table, size_t phdr_count,
                             ElfW(Addr) load_bias,
                             ElfW(Addr)** arm_exidx, unsigned* arm_exidx_count) {
  const ElfW(Phdr)* phdr = phdr_table;
  const ElfW(Phdr)* phdr_limit = phdr + phdr_count;

  for (phdr = phdr_table; phdr < phdr_limit; phdr++) {
    if (phdr->p_type != PT_ARM_EXIDX) {
      continue;
    }

    *arm_exidx = reinterpret_cast<ElfW(Addr)*>(load_bias + phdr->p_vaddr);
    *arm_exidx_count = (unsigned)(phdr->p_memsz / 8);
    return 0;
  }
  *arm_exidx = nullptr;
  *arm_exidx_count = 0;
  return -1;
}
#endif

/* Return the address and size of the ELF file's .dynamic section in memory,
 * or null if missing.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 * Output:
 *   dynamic       -> address of table in memory (null on failure).
 *   dynamic_flags -> protection flags for section (unset on failure)
 * Return:
 *   void
 */
void phdr_table_get_dynamic_section(const ElfW(Phdr)* phdr_table, size_t phdr_count,
                                    ElfW(Addr) load_bias, ElfW(Dyn)** dynamic,
                                    ElfW(Word)* dynamic_flags) {
  *dynamic = nullptr;
  for (const ElfW(Phdr)* phdr = phdr_table, *phdr_limit = phdr + phdr_count; phdr < phdr_limit; phdr++) {
    if (phdr->p_type == PT_DYNAMIC) {
      *dynamic = reinterpret_cast<ElfW(Dyn)*>(load_bias + phdr->p_vaddr);
      if (dynamic_flags) {
        *dynamic_flags = phdr->p_flags;
      }
      return;
    }
  }
}

// Returns the address of the program header table as it appears in the loaded
// segments in memory. This is in contrast with 'phdr_table_' which
// is temporary and will be released before the library is relocated.
bool ElfReader::FindPhdr() {
  const ElfW(Phdr)* phdr_limit = phdr_table_ + phdr_num_;

  // If there is a PT_PHDR, use it directly.
  for (const ElfW(Phdr)* phdr = phdr_table_; phdr < phdr_limit; ++phdr) {
    if (phdr->p_type == PT_PHDR) {
      return CheckPhdr(load_bias_ + phdr->p_vaddr);
    }
  }

  // Otherwise, check the first loadable segment. If its file offset
  // is 0, it starts with the ELF header, and we can trivially find the
  // loaded program header from it.
  for (const ElfW(Phdr)* phdr = phdr_table_; phdr < phdr_limit; ++phdr) {
    if (phdr->p_type == PT_LOAD) {
      if (phdr->p_offset == 0) {
        ElfW(Addr)  elf_addr = load_bias_ + phdr->p_vaddr;
        const ElfW(Ehdr)* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(elf_addr);
        ElfW(Addr)  offset = ehdr->e_phoff;
        return CheckPhdr((ElfW(Addr))ehdr + offset);
      }
      break;
    }
  }

  DL_ERR("can't find loaded phdr for \"%s\"", name_);
  return false;
}

// Ensures that our program header is actually within a loadable
// segment. This should help catch badly-formed ELF files that
// would cause the linker to crash later when trying to access it.
bool ElfReader::CheckPhdr(ElfW(Addr) loaded) {
  const ElfW(Phdr)* phdr_limit = phdr_table_ + phdr_num_;
  ElfW(Addr) loaded_end = loaded + (phdr_num_ * sizeof(ElfW(Phdr)));
  for (ElfW(Phdr)* phdr = phdr_table_; phdr < phdr_limit; ++phdr) {
    if (phdr->p_type != PT_LOAD) {
      continue;
    }
    ElfW(Addr) seg_start = phdr->p_vaddr + load_bias_;
    ElfW(Addr) seg_end = phdr->p_filesz + seg_start;
    if (seg_start <= loaded && loaded_end <= seg_end) {
      loaded_phdr_ = reinterpret_cast<const ElfW(Phdr)*>(loaded);
      return true;
    }
  }
  DL_ERR("\"%s\" loaded phdr %p not in loadable segment", name_, reinterpret_cast<void*>(loaded));
  return false;
}
