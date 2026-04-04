//===-- NT memory API declarations -------------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Virtual Memory APIs
//===----------------------------------------------------------------------===//

// NtQueryVirtualMemory - query virtual memory region attributes.
// BaseAddress is rounded down to a page boundary. The kernel scans upward from
// that page until attributes change, returning the contiguous region.
// MemoryInformationClass: see MEMORY_INFORMATION_CLASS enum.
//   MemoryBasicInformation → MEMORY_BASIC_INFORMATION (most common)
//   MemoryRegionInformationEx → MEMORY_REGION_INFORMATION (placeholder detection)
//   MemoryWorkingSetExInformation → MEMORY_WORKING_SET_EX_INFORMATION (per-page)
//   MemoryMappedFilenameInformation → UNICODE_STRING (backing file path)
//   MemoryImageInformation → MEMORY_IMAGE_INFORMATION (PE image details)
// Returns STATUS_INVALID_ADDRESS if BaseAddress is outside accessible range.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                     MEMORY_INFORMATION_CLASS MemoryInformationClass,
                     PVOID MemoryInformation, SIZE_T MemoryInformationLength,
                     SIZE_T *ReturnLength);

// NtPssCaptureVaSpaceBulk - bulk VA space enumeration (Win10 20H1+)
// Fills BulkInformation header + trailing MBI array. BaseAddress sets
// the enumeration start point (nullptr = beginning of VA space).
// BulkInformationLength is the total buffer size including the header.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtPssCaptureVaSpaceBulk(HANDLE ProcessHandle, PVOID BaseAddress,
                         NTPSS_MEMORY_BULK_INFORMATION *BulkInformation,
                         SIZE_T BulkInformationLength,
                         SIZE_T *ReturnLength);

// NtLockVirtualMemory - lock pages in physical memory
// All pages in the range must be committed; PAGE_NOACCESS pages cannot be
// locked. BaseAddress and RegionSize are [inout] — rounded to page boundaries.
// Returns STATUS_SUCCESS even if pages are already locked (no-op).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtLockVirtualMemory(HANDLE ProcessHandle,
                                                         PVOID *BaseAddress,
                                                         SIZE_T *RegionSize,
                                                         ULONG MapType);

// NtUnlockVirtualMemory - unlock pages from physical memory
// MapType must match the type used in the corresponding NtLockVirtualMemory.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtUnlockVirtualMemory(HANDLE ProcessHandle,
                                                           PVOID *BaseAddress,
                                                           SIZE_T *RegionSize,
                                                           ULONG MapType);

// NtFreeVirtualMemory - free virtual memory
// BaseAddress/RegionSize are [inout], kernel adjusts to page boundaries.
// FreeType must be one of:
//   MEM_DECOMMIT — decommit committed pages (keeps reservation)
//   MEM_RELEASE — release entire region; BaseAddress must be the original
//     allocation base and RegionSize must be 0. Can also combine with
//     MEM_COALESCE_PLACEHOLDERS or MEM_PRESERVE_PLACEHOLDER for placeholder ops.
// Memory allocated by NtAllocateVirtualMemoryEx must be freed with this
// function — not NtUnmapViewOfSectionEx (which is for section views).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtFreeVirtualMemory(HANDLE ProcessHandle,
                                                          PVOID *BaseAddress,
                                                          SIZE_T *RegionSize,
                                                          ULONG FreeType);

// NtProtectVirtualMemory - change memory protection
// All pages in the range must be committed; fails if any page is not committed.
// All pages must be within a single reserved region from the same allocation;
// cannot span adjacent reservations. BaseAddress/RegionSize are [inout],
// kernel adjusts to page boundaries. OldProtection receives the previous
// protection of the first page (must not be NULL).
// For section views (NtMapViewOfSectionEx), NewProtection must be compatible
// with the access protection specified when the view was mapped.
// After making a region executable, caller must flush the instruction cache.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                       SIZE_T *RegionSize, ULONG NewProtection,
                       ULONG *OldProtection);

