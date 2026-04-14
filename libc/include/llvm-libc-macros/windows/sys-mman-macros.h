//===-- POSIX sys/mman.h macros for Windows -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Standard POSIX mmap/mprotect constants. Windows implementations translate
// these to Win32 equivalents internally.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SYS_MMAN_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SYS_MMAN_MACROS_H

// Protection flags
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

// Map flags
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE 0x100000 // Fail with EEXIST if range is occupied
#define MAP_POPULATE 0x08000         // Prefault pages after mapping
#define MAP_NORESERVE 0x4000         // Don't reserve pagefile (demand-commit)
#define MAP_HUGETLB 0x40000          // Use large pages (MEM_LARGE_PAGES)
#define MAP_LOCKED 0x2000            // Lock pages after mapping (mmap + mlock)
#define MAP_STACK 0x20000            // Hint for stack use (no-op, Linux compat)
#define MAP_32BIT 0x40               // Allocate in the low 2 GB of address space
#define MAP_NONBLOCK 0x10000         // No-op (readahead hint, no-op on Linux too)
#define MAP_UNINITIALIZED 0x4000000  // No-op (zero-fill always enforced)
#define MAP_WX 0x8000000             // Permit simultaneous W+X (JIT opt-in)

// Large page size encoding (matches Linux MAP_HUGE_* shift/mask)
#define MAP_HUGE_SHIFT 26
#define MAP_HUGE_MASK 0x3f
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)

// Failed mmap return value
#define MAP_FAILED ((void *)-1)

// mremap flags (values match Linux)
#define MREMAP_MAYMOVE 1  // Allow relocation to a new address
#define MREMAP_FIXED 2    // Move to a specific address (implies MAYMOVE)
#define MREMAP_DONTUNMAP 4 // Keep old mapping as zero-filled pages

// mlock2 flags (values match Linux)
#define MLOCK_ONFAULT 0x01 // Lock pages on fault (not on mlock2 call)

// madvise flags (values match Linux for cross-platform compatibility)
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8     // DiscardVirtualMemory / MEM_RESET (content immediately undefined)
#define MADV_COLD 20    // Deprioritize pages (VmPagePriorityInformation LOWEST)
#define MADV_PAGEOUT 21 // Actively evict pages (VmRemoveFromWorkingSetInformation)
#define MADV_POPULATE_READ 22  // Fault in pages for reading (PrefetchVirtualMemory)
#define MADV_POPULATE_WRITE 23 // Fault in pages for writing (PrefetchVirtualMemory)

// Crash dump control (values match Linux)
#define MADV_DONTDUMP 16   // Exclude from WER crash dumps
#define MADV_DODUMP 17     // Re-include in WER crash dumps

// Linux-specific hints with no Windows equivalent (no-op, values match Linux)
#define MADV_HUGEPAGE 14   // Enable transparent huge pages
#define MADV_NOHUGEPAGE 15 // Disable transparent huge pages
#define MADV_MERGEABLE 12  // Enable KSM (Kernel Samepage Merging)
#define MADV_UNMERGEABLE 13 // Disable KSM

// POSIX madvise flags
#define POSIX_MADV_NORMAL MADV_NORMAL
#define POSIX_MADV_RANDOM MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED MADV_WILLNEED
#define POSIX_MADV_DONTNEED MADV_DONTNEED

// msync flags
#define MS_ASYNC 1
#define MS_SYNC 4
#define MS_INVALIDATE 2

// mlockall flags (MCL = Memory Control Lock)
#define MCL_CURRENT 1  // Lock all current pages
#define MCL_FUTURE 2   // Lock all future pages
#define MCL_ONFAULT 4  // Lock pages on fault (deferred locking)

// NUMA memory policy modes (values match Linux numaif.h)
#define MPOL_DEFAULT 0    // Use system default (no explicit policy)
#define MPOL_PREFERRED 1  // Prefer a specific NUMA node
#define MPOL_BIND 2       // Restrict to specified NUMA nodes
#define MPOL_INTERLEAVE 3 // Interleave across specified NUMA nodes

// mbind flags (values match Linux numaif.h)
#define MPOL_MF_STRICT 0x01 // Verify existing placement
#define MPOL_MF_MOVE 0x02   // Move existing pages to conform

// get_mempolicy flags
#define MPOL_F_NODE 0x01        // Return node for addr (not policy)
#define MPOL_F_ADDR 0x02        // Look up policy for addr
#define MPOL_F_MEMS_ALLOWED 0x04 // Return allowed nodes mask

// memfd_create flags (values match Linux)
#define MFD_CLOEXEC 0x0001U       // Set FD_CLOEXEC on the fd
#define MFD_ALLOW_SEALING 0x0002U // Allow fcntl(F_ADD_SEALS)
#define MFD_HUGETLB 0x0004U      // Use large pages (SEC_LARGE_PAGES)

// Memory protection keys (values match Linux)
#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE 0x2

#endif // LLVM_LIBC_MACROS_WINDOWS_SYS_MMAN_MACROS_H
