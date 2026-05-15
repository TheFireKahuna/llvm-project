//===-- NT memory constants, structures, and enums ----------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

//===----------------------------------------------------------------------===//
// Memory Constants
//===----------------------------------------------------------------------===//

// Memory state values (returned by NtQueryVirtualMemory)
// Pages are free — not committed or reserved, available for allocation.
inline constexpr DWORD MEM_FREE = 0x00010000;
// Pages are committed — physical storage allocated in RAM or pagefile.
inline constexpr DWORD MEM_COMMIT = 0x00001000;
// Pages are reserved — address space set aside, no physical storage yet.
inline constexpr DWORD MEM_RESERVE = 0x00002000;

// Memory type values (returned by NtQueryVirtualMemory)
// Region is mapped from an image section (PE executable/DLL).
inline constexpr DWORD MEM_IMAGE = 0x01000000;
// Region is private to the process (not shared or file-backed).
inline constexpr DWORD MEM_PRIVATE = 0x00020000;
// Region is mapped from a data/pagefile-backed section (shared memory).
inline constexpr DWORD MEM_MAPPED = 0x00040000;

// Memory allocation/free types (NtAllocateVirtualMemoryEx / NtFreeVirtualMemory)
//
// The MEM_COMMIT/MEM_RESERVE values above double as allocation flags:
//
// MEM_COMMIT (alloc): commits reserved pages — allocates physical storage
// (RAM/pagefile). Contents are zero-filled on first access. If BaseAddress
// is non-NULL, the range must already be reserved (or combine with
// MEM_RESERVE). Committing already-committed pages is a no-op (not an error).
//
// MEM_RESERVE (alloc): reserves address space without backing physical
// storage. Cannot reserve already-reserved pages. Combine with MEM_COMMIT
// to reserve and commit in one call.
//
// Decommits committed pages, releasing physical storage but keeping the
// reservation. Pages become reserved and inaccessible.
inline constexpr DWORD MEM_DECOMMIT = 0x00004000;
// Releases a region of pages. BaseAddress must be the exact value returned by
// the original NtAllocateVirtualMemoryEx, and RegionSize must be 0 (releases
// the entire region). Cannot combine with MEM_DECOMMIT.
inline constexpr DWORD MEM_RELEASE = 0x00008000;
// Marks committed pages as uninteresting — contents may be discarded.
// The range remains committed but may be zero-filled on next access.
// Must not combine with any other flag. Cannot use on section views
// (NtMapViewOfSectionEx regions). PageProtection is ignored but must still
// be set to a valid value (e.g. PAGE_NOACCESS).
inline constexpr DWORD MEM_RESET = 0x00080000;
// Allocates at the highest available address instead of the lowest.
// Can be slower than regular allocations when many allocations exist.
inline constexpr DWORD MEM_TOP_DOWN = 0x00100000;
// Enables write-watch tracking on NtAllocateVirtualMemoryEx regions.
// NtGetWriteWatch retrieves pages written since allocation or last reset.
// Must combine with MEM_RESERVE; cannot combine with MEM_LARGE_PAGES.
inline constexpr DWORD MEM_WRITE_WATCH = 0x00200000;
// Reserves an address range for Address Windowing Extensions (AWE).
// Must combine with MEM_RESERVE. Must not combine with MEM_COMMIT.
// PageProtection must be PAGE_READWRITE.
inline constexpr DWORD MEM_PHYSICAL = 0x00400000;
// Allocates using large pages. Size and alignment must be a multiple of the
// large-page minimum. Must combine with MEM_RESERVE | MEM_COMMIT.
// Caller must hold SeLockMemoryPrivilege.
inline constexpr DWORD MEM_LARGE_PAGES = 0x20000000;
// Hint to use 64K physically-contiguous pages (MEM_LARGE_PAGES|MEM_PHYSICAL).
// BaseAddress and Size must both be multiples of 64K (BaseAddress may be NULL).
// Falls back to non-contiguous small pages if physical memory is fragmented,
// unless combined with MEM_EXTENDED_PARAMETER_NONPAGED (then it fails).
inline constexpr DWORD MEM_64K_PAGES = 0x20400000;
// Reverses MEM_RESET — attempts to recover discarded page contents.
// Must only be called on ranges previously passed to MEM_RESET; behavior is
// undefined otherwise. Must not combine with any other flag. PageProtection
// is ignored but must still be set to a valid value.
// Same value as MEM_IMAGE (0x01000000). Not a conflict: MEM_IMAGE is a
// region type returned by queries, MEM_RESET_UNDO is an allocation flag.
inline constexpr DWORD MEM_RESET_UNDO = 0x01000000;
// NtFreeVirtualMemory flag: coalesces adjacent placeholder regions into one.
// Must combine with MEM_RELEASE.
inline constexpr DWORD MEM_COALESCE_PLACEHOLDERS = 0x00000001;

// Placeholder flags (Win10 1803+) — for virtual memory ring buffers, etc.
// Placeholders are reserved regions that can be replaced by private allocations
// (NtAllocateVirtualMemoryEx) or section views (NtMapViewOfSectionEx), then
// restored via NtFreeVirtualMemory or NtUnmapViewOfSectionEx respectively.
//
// Creates a placeholder reservation via NtAllocateVirtualMemoryEx.
// Must combine with MEM_RESERVE. PageProtection must be PAGE_NOACCESS.
inline constexpr DWORD MEM_RESERVE_PLACEHOLDER = 0x00040000;
// Replaces a placeholder with a normal private allocation
// (NtAllocateVirtualMemoryEx) or section view (NtMapViewOfSectionEx).
// BaseAddress and Size must exactly match the placeholder's. Any
// MEM_ADDRESS_REQUIREMENTS in ExtendedParameters must be all zeroes.
// Section-specific: only data/pagefile-backed sections (no images, no physical
// memory). 64K alignment on Offset/BaseAddress is relaxed to page alignment.
inline constexpr DWORD MEM_REPLACE_PLACEHOLDER = 0x00004000;
// NtFreeVirtualMemory: splits a placeholder at RegionSize, preserving the
// remainder as a new placeholder. Must combine with MEM_RELEASE.
// Same value as MEM_PRESERVE_PLACEHOLDER_ON_UNMAP (0x02) but different
// context: this splits/preserves private placeholders, while the unmap variant
// converts a section view back to a placeholder.
inline constexpr DWORD MEM_PRESERVE_PLACEHOLDER = 0x00000002;

