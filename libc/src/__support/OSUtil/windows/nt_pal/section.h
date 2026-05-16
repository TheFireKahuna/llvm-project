//===-- nt_pal::section — section view primitives ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Section / view-of-section operations. The Layer 0 surface for every
// `mmap` of a file or shared anonymous region; the allocator's
// private-anonymous path goes through `placeholder.h` instead.
//
// Public surface (per `NTPOSIX_MEMORY_ARCHITECTURE_DESIGN` §6.0):
//
//   * `create_section_anon`    — `NtCreateSectionEx` wrapper for an
//                                 anonymous (pagefile-backed) section.
//                                 Caller-controlled `sec_flags` admits
//                                 `SEC_COMMIT` (default), `SEC_RESERVE`,
//                                 `SEC_64K_PAGES`, `SEC_LARGE_PAGES`,
//                                 `SEC_HUGE_PAGES`, etc.
//   * `create_section_file`    — same shape over a file handle. Use
//                                 `PAGE_WRITECOPY` for MAP_PRIVATE,
//                                 `PAGE_READWRITE` / `PAGE_READONLY` for
//                                 MAP_SHARED.
//   * `close_section`          — `NtClose` typed for section handles.
//   * `map_section_replace`    — map a view atop a placeholder via
//                                 `MEM_REPLACE_PLACEHOLDER`.
//   * `unmap_view_preserve`    — tear down a view but keep the VA as a
//                                 placeholder.
//   * `unmap_view_preserve_transient` — same as above with the kernel's
//                                 deferred-IO priority hint, used on
//                                 hot paths where unmap dominates.
//   * `unmap_view`             — tear down a view to MEM_FREE; used on
//                                 full-munmap and on error rollback.
//
// Section creators return `NTSTATUS` so callers can map kernel failure
// modes (privilege-not-held for `SEC_LARGE_PAGES`, file-access-denied,
// address-conflict) onto distinct POSIX errnos. The HANDLE goes via an
// out parameter.
//
// Object attributes are uniformly `windows::internal_oa()` —
// non-inheritable, anonymous. Every libc-internal section object follows
// the convention; see `nt/handle_attributes.h` for rationale.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_SECTION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_SECTION_H

#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Create an anonymous section (pagefile-backed) of the given size.
// `protect` is the section's max page protection (caps any view's
// later mprotect). `sec_flags` selects the section type: `SEC_COMMIT`
// (default), `SEC_RESERVE` for demand-commit, `SEC_64K_PAGES` for 64 KiB
// physical contiguity, `SEC_LARGE_PAGES` / `SEC_HUGE_PAGES` for large /
// huge-page backing (both require `SeLockMemoryPrivilege`).
//
// On success returns `STATUS_SUCCESS` and writes the handle to
// `*out_section`. On failure returns the kernel's NTSTATUS and leaves
// `*out_section` untouched.
[[nodiscard]] LIBC_INLINE NTSTATUS create_section_anon(size_t size,
                                                       DWORD protect,
                                                       HANDLE *out_section,
                                                       ULONG sec_flags
                                                           = SEC_COMMIT) {
  HANDLE handle = nullptr;
  LARGE_INTEGER section_size;
  section_size.QuadPart = static_cast<LONGLONG>(size);
  auto oa = ::LIBC_NAMESPACE::windows::internal_oa();
  NTSTATUS st = ::NtCreateSectionEx(
      &handle, SECTION_ALL_ACCESS, &oa, &section_size, protect, sec_flags,
      /* FileHandle= */ nullptr, /* ExtendedParameters= */ nullptr,
      /* ExtendedParameterCount= */ 0);
  if (NT_SUCCESS(st))
    *out_section = handle;
  return st;
}