// NtFlushVirtualMemory - flush memory-mapped pages to backing store
// Used to implement msync(). BaseAddress and RegionSize are rounded to pages.
// IoStatusBlock receives operation status; Information field has flushed size.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtFlushVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                     SIZE_T *RegionSize, IO_STATUS_BLOCK *IoStatusBlock);

// NtFlushProcessWriteBuffers - global memory barrier (IPI to all CPUs).
// Equivalent to Linux membarrier(MEMBARRIER_CMD_GLOBAL). Ensures all
// preceding stores on all processors are visible before returning.
// Required after unmap/remap cycles to flush stale TLB entries.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtFlushProcessWriteBuffers(void);


//===----------------------------------------------------------------------===//
// Extended Virtual Memory APIs (Win10 RS5+ / Win8+)
//===----------------------------------------------------------------------===//

// NtAllocateVirtualMemoryEx - allocate with extended parameters (Win10 RS5+)
// Extension of NtAllocateVirtualMemory with MEM_EXTENDED_PARAMETER support for
// NUMA node selection, address range/alignment requirements, and partitions.
//
// BaseAddress [inout]: if non-NULL, rounded down to page boundary; the range
//   must be free (for MEM_RESERVE) or already reserved (for MEM_COMMIT alone).
//   If NULL, the system chooses the address.
// RegionSize [inout]: rounded up to page boundary. Must not be zero on input.
// AllocationType: one of MEM_COMMIT, MEM_RESERVE, or MEM_RESET must be set.
//   See MEM_* constants above for full flag descriptions and constraints.
// PageProtection: PAGE_* constant. Ignored for MEM_RESET/MEM_RESET_UNDO but
//   must still be a valid value.
// Returns STATUS_SUCCESS or an error:
//   STATUS_INVALID_PARAMETER — bad flags, zero RegionSize, invalid protection
//   STATUS_CONFLICTING_ADDRESSES — BaseAddress conflicts with existing state
//   STATUS_INVALID_PAGE_PROTECTION — incompatible protection for the region
//   STATUS_ALREADY_COMMITTED — MEM_RESERVE on already-reserved pages
//   STATUS_NO_MEMORY / STATUS_COMMITMENT_LIMIT — out of memory/commit charge
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAllocateVirtualMemoryEx(HANDLE ProcessHandle, PVOID *BaseAddress,
                          SIZE_T *RegionSize, ULONG AllocationType,
                          ULONG PageProtection,
                          MEM_EXTENDED_PARAMETER *ExtendedParameters,
                          ULONG ExtendedParameterCount);

// NtMapViewOfSectionEx - map section with extended parameters (Win10 RS5+)
// AllocationType: 0, MEM_RESERVE, MEM_REPLACE_PLACEHOLDER, or MEM_LARGE_PAGES.
//   MEM_RESERVE: maps a reserved (non-committed) view.
//   MEM_REPLACE_PLACEHOLDER: replaces a placeholder; BaseAddress and ViewSize
//     must exactly match the placeholder. Only data/pagefile-backed sections
//     (no images, no physical memory). 64K alignment on Offset/BaseAddress is
//     relaxed — only page alignment required.
//   MEM_LARGE_PAGES: maps using large pages. ViewSize must be a multiple of
//     the large-page minimum. Section must have been created with
//     SEC_LARGE_PAGES. If BaseAddress is non-NULL it must be large-page
//     aligned. 64K alignment on Offset is relaxed.
// SectionOffset: must be 64K-aligned (or large-page-aligned with
//   MEM_LARGE_PAGES, or page-aligned with MEM_REPLACE_PLACEHOLDER).
// PageProtection: must be compatible with the section's original protection.
//   For SEC_IMAGE sections, ignored (set to any valid value like PAGE_READONLY).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtMapViewOfSectionEx(HANDLE SectionHandle, HANDLE ProcessHandle,
                     PVOID *BaseAddress, LARGE_INTEGER *SectionOffset,
                     SIZE_T *ViewSize, ULONG AllocationType,
                     ULONG PageProtection,
                     MEM_EXTENDED_PARAMETER *ExtendedParameters,
                     ULONG ExtendedParameterCount);