// Memory protection — base constants (mutually exclusive, pick exactly one)
// Used by NtAllocateVirtualMemoryEx, NtProtectVirtualMemory,
// NtCreateSectionEx, NtMapViewOfSectionEx.
//
// Disables all access; any read/write/execute faults.
// Cannot use with PAGE_GUARD. Not valid for NtCreateSectionEx.
inline constexpr DWORD PAGE_NOACCESS = 0x01;
// Read-only; writes and execution (with DEP) fault.
inline constexpr DWORD PAGE_READONLY = 0x02;
// Read/write; execution (with DEP) faults.
inline constexpr DWORD PAGE_READWRITE = 0x04;
// Copy-on-write for section views; write creates a private copy (promoted to
// PAGE_READWRITE). Not valid for NtAllocateVirtualMemoryEx — sections only.
inline constexpr DWORD PAGE_WRITECOPY = 0x08;
// Execute-only; reads and writes fault. Not valid for NtCreateSectionEx.
inline constexpr DWORD PAGE_EXECUTE = 0x10;
// Execute or read; writes fault.
inline constexpr DWORD PAGE_EXECUTE_READ = 0x20;
// Execute, read, or write.
inline constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
// Execute, read, or copy-on-write for section views (promoted to
// PAGE_EXECUTE_READWRITE on write). Not valid for NtAllocateVirtualMemoryEx.
inline constexpr DWORD PAGE_EXECUTE_WRITECOPY = 0x80;

// Memory protection — modifier flags (OR with a base constant)
// One-shot access alarm; first access raises STATUS_GUARD_PAGE_VIOLATION
// and clears the guard bit, leaving the underlying protection.
// Cannot combine with PAGE_NOACCESS, PAGE_NOCACHE, or PAGE_WRITECOMBINE.
// Not valid for NtCreateSectionEx.
inline constexpr DWORD PAGE_GUARD = 0x100;
// Disables CPU caching; for device memory only. Cannot combine with
// PAGE_GUARD, PAGE_NOACCESS, or PAGE_WRITECOMBINE. Only valid for
// NtAllocateVirtualMemoryEx private memory (use SEC_NOCACHE for sections).
inline constexpr DWORD PAGE_NOCACHE = 0x200;
// Enables write-combining; for device frame-buffer memory. Cannot combine
// with PAGE_GUARD, PAGE_NOACCESS, or PAGE_NOCACHE. Only valid for
// NtAllocateVirtualMemoryEx private memory (use SEC_WRITECOMBINE for sections).
inline constexpr DWORD PAGE_WRITECOMBINE = 0x400;

// Memory protection — CFG (Control Flow Guard) flags
// Marks all locations as invalid CFG call targets. Must combine with an
// execute protection (PAGE_EXECUTE*). Only valid for NtAllocateVirtualMemoryEx,
// not NtProtectVirtualMemory or NtCreateSection.
// Same value as PAGE_TARGETS_NO_UPDATE (context-dependent).
inline constexpr DWORD PAGE_TARGETS_INVALID = 0x40000000;
// Preserves existing CFG info during NtProtectVirtualMemory protection changes.
// Only valid when changing to an execute protection (PAGE_EXECUTE*).
inline constexpr DWORD PAGE_TARGETS_NO_UPDATE = 0x40000000;

// Protection modifier for NtProtectVirtualMemory on copy-on-write file views.
// Discards private COW modifications and reverts pages to the original
// file-backed content. Used by msync(MS_INVALIDATE) on MAP_PRIVATE mappings.
// Shares value 0x80000000 with PAGE_ENCLAVE_THREAD_CONTROL (mutually exclusive
// contexts: file mapping vs. SGX enclave).
inline constexpr DWORD PAGE_REVERT_TO_FILE_MAP = 0x80000000;

// Section allocation attributes (NtCreateSection / NtCreateSectionEx)
// These control how the section's pages are backed and mapped.
//
// Pages are committed on creation (backed by file or pagefile immediately).
// Default for pagefile-backed sections. Cannot combine with SEC_RESERVE.
inline constexpr DWORD SEC_COMMIT = 0x08000000;
// Section is a PE image. The system parses the image and sets per-page
// protections from the PE headers. Cannot combine with SEC_COMMIT/SEC_RESERVE.
// MaximumSize is ignored (derived from the image). FileHandle required.
inline constexpr DWORD SEC_IMAGE = 0x01000000;
// Image section without execute permission (SEC_IMAGE | SEC_NOCACHE).
inline constexpr DWORD SEC_IMAGE_NO_EXECUTE = 0x11000000;
// Section backed by large pages. Must combine with SEC_COMMIT.
// MaximumSize must be a multiple of the large-page minimum.
// SectionPageProtection must be PAGE_READWRITE.
// Caller must hold SeLockMemoryPrivilege.
inline constexpr DWORD SEC_LARGE_PAGES = 0x80000000;
// Shared-memory equivalent of PAGE_NOCACHE. All views of this section
// are non-cacheable. For private memory use PAGE_NOCACHE instead.
inline constexpr DWORD SEC_NOCACHE = 0x10000000;
// Pages start reserved; committed on demand when views are mapped with
// MEM_COMMIT. Cannot combine with SEC_COMMIT.
inline constexpr DWORD SEC_RESERVE = 0x04000000;
// Shared-memory equivalent of PAGE_WRITECOMBINE. All views of this section
// use write-combining. For private memory use PAGE_WRITECOMBINE instead.
inline constexpr DWORD SEC_WRITECOMBINE = 0x40000000;
// Section backed by 1GB huge pages. Must combine with SEC_COMMIT.
inline constexpr DWORD SEC_HUGE_PAGES = 0x00020000;
// Hint to use 64K contiguous pages.
inline constexpr DWORD SEC_64K_PAGES = 0x00080000;
// Legacy: section is mapped at a fixed base address across processes.
inline constexpr DWORD SEC_BASED = 0x00200000;
// Protection on mapped views cannot be changed after creation.
inline constexpr DWORD SEC_NO_CHANGE = 0x00400000;
// Explicitly file-backed (default when FileHandle is provided).
inline constexpr DWORD SEC_FILE = 0x00800000;
// Section is visible across all terminal server sessions.
inline constexpr DWORD SEC_GLOBAL = 0x20000000;