// Create a file-backed section over `file_handle`. `protect` selects
// the view semantics: `PAGE_WRITECOPY` for MAP_PRIVATE, `PAGE_READWRITE`
// or `PAGE_READONLY` for MAP_SHARED. `section_access` is the section
// object access mask — derive from the file handle's grants
// (`SECTION_MAP_READ` / `SECTION_MAP_WRITE` / `SECTION_QUERY` are the
// usual selection). Read-only file handles MUST NOT request
// `SECTION_MAP_WRITE` or `SECTION_MAP_EXECUTE`; the kernel returns
// `STATUS_ACCESS_DENIED` against the source file's grants — this is
// load-bearing security, not just an error code.
//
// `max_size` may be 0 to use the file's current length. `sec_flags`
// defaults to `SEC_COMMIT`; pass `SEC_LARGE_PAGES` / `SEC_HUGE_PAGES`
// for file-backed MAP_HUGETLB or `SEC_64K_PAGES` for the 64 KiB-page
// MAP_SHARED optimisation.
//
// On success returns `STATUS_SUCCESS` and writes the handle to
// `*out_section`. On failure returns the kernel's NTSTATUS and leaves
// `*out_section` untouched.
[[nodiscard]] LIBC_INLINE NTSTATUS create_section_file(HANDLE file_handle,
                                                       size_t max_size,
                                                       DWORD protect,
                                                       ULONG section_access,
                                                       HANDLE *out_section,
                                                       ULONG sec_flags
                                                           = SEC_COMMIT) {
  HANDLE handle = nullptr;
  LARGE_INTEGER section_size;
  section_size.QuadPart = static_cast<LONGLONG>(max_size);
  PLARGE_INTEGER size_arg = (max_size != 0) ? &section_size : nullptr;
  auto oa = ::LIBC_NAMESPACE::windows::internal_oa();
  NTSTATUS st = ::NtCreateSectionEx(
      &handle, section_access, &oa, size_arg, protect, sec_flags, file_handle,
      /* ExtendedParameters= */ nullptr, /* ExtendedParameterCount= */ 0);
  if (NT_SUCCESS(st))
    *out_section = handle;
  return st;
}

// Close a section handle. Wraps `NtClose` for surface uniformity.
LIBC_INLINE bool close_section(HANDLE section) {
  return NT_SUCCESS(::NtClose(section));
}

// Map a view of `section` over a placeholder at `addr`. Uses
// `MEM_REPLACE_PLACEHOLDER` so the placeholder's VA is consumed by the
// view atomically — overlap detection is handled by the kernel.
//
// `view_size` is the size of the view (must equal the placeholder's
// size or the kernel rejects). `section_offset` is the byte offset
// within the section the view starts at. `prot` is the view
// protection.
[[nodiscard]] LIBC_INLINE NTSTATUS
map_section_replace(HANDLE section, void *addr, size_t view_size,
                    LARGE_INTEGER section_offset, DWORD prot) {
  PVOID base = addr;
  SIZE_T sz = view_size;
  return ::NtMapViewOfSectionEx(section, NtCurrentProcess(), &base,
                                 &section_offset, &sz, MEM_REPLACE_PLACEHOLDER,
                                 prot, /* ExtendedParameters= */ nullptr,
                                 /* ExtendedParameterCount= */ 0);
}

// Tear down a section view, leaving the VA as MEM_FREE. Use when the
// VA is being released (full munmap, error rollback that does not
// need to preserve the placeholder).
LIBC_INLINE NTSTATUS unmap_view(void *addr) {
  return ::NtUnmapViewOfSectionEx(NtCurrentProcess(), addr, 0);
}