// NtUnmapViewOfSectionEx - unmap with flags (Win8+)
// BaseAddress must be the exact value returned by NtMapViewOfSectionEx.
// Flags: 0, MEM_UNMAP_WITH_TRANSIENT_BOOST, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtUnmapViewOfSectionEx(HANDLE ProcessHandle,
                                                            PVOID BaseAddress,
                                                            ULONG Flags);

// NtSetInformationVirtualMemory - batched operations on address ranges (Win8+)
// Performs a class-specific operation on NumberOfEntries address ranges.
// NumberOfEntries must not be 0. Ranges may cover any accessible part of the
// process address space.
//
// VmPrefetchInformation: issues large concurrent I/Os to bring paged-out ranges
//   into physical memory (cached, not added to working set until accessed).
//   Pure performance hint — not required for correctness, may partially fail
//   under low memory. VmInformation → MEMORY_PREFETCH_INFORMATION (Flags = 0).
// VmPagePriorityInformation: sets eviction priority for ranges.
//   VmInformation → MEMORY_PAGE_PRIORITY_INFORMATION.
// VmCfgCallTargetInformation: validates/invalidates CFG call targets.
//   VmInformation → CFG_CALL_TARGET_LIST_INFORMATION.
// VmPageDirtyStateInformation: dirty state manipulation (Hyper-V only).
//   VmInformation → MEMORY_PAGE_DIRTY_STATE_INFORMATION.
//   Returns 0xC00000F3 for non-Hyper-V processes; see struct comment.
// VmRemoveFromWorkingSetInformation: evicts pages from working set.
//   VmInformation → MEMORY_REMOVE_WORKING_SET_INFORMATION.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetInformationVirtualMemory(
    HANDLE ProcessHandle,
    VIRTUAL_MEMORY_INFORMATION_CLASS VmInformationClass,
    SIZE_T NumberOfEntries, MEMORY_RANGE_ENTRY *VirtualAddresses,
    PVOID VmInformation, ULONG VmInformationLength);

// NtReadVirtualMemoryEx - read virtual memory with extended options (Win11+)
// ProcessHandle must have PROCESS_VM_READ access.
// Flags: undocumented; pass 0 for standard read behavior. No known flag
// values have been publicly documented or reverse-engineered.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtReadVirtualMemoryEx(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                      SIZE_T NumberOfBytesToRead, SIZE_T *NumberOfBytesRead,
                      ULONG Flags);

// NtWriteVirtualMemory - write to a process's virtual memory.
// ProcessHandle must have PROCESS_VM_WRITE | PROCESS_VM_OPERATION access.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtWriteVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                      SIZE_T NumberOfBytesToWrite, SIZE_T *NumberOfBytesWritten);

// NtGetWriteWatch - retrieve addresses of pages written since allocation or
// last reset. Region must have been allocated with MEM_WRITE_WATCH.
// Flags: 0 to query without resetting, or WRITE_WATCH_FLAG_RESET to
//   atomically query and reset (preferred — avoids race window).
// UserAddressArray [out]: receives page-aligned addresses of written pages.
// EntriesInUserAddressArray [inout]: array capacity in, entries returned out.
// Granularity [out]: page size in bytes.
// Only works on private memory (NtAllocateVirtualMemoryEx), not section views.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtGetWriteWatch(HANDLE ProcessHandle, ULONG Flags, PVOID BaseAddress,
                SIZE_T RegionSize, PVOID *UserAddressArray,
                PULONG_PTR EntriesInUserAddressArray, PULONG Granularity);

// NtResetWriteWatch - reset write-tracking state for a region.
// After reset, NtGetWriteWatch only reports pages written since this call.
// Caller must ensure no threads write to the region between a non-resetting
// NtGetWriteWatch and this call — otherwise written pages may go undetected.
// Prefer WRITE_WATCH_FLAG_RESET in NtGetWriteWatch to avoid this race.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtResetWriteWatch(HANDLE ProcessHandle, PVOID BaseAddress, SIZE_T RegionSize);