// Section access rights (NtCreateSection / NtCreateSectionEx DesiredAccess)
// Query section attributes (NtQuerySection). Always set this.
inline constexpr ULONG SECTION_QUERY = 0x0001;
// Map writable views.
inline constexpr ULONG SECTION_MAP_WRITE = 0x0002;
// Map readable views.
inline constexpr ULONG SECTION_MAP_READ = 0x0004;
// Map executable views.
inline constexpr ULONG SECTION_MAP_EXECUTE = 0x0008;
// Grow the section via NtExtendSection.
inline constexpr ULONG SECTION_EXTEND_SIZE = 0x0010;
// Map executable views with explicit execute (CFG-aware).
inline constexpr ULONG SECTION_MAP_EXECUTE_EXPLICIT = 0x0020;
// All section rights combined with STANDARD_RIGHTS_REQUIRED.
inline constexpr ULONG SECTION_ALL_ACCESS = 0x000F001F;

//===----------------------------------------------------------------------===//
// NT Memory Structures
//===----------------------------------------------------------------------===//

// MEMORY_INFORMATION_CLASS for NtQueryVirtualMemory
// Reference: System Informer phnt/include/ntmmapi.h
enum MEMORY_INFORMATION_CLASS {
  MemoryBasicInformation = 0,          // q: MEMORY_BASIC_INFORMATION
  MemoryWorkingSetInformation = 1,     // q: MEMORY_WORKING_SET_INFORMATION
  MemoryMappedFilenameInformation = 2, // q: UNICODE_STRING (backing file path)
  MemoryRegionInformation = 3,         // q: MEMORY_REGION_INFORMATION (raw RegionType)
  MemoryWorkingSetExInformation = 4,   // q: MEMORY_WORKING_SET_EX_INFORMATION (Vista+)
  MemorySharedCommitInformation = 5,   // q: MEMORY_SHARED_COMMIT_INFORMATION (Win8+)
  MemoryImageInformation = 6,          // q: MEMORY_IMAGE_INFORMATION
  MemoryRegionInformationEx = 7,       // q: MEMORY_REGION_INFORMATION (bitfield RegionType)
  MemoryPrivilegedBasicInformation = 8, // q: MEMORY_BASIC_INFORMATION (privileged)
  MemoryEnclaveImageInformation = 9,   // q: MEMORY_ENCLAVE_IMAGE_INFORMATION (RS3+)
  MemoryBasicInformationCapped = 10,   // q: (undocumented)
  MemoryPhysicalContiguityInformation = 11, // q: MEMORY_PHYSICAL_CONTIGUITY_INFORMATION (20H1+)
  MemoryBadInformation = 12,           // q: MEMORY_BAD_INFORMATION (Win11+)
  MemoryBadInformationAllProcesses = 13, // not implemented (22H1+)
  MemoryImageExtensionInformation = 14, // q: MEMORY_IMAGE_EXTENSION_INFORMATION (24H2+)
};

// MEMORY_REGION_INFORMATION - extended region info.
// Must query with MemoryRegionInformationEx (class 7), not
// MemoryRegionInformation (class 3). Class 3 returns raw allocation type
// constants (MEM_PRIVATE, etc.) in RegionType instead of the bitfield
// encoding, so PlaceholderReservation and other flags are always zero.
// Reference: System Informer phnt/include/ntmmapi.h
struct MEMORY_REGION_INFORMATION {
  PVOID AllocationBase;       // Base of the containing allocation
  ULONG AllocationProtect;    // PAGE_* protection at allocation time
  union {
    ULONG RegionType;
    struct {
      ULONG Private : 1;             // NtAllocateVirtualMemoryEx private memory
      ULONG MappedDataFile : 1;      // NtMapViewOfSectionEx data file view
      ULONG MappedImage : 1;         // NtMapViewOfSectionEx image (SEC_IMAGE) view
      ULONG MappedPageFile : 1;      // NtMapViewOfSectionEx pagefile-backed section
      ULONG MappedPhysical : 1;      // \Device\PhysicalMemory section view
      ULONG DirectMapped : 1;        // Direct-mapped file
      ULONG SoftwareEnclave : 1;     // SGX enclave (RS3+)
      ULONG PageSize64K : 1;         // 64KB pages (MEM_64K_PAGES)
      ULONG PlaceholderReservation : 1; // Placeholder (RS4+ / Win10 1803+)
      ULONG MappedAwe : 1;           // AWE mapping (MEM_PHYSICAL) (21H1+)
      ULONG MappedWriteWatch : 1;    // Write-watch (MEM_WRITE_WATCH)
      ULONG PageSizeLarge : 1;       // Large pages (MEM_LARGE_PAGES, 2MB/4MB)
      ULONG PageSizeHuge : 1;        // Huge pages (SEC_HUGE_PAGES, 1GB)
      ULONG Reserved : 19;
    };
  };
  SIZE_T RegionSize;          // Combined size of pages in this region
  SIZE_T CommitSize;          // Commit charge for the allocation
  ULONG_PTR PartitionId;     // Memory partition ID (19H1+)
  ULONG_PTR NodePreference;  // Preferred NUMA node (20H1+)
};

// MEMORY_SHARED_COMMIT_INFORMATION for MemorySharedCommitInformation (Win8+)
// Total commit charge for shared memory in the queried region.
struct MEMORY_SHARED_COMMIT_INFORMATION {
  SIZE_T CommitSize;
};