// Tear down a view but keep the VA reserved as a placeholder. Use
// during split / rollback paths where the placeholder will be reused
// or coalesced. Returns `STATUS_NOT_MAPPED_VIEW` on an already-unmapped
// address — rollback callers operating on partial state treat that as
// success.
LIBC_INLINE NTSTATUS unmap_view_preserve(void *addr) {
  return ::NtUnmapViewOfSectionEx(NtCurrentProcess(), addr,
                                   MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
}

// As `unmap_view_preserve`, with the kernel's transient priority boost
// (deferred-IO hint). Same `STATUS_NOT_MAPPED_VIEW` idempotence.
LIBC_INLINE NTSTATUS unmap_view_preserve_transient(void *addr) {
  return ::NtUnmapViewOfSectionEx(
      NtCurrentProcess(), addr,
      MEM_UNMAP_WITH_TRANSIENT_BOOST | MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
}

// Flush dirty pages in `[addr, addr+size)` of a writable file-backed
// view back to the underlying file. Wraps `NtFlushVirtualMemory`,
// the canonical surface for POSIX `msync(MS_SYNC)` /
// `msync(MS_ASYNC)`. Returns the raw NTSTATUS; on success
// `*iosb_out` carries the IO_STATUS_BLOCK whose `Information` field
// is the actual byte count flushed (may be smaller than `size` if
// the kernel coalesced an end-of-section overlap).
//
// `iosb_out` must be non-null — the kernel always writes to it. The
// caller passes a stack-local IO_STATUS_BLOCK and inspects after
// return.
[[nodiscard]] LIBC_INLINE NTSTATUS flush_virtual_memory(
    void *addr, size_t size, IO_STATUS_BLOCK *iosb_out) {
  PVOID base = addr;
  SIZE_T sz = size;
  return ::NtFlushVirtualMemory(NtCurrentProcess(), &base, &sz, iosb_out);
}

// Create a NAMED section under `namespace_root` with the given name.
// Used for cross-process IPC rendezvous (POSIX shm, PTY rings, SysV
// sem headers) where the section name is a stable handle other
// processes can `NtOpenSection` against.
//
// `namespace_root` is the OBJECT_ATTRIBUTES root directory — typically
// the libc-private namespace handle (created at Tier A bring-up). Pass
// `nullptr` only if the caller has already qualified the name as a
// fully-rooted NT path; this is rare and is the only escape hatch.
//
// `name` is a NUL-terminated UTF-16 string; `name_len_chars` is its
// length in WCHARs (excluding the NUL). The PAL builds the
// UNICODE_STRING in stack storage.
//
// `WCHAR` is the NT-canonical UTF-16 char type (`char16_t`, 16-bit) —
// not the C++ `wchar_t`, which under NTPOSIX is 32-bit per the
// `-fwchar-type=int -fsigned-wchar` driver flags.
//
// Other parameters mirror `create_section_anon`. Returns
// `STATUS_OBJECT_NAME_COLLISION` if a section with the same name
// already exists in the namespace; the caller handles by either
// opening the existing section (`NtOpenSection`) or surfacing an
// errno.
[[nodiscard]] LIBC_INLINE NTSTATUS create_section_named(
    HANDLE namespace_root, const WCHAR *name, size_t name_len_chars,
    size_t section_size, DWORD page_protect, ULONG sec_flags,
    HANDLE *out_section) {
  HANDLE handle = nullptr;
  LARGE_INTEGER size;
  size.QuadPart = static_cast<LONGLONG>(section_size);

  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(name_len_chars * sizeof(WCHAR));
  us.MaximumLength = static_cast<USHORT>((name_len_chars + 1) * sizeof(WCHAR));
  us.Buffer = const_cast<WCHAR *>(name);

  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, namespace_root,
                             nullptr);

  NTSTATUS st = ::NtCreateSectionEx(
      &handle, SECTION_ALL_ACCESS, &oa, &size, page_protect, sec_flags,
      /* FileHandle= */ nullptr, /* ExtendedParameters= */ nullptr,
      /* ExtendedParameterCount= */ 0);
  if (NT_SUCCESS(st))
    *out_section = handle;
  return st;
}

// Map a view of `section` at a kernel-chosen VA. Used for libc-
// internal subsystems whose VA does not need to coordinate with the
// placeholder grid (signal mailboxes, FIFO channels, PTY rings, SysV
// sem headers). The B3 facade in Phase B wraps this with a pagemap
// stamp so the resulting VA participates in the universal-non-faulting
// reader contract.
//
// `*inout_base`: if non-null on entry, used as a kernel allocation
// hint; otherwise the kernel picks any free VA. On success holds the
// final VA.
// `*inout_size`: if non-zero on entry, requests a partial view of
// that size (rounded up to allocation granularity); otherwise full
// section. On success holds the actual mapped size.
// `section_offset` is byte offset into the section.
// `alloc_type` may include `MEM_RESERVE` / `MEM_COMMIT` / `MEM_TOP_DOWN`
// / `MEM_LARGE_PAGES`; do NOT include `MEM_REPLACE_PLACEHOLDER` here
// (use `map_section_replace` for that path).
[[nodiscard]] LIBC_INLINE NTSTATUS map_section_anywhere(
    HANDLE section, LARGE_INTEGER section_offset, DWORD prot,
    ULONG alloc_type, void **inout_base, size_t *inout_size) {
  PVOID base = inout_base ? *inout_base : nullptr;
  SIZE_T sz = inout_size ? *inout_size : 0;
  NTSTATUS st = ::NtMapViewOfSectionEx(
      section, NtCurrentProcess(), &base, &section_offset, &sz, alloc_type,
      prot, /* ExtendedParameters= */ nullptr,
      /* ExtendedParameterCount= */ 0);
  if (NT_SUCCESS(st)) {
    if (inout_base)
      *inout_base = base;
    if (inout_size)
      *inout_size = static_cast<size_t>(sz);
  }
  return st;
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_SECTION_H