//===----------------------------------------------------------------------===//
// Memory Partition APIs (Win10+)
//===----------------------------------------------------------------------===//

// NtCreatePartition - create memory partition
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreatePartition(HANDLE ParentPartitionHandle, HANDLE *PartitionHandle,
                  ULONG DesiredAccess, PVOID ObjectAttributes);

// NtOpenPartition - open existing partition
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtOpenPartition(HANDLE *PartitionHandle,
                                                     ULONG DesiredAccess,
                                                     PVOID ObjectAttributes);

// NtManagePartition - manage partition properties
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtManagePartition(HANDLE TargetHandle, HANDLE SourceHandle,
                  PARTITION_INFORMATION_CLASS PartitionInformationClass,
                  PVOID PartitionInformation, ULONG PartitionInformationLength);


//===----------------------------------------------------------------------===//
// Section APIs
//===----------------------------------------------------------------------===//

// NtCreateSectionEx - create a section object backed by a file or the pagefile.
// SectionPageProtection: PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE, or
//   PAGE_WRITECOPY. PAGE_NOACCESS and modifiers (PAGE_GUARD, PAGE_NOCACHE,
//   PAGE_WRITECOMBINE) are not valid here — use SEC_NOCACHE/SEC_WRITECOMBINE.
// AllocationAttributes: SEC_* flags. Must include exactly one of SEC_COMMIT,
//   SEC_RESERVE, or SEC_IMAGE. See SEC_* constants for constraints.
// FileHandle: if NULL, section is pagefile-backed and MaximumSize is required.
//   If non-NULL, section is file-backed; MaximumSize specifies the max extent
//   the file can be mapped/extended to (rounded up to page size).
// ExtendedParameters: supports MemExtendedParameterNumaNode (preferred NUMA
//   node, passed as 1-based internally) and MemExtendedParameterAttributeFlags.
//   Only one instance of each parameter type per call. Pass nullptr/0 if unused.
// Returns STATUS_MAPPED_FILE_SIZE_ZERO if both file size and MaximumSize are 0.
// Close SectionHandle with NtClose when done.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateSectionEx(HANDLE *SectionHandle, ULONG DesiredAccess,
                  PVOID ObjectAttributes, LARGE_INTEGER *MaximumSize,
                  ULONG SectionPageProtection, ULONG AllocationAttributes,
                  HANDLE FileHandle,
                  MEM_EXTENDED_PARAMETER *ExtendedParameters,
                  ULONG ExtendedParameterCount);

// NtOpenSection — open a handle to an existing named section object.
// Used to implement shm_open() for existing shared memory segments.
// ObjectAttributes.ObjectName identifies the section in the NT namespace
// (e.g. \BaseNamedObjects\my_shared_mem). The section must already exist
// (created by NtCreateSectionEx or another process).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenSection(HANDLE *SectionHandle, ACCESS_MASK DesiredAccess,
              PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtExtendSection - grow a section's maximum size after creation.
// Only valid for pagefile-backed or writable file-backed sections.
// NewMaximumSize must be >= current max size.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtExtendSection(HANDLE SectionHandle,
                                                      LARGE_INTEGER *NewMaximumSize);

// NtQuerySection - query section attributes (max size, protection, type).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQuerySection(HANDLE SectionHandle,
               SECTION_INFORMATION_CLASS SectionInformationClass,
               PVOID SectionInformation, SIZE_T SectionInformationLength,
               SIZE_T *ReturnLength);

// NtAreMappedFilesTheSame — determine whether two addresses belong to views
// mapped from the same underlying section (file) object. Returns
// STATUS_SUCCESS if both addresses are within mappings of the same file,
// STATUS_NOT_SAME_DEVICE otherwise.
// Useful for stat() equivalence checks (same st_dev/st_ino) and detecting
// duplicate/overlapping mappings without kernel handle comparison.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAreMappedFilesTheSame(PVOID File1MappedAsAnImage, PVOID File2MappedAsFile);


} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_MEMORY_API_H