// MEMORY_IMAGE_INFORMATION for MemoryImageInformation
struct MEMORY_IMAGE_INFORMATION {
  PVOID ImageBase;
  SIZE_T SizeOfImage;
  union {
    ULONG ImageFlags;
    struct {
      ULONG ImagePartialMap : 1;    // Partial mapping
      ULONG ImageNotExecutable : 1; // Non-executable image
      ULONG ImageSigningLevel : 4;  // Signing level (RS3+)
      ULONG ImageExtensionPresent : 1; // Extension present (24H2+)
      ULONG Reserved : 25;
    };
  };
};

// MEMORY_ENCLAVE_IMAGE_INFORMATION for MemoryEnclaveImageInformation (RS3+)
struct MEMORY_ENCLAVE_IMAGE_INFORMATION {
  MEMORY_IMAGE_INFORMATION ImageInfo;
  unsigned char UniqueID[32];  // Enclave image unique identifier
  unsigned char AuthorID[32];  // Enclave author/creator identifier
};

// MEMORY_IMAGE_EXTENSION_TYPE for MemoryImageExtensionInformation (24H2+)
enum MEMORY_IMAGE_EXTENSION_TYPE : ULONG {
  MemoryImageExtensionCfgScp = 0,          // CFG/SCP extension
  MemoryImageExtensionCfgEmulatedScp = 1,  // Emulated CFG/SCP extension
  MemoryImageExtensionTypeMax = 2,
};

// MEMORY_IMAGE_EXTENSION_INFORMATION for MemoryImageExtensionInformation (24H2+)
// Describes optional image extension metadata (e.g. CFG/SCP).
struct MEMORY_IMAGE_EXTENSION_INFORMATION {
  MEMORY_IMAGE_EXTENSION_TYPE ExtensionType;
  ULONG Flags;
  PVOID ExtensionImageBaseRva; // RVA of the extension image base
  SIZE_T ExtensionSize;        // Size in bytes of the extension region
};

// MEMORY_BAD_INFORMATION for MemoryBadInformation (Win11+)
// Reports a range of memory marked as bad or problematic.
struct MEMORY_BAD_INFORMATION {
  PVOID BadAddress;
  ULONG_PTR Length;
  ULONG Flags;
  ULONG Reserved;
};

// MEMORY_WORKING_SET_BLOCK - per-page working set info (class 1)
// VirtualPage is the page address shifted right by PAGE_SHIFT.
struct MEMORY_WORKING_SET_BLOCK {
  ULONG_PTR Protection : 5;   // Page protection attributes
  ULONG_PTR ShareCount : 3;   // Number of sharing processes (max 7)
  ULONG_PTR Shared : 1;       // Page is sharable
  ULONG_PTR Node : 3;         // NUMA node
  ULONG_PTR VirtualPage : 52; // Page address >> PAGE_SHIFT (x64)
};

// MEMORY_WORKING_SET_BLOCK::Protection values (5-bit encoded)
// Layout: [4]=guard, [3]=nocache, [2:0]=base protection
inline constexpr ULONG_PTR MEMORY_BLOCK_NOT_ACCESSED = 0;
inline constexpr ULONG_PTR MEMORY_BLOCK_READONLY = 1;
inline constexpr ULONG_PTR MEMORY_BLOCK_EXECUTABLE = 2;
inline constexpr ULONG_PTR MEMORY_BLOCK_EXECUTABLE_READONLY = 3;
inline constexpr ULONG_PTR MEMORY_BLOCK_READWRITE = 4;
inline constexpr ULONG_PTR MEMORY_BLOCK_COPYONWRITE = 5;
inline constexpr ULONG_PTR MEMORY_BLOCK_EXECUTABLE_READWRITE = 6;
inline constexpr ULONG_PTR MEMORY_BLOCK_EXECUTABLE_COPYONWRITE = 7;
inline constexpr ULONG_PTR MEMORY_BLOCK_NOT_ACCESSED_2 = 8;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_READONLY = 9;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_EXECUTABLE = 10;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_EXECUTABLE_READONLY = 11;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_READWRITE = 12;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_COPYONWRITE = 13;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_EXECUTABLE_READWRITE = 14;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_EXECUTABLE_COPYONWRITE = 15;
inline constexpr ULONG_PTR MEMORY_BLOCK_NOT_ACCESSED_3 = 16;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_READONLY = 17;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_EXECUTABLE = 18;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_EXECUTABLE_READONLY = 19;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_READWRITE = 20;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_COPYONWRITE = 21;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_EXECUTABLE_READWRITE = 22;
inline constexpr ULONG_PTR MEMORY_BLOCK_GUARD_EXECUTABLE_COPYONWRITE = 23;
inline constexpr ULONG_PTR MEMORY_BLOCK_NOT_ACCESSED_4 = 24;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_READONLY = 25;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_EXECUTABLE = 26;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_EXECUTABLE_READONLY = 27;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_READWRITE = 28;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_COPYONWRITE = 29;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_EXECUTABLE_READWRITE = 30;
inline constexpr ULONG_PTR MEMORY_BLOCK_NON_CACHEABLE_GUARD_EXECUTABLE_COPYONWRITE = 31;

// MEMORY_WORKING_SET_INFORMATION for MemoryWorkingSetInformation (class 1)
// Variable-length: header followed by NumberOfEntries working set blocks.
struct MEMORY_WORKING_SET_INFORMATION {
  ULONG_PTR NumberOfEntries;
  MEMORY_WORKING_SET_BLOCK WorkingSetInfo[1]; // Variable-length array
};

// MEMORY_WORKING_SET_EX_BLOCK - per-page extended attributes (class 4)
// Reference: System Informer phnt/include/ntmmapi.h
union MEMORY_WORKING_SET_EX_BLOCK {
  ULONG_PTR Flags;
  // Valid page structure (when Valid bit = 1)
  struct {
    ULONG_PTR Valid : 1;            // If 1, page is in working set (resident)
    ULONG_PTR ShareCount : 3;       // Number of processes sharing
    ULONG_PTR Win32Protection : 11; // PAGE_* protection
    ULONG_PTR Shared : 1;           // Page can be shared
    ULONG_PTR Node : 6;             // NUMA node (max 63)
    ULONG_PTR Locked : 1;           // Page is VirtualLock'd
    ULONG_PTR LargePage : 1;        // Large page
    ULONG_PTR Priority : 3;         // Memory priority
    ULONG_PTR Reserved : 3;
    ULONG_PTR SharedOriginal : 1;   // Unmodified
    ULONG_PTR Bad : 1;              // Bad page
    ULONG_PTR Win32GraphicsProtection : 4; // Since 19H1
    ULONG_PTR ReservedUlong : 28;
  };
  // Invalid page structure (when Valid bit = 0)
  struct {
    ULONG_PTR Valid_ : 1;           // Always 0 in this variant
    ULONG_PTR Reserved0 : 14;
    ULONG_PTR Shared_ : 1;
    ULONG_PTR Reserved1 : 5;
    ULONG_PTR PageTable : 1;        // Page table page
    ULONG_PTR Location : 2;         // Page location
    ULONG_PTR Priority_ : 3;
    ULONG_PTR ModifiedList : 1;     // On modified list
    ULONG_PTR Reserved2 : 2;
    ULONG_PTR SharedOriginal_ : 1;
    ULONG_PTR Bad_ : 1;
    ULONG_PTR ReservedUlong_ : 32;
  } Invalid;
};

// MEMORY_WORKING_SET_EX_BLOCK Invalid.Location values
// When Valid=0, Location indicates where the page currently resides.
// A page not in any working set is a "transition page" — it stays cached
// in RAM (standby/modified lists) until repurposed or re-faulted.
// Accessing a transition page causes a soft fault (no disk I/O).
// Accessing a paged-out page causes a hard fault (reads from backing store).
inline constexpr ULONG_PTR MemoryLocationInvalid = 0;  // Paged out or never faulted (hard fault)
inline constexpr ULONG_PTR MemoryLocationResident = 1;  // Transition page in RAM (soft fault)
inline constexpr ULONG_PTR MemoryLocationPagefile = 2;  // In pagefile (hard fault)
inline constexpr ULONG_PTR MemoryLocationReserved = 3;

// MEMORY_WORKING_SET_EX_INFORMATION - for MemoryWorkingSetExInformation
struct MEMORY_WORKING_SET_EX_INFORMATION {
  PVOID VirtualAddress;                        // [in] Address to query
  MEMORY_WORKING_SET_EX_BLOCK VirtualAttributes; // [out] Page info
};

// NtLockVirtualMemory/NtUnlockVirtualMemory MapType flags
// Reference: phnt/include/ntmmapi.h (MAP_PROCESS, MAP_SYSTEM)
//
// All pages in the region must be committed. PAGE_NOACCESS pages cannot be
// locked. Locked pages remain in physical memory until explicitly unlocked or
// process termination — no lock count, a single NtUnlockVirtualMemory undoes it.
//
// Lock to process working set. Max lockable pages = minimum working set size
// minus overhead. Call NtSetInformationProcess to raise the working set first
// if locking large regions.
inline constexpr ULONG MAP_PROCESS = 0x0001;
// Lock to physical memory (kernel-pinned, non-pageable). Caller must hold
// SeLockMemoryPrivilege.
inline constexpr ULONG MAP_SYSTEM = 0x0002;

// NTPSS_MEMORY_BULK_INFORMATION — header for NtPssCaptureVaSpaceBulk.
// Returns MEMORY_BASIC_INFORMATION entries in bulk, replacing iterative
// NtQueryVirtualMemory loops. Win10 20H1+ (Build 19041).
//
// Empirically confirmed layout: 16-byte header followed immediately by
// MEMORY_BASIC_INFORMATION entries at sizeof(NTPSS_MEMORY_BULK_INFORMATION).
//
// BaseAddress parameter: start enumeration from any VA (nullptr = process
// start). Combined with NextValidAddress output, enables pagination through
// arbitrary ranges — not limited to full-process scans.
//
// NextValidAddress: resume cursor. Enumeration is complete when this
// value >= the highest user-mode address (typically 0x7FFFFFFF0000).
//
// On partial fill, returns STATUS_MORE_ENTRIES (0x00000105 — NT_SUCCESS
// positive) with NumberOfEntries set to the entries that fit and
// NextValidAddress set to resume from. Buffers smaller than the 16-byte
// header return STATUS_INFO_LENGTH_MISMATCH; the smallest useful buffer
// is 64 bytes (header + one MBI slot). The kernel truncates at MBI
// boundaries — trailing bytes that don't form a complete slot are
// ignored.
//
// QueryFlags: must be non-zero. MEMORY_BULK_INFORMATION_FLAG_BASIC
// (0x1) is the only documented value. The RESERVED bit (0x2) is
// validated by the kernel against VALID_MASK (0x3) and accepted but
// currently produces identical output. Values outside the mask return
// STATUS_NOT_SUPPORTED (0xC00000BB); QueryFlags == 0 returns
// STATUS_INVALID_PARAMETER.
//
// Reference: System Informer phnt/include/ntpsapi.h
inline constexpr ULONG MEMORY_BULK_INFORMATION_FLAG_BASIC = 0x1;

struct NTPSS_MEMORY_BULK_INFORMATION {
  ULONG QueryFlags;              // [in]  MEMORY_BULK_INFORMATION_FLAG_BASIC
  ULONG NumberOfEntries;         // [out] MBI entries in this response
  PVOID NextValidAddress;        // [out] resume cursor for next call
};

//===----------------------------------------------------------------------===//
// Virtual Memory Information Classes (NtSetInformationVirtualMemory)
// Reference: System Informer phnt/include/ntmmapi.h
//===----------------------------------------------------------------------===//

enum VIRTUAL_MEMORY_INFORMATION_CLASS {
  VmPrefetchInformation = 0,               // s: MEMORY_PREFETCH_INFORMATION (Win8+)
  VmPagePriorityInformation = 1,           // s: MEMORY_PAGE_PRIORITY_INFORMATION (Win8+)
  VmCfgCallTargetInformation = 2,          // s: CFG_CALL_TARGET_LIST_INFORMATION (RS2+)
  VmPageDirtyStateInformation = 3,         // s: MEMORY_PAGE_DIRTY_STATE_INFORMATION (RS3+)
  VmImageHotPatchInformation = 4,          // s: (19H1+)
  VmPhysicalContiguityInformation = 5,     // s: MEMORY_PHYSICAL_CONTIGUITY_INFORMATION (20H1+, SeLockMemoryPrivilege)
  VmVirtualMachinePrepopulateInformation = 6, // s: VM prepopulate
  VmRemoveFromWorkingSetInformation = 7,   // s: MEMORY_REMOVE_WORKING_SET_INFORMATION
  MaxVmInfoClass = 8
};

// MEMORY_RANGE_ENTRY - describes a contiguous virtual address region
// Used by NtSetInformationVirtualMemory for batched operations
struct MEMORY_RANGE_ENTRY {
  PVOID VirtualAddress;
  SIZE_T NumberOfBytes;
};

// MEMORY_PREFETCH_INFORMATION for VmPrefetchInformation
// Issues large concurrent I/Os to bring paged-out ranges into physical memory.
// Pages are cached but NOT added to the working set until actually accessed.
struct MEMORY_PREFETCH_INFORMATION {
  ULONG Flags;
};

// Prefetch flag: populate pages directly into working set (not just cache).
// Undocumented; since 24H4. Official PrefetchVirtualMemory docs state Flags
// must be 0. All call sites fall back to Flags=0 on failure for safety.
inline constexpr ULONG VM_PREFETCH_TO_WORKING_SET = 0x00000001;

// MEMORY_PAGE_PRIORITY_INFORMATION for VmPagePriorityInformation
// Sets memory priority (affects eviction order under memory pressure)
struct MEMORY_PAGE_PRIORITY_INFORMATION {
  ULONG PagePriority;
};

// Page priority levels (0 = lowest, evicted first under memory pressure)
// Reference: System Informer phnt/include/ntmmapi.h
// Probed on Windows 11 26200 (RA15): values 0-5 accepted, 6-7 rejected
// with STATUS_INVALID_PARAMETER_4. Kernel user-mode max is NORMAL (5).
inline constexpr ULONG MEMORY_PRIORITY_LOWEST = 0;
inline constexpr ULONG MEMORY_PRIORITY_VERY_LOW = 1;
inline constexpr ULONG MEMORY_PRIORITY_LOW = 2;
inline constexpr ULONG MEMORY_PRIORITY_MEDIUM = 3;
inline constexpr ULONG MEMORY_PRIORITY_BELOW_NORMAL = 4;
inline constexpr ULONG MEMORY_PRIORITY_NORMAL = 5;
// Values 6-7 are rejected by NtSetInformationVirtualMemory on Build 26200.
// They may be kernel-mode only or reserved for future use.
// inline constexpr ULONG MEMORY_PRIORITY_ABOVE_NORMAL = 6; // REJECTED
// inline constexpr ULONG MEMORY_PRIORITY_HIGH = 7;          // REJECTED

// MEMORY_PAGE_DIRTY_STATE_INFORMATION for VmPageDirtyStateInformation
// Probed on Windows 11 26200: the kernel validates struct size (must be 4)
// and rejects Flags=0 with STATUS_NOT_SUPPORTED (0xC00000BB). Non-zero
// Flags values return undocumented 0xC00000F3 on all memory types (private,
// section views, write-watch, WRITECOPY). Likely gated behind a Hyper-V
// VM worker process context — not usable for general-purpose POSIX memory.
struct MEMORY_PAGE_DIRTY_STATE_INFORMATION {
  ULONG Flags;
};

// MEMORY_REMOVE_WORKING_SET_INFORMATION for VmRemoveFromWorkingSetInformation
// Evicts pages from the process working set (forces them to standby/modified).
struct MEMORY_REMOVE_WORKING_SET_INFORMATION {
  ULONG Flags;
};

// MEMORY_PHYSICAL_CONTIGUITY_UNIT_STATE for VmPhysicalContiguityInformation
enum MEMORY_PHYSICAL_CONTIGUITY_UNIT_STATE : ULONG {
  MemoryNotContiguous = 0,
  MemoryAlignedAndContiguous = 1,
  MemoryNotResident = 2,
  MemoryNotEligibleToMakeContiguous = 3,
  MemoryContiguityStateMax = 4,
};

// Per-unit contiguity state for MEMORY_PHYSICAL_CONTIGUITY_INFORMATION.
struct MEMORY_PHYSICAL_CONTIGUITY_UNIT_INFORMATION {
  union {
    ULONG AllInformation;
    struct {
      ULONG State : 2;    // MEMORY_PHYSICAL_CONTIGUITY_UNIT_STATE
      ULONG Reserved : 30;
    };
  };
};

// MEMORY_PHYSICAL_CONTIGUITY_INFORMATION for VmPhysicalContiguityInformation
// Queries whether physical pages backing a VA range are contiguous.
// Requires SeLockMemoryPrivilege. (20H1+)
struct MEMORY_PHYSICAL_CONTIGUITY_INFORMATION {
  PVOID VirtualAddress;
  ULONG_PTR Size;
  ULONG_PTR ContiguityUnitSize;
  ULONG Flags;
  MEMORY_PHYSICAL_CONTIGUITY_UNIT_INFORMATION *ContiguityUnitInformation;
};

// CFG_CALL_TARGET_INFO for Control Flow Guard validation
struct CFG_CALL_TARGET_INFO {
  ULONG_PTR Offset;
  ULONG_PTR Flags;
};

// CFG call target flags
inline constexpr ULONG_PTR CFG_CALL_TARGET_VALID = 0x00000001;
inline constexpr ULONG_PTR CFG_CALL_TARGET_PROCESSED = 0x00000002;
inline constexpr ULONG_PTR CFG_CALL_TARGET_CONVERT_EXPORT_SUPPRESSED_TO_VALID = 0x00000004;

// CFG_CALL_TARGET_LIST_INFORMATION for VmCfgCallTargetInformation (RS2+)
struct CFG_CALL_TARGET_LIST_INFORMATION {
  ULONG NumberOfEntries;
  ULONG Reserved;
  ULONG *NumberOfEntriesProcessed;
  CFG_CALL_TARGET_INFO *CallTargetInfo;
  PVOID Section;        // RS5+ only
  ULARGE_INTEGER FileOffset; // RS5+ only
};

//===----------------------------------------------------------------------===//
// Memory Partition Constants (Win10+)
//===----------------------------------------------------------------------===//

// Special partition handles
#define MEMORY_CURRENT_PARTITION_HANDLE ((HANDLE)(LONG_PTR)-1)
#define MEMORY_SYSTEM_PARTITION_HANDLE ((HANDLE)(LONG_PTR)-2)
#define MEMORY_EXISTING_VAD_PARTITION_HANDLE ((HANDLE)(LONG_PTR)-3)

// Partition information classes
enum PARTITION_INFORMATION_CLASS {
  SystemMemoryPartitionInformation = 0,
  SystemMemoryPartitionMoveMemory = 1,
  SystemMemoryPartitionAddPagefile = 2,
  SystemMemoryPartitionCombineMemory = 3,
  SystemMemoryPartitionInitialAddMemory = 4,
  SystemMemoryPartitionGetMemoryEvents = 5,
  SystemMemoryPartitionSetAttributes = 6,
  SystemMemoryPartitionNodeInformation = 7,
  SystemMemoryPartitionCreateLargePages = 8,
  SystemMemoryPartitionDedicatedMemoryInformation = 9,
  SystemMemoryPartitionOpenDedicatedMemory = 10,
  SystemMemoryPartitionMemoryChargeAttributes = 11,
  SystemMemoryPartitionClearAttributes = 12,
  SystemMemoryPartitionSetMemoryThresholds = 13,
  SystemMemoryPartitionMemoryListCommand = 14,
  SystemMemoryPartitionMax = 15
};


//===----------------------------------------------------------------------===//
// Unmap flags for NtUnmapViewOfSectionEx
//===----------------------------------------------------------------------===//

// No special unmap behavior.
inline constexpr ULONG MEM_UNMAP_NONE = 0x00000000;
// Temporarily boost page priority on unmap — hints that another thread will
// access these pages shortly. Reduces re-fault cost for producer/consumer
// patterns on shared sections.
inline constexpr ULONG MEM_UNMAP_WITH_TRANSIENT_BOOST = 0x00000001;
// Unmaps the section view back to a placeholder. The region reverts to a
// reserved placeholder that can be re-filled via NtMapViewOfSectionEx with
// MEM_REPLACE_PLACEHOLDER or NtAllocateVirtualMemoryEx. BaseAddress must be
// the exact value from the original NtMapViewOfSectionEx that replaced the
// placeholder. Same value as MEM_PRESERVE_PLACEHOLDER (0x02) but different
// context: this converts a section view back to a placeholder, while the
// NtFreeVirtualMemory variant splits/preserves private placeholders.
inline constexpr ULONG MEM_PRESERVE_PLACEHOLDER_ON_UNMAP = 0x00000002;

// NtMapViewOfSectionEx AllocationType flag: allows mapping a SEC_IMAGE section
// at an address that differs from the image's preferred ImageBase. Without this
// flag, NtMapViewOfSectionEx rejects SEC_IMAGE at non-preferred addresses. The
// kernel applies base relocations as if ASLR randomized the load address.
// Used by self-hollowing exec to remap a new target at the old EXE's base.
inline constexpr ULONG MEM_DIFFERENT_IMAGE_BASE_OK = 0x00800000;


//===----------------------------------------------------------------------===//
// Extended Virtual Memory Types (Win10 RS5+ / Win8+)
//===----------------------------------------------------------------------===//

// Extended parameter types for NtAllocateVirtualMemoryEx, NtMapViewOfSectionEx,
// NtCreateSectionEx. Multiple parameters with different Types can be passed in
// a single call as an array — this is the extensibility mechanism that replaces
// adding new flags or syscall variants.
enum MEM_EXTENDED_PARAMETER_TYPE {
  MemExtendedParameterInvalidType = 0,
  // Pointer → MEM_ADDRESS_REQUIREMENTS: constrain VA range and alignment.
  MemExtendedParameterAddressRequirements = 1,
  // ULong → preferred NUMA node for physical page allocation.
  MemExtendedParameterNumaNode = 2,
  // Handle → target a specific memory partition.
  MemExtendedParameterPartitionHandle = 3,
  // Handle → AWE physical memory handle.
  MemExtendedParameterUserPhysicalHandle = 4,
  // ULong64 → attribute flags (MEM_EXTENDED_PARAMETER_NONPAGED, etc.).
  MemExtendedParameterAttributeFlags = 5,
  // ULong → machine type for image sections (IMAGE_FILE_MACHINE_*).
  MemExtendedParameterImageMachine = 6,
  MemExtendedParameterMax = 7
};

struct MEM_ADDRESS_REQUIREMENTS {
  // Specifies the lowest acceptable address.
  // This address must be a multiple of the allocation granularity returned by GetSystemInfo, or a multiple of the large page size returned by GetLargePageMinimum if large pages are being requested.
  // If this member is NULL, then there is no lower limit.
  PVOID LowestStartingAddress;
  // Specifies the highest acceptable address (inclusive).
  // This address must not exceed lpMaximumApplicationAddress and must be one less than a multiple of the allocation granularity returned by GetSystemInfo.
  // If this member is NULL, then there is no upper limit.
  PVOID HighestEndingAddress;
  // Specifies power-of-2 alignment.
  // Specifying 0 aligns the returned address on the system allocation granularity.
  // If nonzero, this value must be greater than or equal to the system allocation granularity.
  SIZE_T Alignment;
}; // Specifying a MEM_ADDRESS_REQUIREMENTS structure with all fields set to 0 is the same as not specifying one at all.

// Each element describes one parameter; Type selects which union member is
// active and how the value is interpreted.
struct MEM_EXTENDED_PARAMETER {
  struct {
    DWORD64 Type : 8;     // MEM_EXTENDED_PARAMETER_TYPE
    DWORD64 Reserved : 56;
  };
  union {
    // NumaNode → ULong; AttributeFlags → ULong64:
    //   MEM_EXTENDED_PARAMETER_NONPAGED       = 0x02
    //   MEM_EXTENDED_PARAMETER_NONPAGED_LARGE  = 0x08
    //   MEM_EXTENDED_PARAMETER_NONPAGED_HUGE   = 0x10
    //   MEM_EXTENDED_PARAMETER_EC_CODE         = 0x40
    DWORD64 ULong64;
    PVOID Pointer;  // AddressRequirements → MEM_ADDRESS_REQUIREMENTS*
    SIZE_T Size;
    HANDLE Handle;  // PartitionHandle, UserPhysicalHandle
    DWORD ULong;    // NumaNode, ImageMachine
  };
};

// NtGetWriteWatch Flags constant
// Flags: 0 to query without resetting, or WRITE_WATCH_FLAG_RESET to
//   atomically query and reset (preferred — avoids race window).
// Only works on private memory (NtAllocateVirtualMemoryEx), not section views.
inline constexpr ULONG WRITE_WATCH_FLAG_RESET = 0x01;

//===----------------------------------------------------------------------===//
// Section Information Types
//===----------------------------------------------------------------------===//

// Section information classes for NtQuerySection.
enum SECTION_INFORMATION_CLASS {
  SectionBasicInformation = 0,          // q: SECTION_BASIC_INFORMATION
  SectionImageInformation = 1,          // q: SECTION_IMAGE_INFORMATION
  SectionRelocationInformation = 2,     // q: ULONG_PTR RelocationDelta (Win7+)
  SectionOriginalBaseInformation = 3,   // q: PVOID BaseAddress (RS1+)
  SectionInternalImageInformation = 4,  // q: SECTION_INTERNAL_IMAGE_INFORMATION (RS2+)
  MaxSectionInfoClass = 5
};

// Result structure for NtQuerySection(SectionBasicInformation).
struct SECTION_BASIC_INFORMATION {
  PVOID BaseAddress;              // Base VA if based section, else NULL
  ULONG AllocationAttributes;     // SEC_* flags
  LARGE_INTEGER MaximumSize;      // Maximum size in bytes
};

// Result structure for NtQuerySection(SectionImageInformation).
// Returns PE image details from a section handle without mapping it.
struct SECTION_IMAGE_INFORMATION {
  PVOID TransferAddress;          // Image entry point
  ULONG ZeroBits;                 // High-order zero bits required in base address
  SIZE_T MaximumStackSize;        // Max stack size from PE header
  SIZE_T CommittedStackSize;      // Initial stack commit from PE header
  ULONG SubSystemType;            // IMAGE_SUBSYSTEM_* value
  union {
    struct {
      USHORT SubSystemMinorVersion;
      USHORT SubSystemMajorVersion;
    };
    ULONG SubSystemVersion;
  };
  union {
    struct {
      USHORT MajorOperatingSystemVersion;
      USHORT MinorOperatingSystemVersion;
    };
    ULONG OperatingSystemVersion;
  };
  USHORT ImageCharacteristics;    // PE characteristics (IMAGE_FILE_*)
  USHORT DllCharacteristics;      // DLL characteristics (ASLR, NX, etc.)
  USHORT Machine;                 // IMAGE_FILE_MACHINE_* architecture
  BOOLEAN ImageContainsCode;      // Image has native executable code
  union {
    UCHAR ImageFlags;
    struct {
      UCHAR ComPlusNativeReady : 1;        // NGEN precompiled .NET
      UCHAR ComPlusILOnly : 1;             // IL-only .NET assembly
      UCHAR ImageDynamicallyRelocated : 1; // ASLR randomized base
      UCHAR ImageMappedFlat : 1;           // Single contiguous mapping
      UCHAR BaseBelow4gb : 1;              // Mapped below 4GB
      UCHAR ComPlusPrefer32bit : 1;        // Prefers WoW64
      UCHAR Reserved : 2;
    };
  };
  ULONG LoaderFlags;              // Reserved for ntdll loader
  ULONG ImageFileSize;            // Total image size including headers
  ULONG CheckSum;                 // PE optional header checksum
};

// Result structure for NtQuerySection(SectionInternalImageInformation) (RS2+).
// Extends SECTION_IMAGE_INFORMATION with CFG/CET/XFG security flags.
struct SECTION_INTERNAL_IMAGE_INFORMATION {
  SECTION_IMAGE_INFORMATION SectionInformation;
  union {
    ULONG ExtendedFlags;
    struct {
      ULONG ImageExportSuppressionEnabled : 1;
      ULONG ImageCetShadowStacksReady : 1;              // 20H1+
      ULONG ImageXfgEnabled : 1;                         // 20H2+
      ULONG ImageCetShadowStacksStrictMode : 1;
      ULONG ImageCetSetContextIpValidationRelaxedMode : 1;
      ULONG ImageCetDynamicApisAllowInProc : 1;
      ULONG ImageCetDowngradeReserved1 : 1;
      ULONG ImageCetDowngradeReserved2 : 1;
      ULONG ImageExportSuppressionInfoPresent : 1;
      ULONG ImageCfgEnabled : 1;
      ULONG Reserved : 22;
    };
  };
};

// ABI layout validation for section information types.
static_assert(sizeof(SECTION_IMAGE_INFORMATION) == 64,
              "SECTION_IMAGE_INFORMATION must be 64 bytes");
static_assert(FIELD_OFFSET(SECTION_IMAGE_INFORMATION, Machine) == 0x30,
              "SECTION_IMAGE_INFORMATION::Machine must be at offset 0x30");
static_assert(FIELD_OFFSET(SECTION_IMAGE_INFORMATION, CheckSum) == 0x3C,
              "SECTION_IMAGE_INFORMATION::CheckSum must be at offset 0x3C");

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_TYPES_H
