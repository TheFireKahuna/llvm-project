//===-- nt_pal::large_pages — large-page PAL primitives ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Large-page (2 MiB / 1 GiB) PAL surface. Three responsibilities:
//
//   1. `probe_se_lock_memory_privilege` — one-shot privilege probe via
//      `RtlAdjustPrivilege`, called from `pal_init.cpp` at
//      `.libcmem$P0` and re-called from `pal_fork.cpp` post-fork.
//      The result is cached as `large_pages_available()` in
//      `pal_state.h`.
//
//   2. `large_page_alignment(LargePageKind)` — alignment / minimum size
//      for the requested page kind, sourced from
//      `KUSER_SHARED_DATA.LargePageMinimum` (= `GetLargePageMinimum`)
//      for the 2 MiB case and a constant for the 1 GiB case. Returns 0
//      when the kernel does not advertise large-page support.
//
//   3. `reserve_large_pages` / `commit_replace_large` — the
//      placeholder-replace pair for anonymous large-page mappings,
//      composed from `placeholder.h::reserve_placeholder_aligned` plus
//      a section-backed commit (`SEC_LARGE_PAGES` / `SEC_HUGE_PAGES` +
//      `MEM_REPLACE_PLACEHOLDER`). MAP_HUGETLB consumes these.
//
// **Documented exception to P1.A.** `commit_replace` always passes
// `MEM_WRITE_WATCH`; `commit_replace_large` does NOT. The kernel
// rejects `MEM_LARGE_PAGES + MEM_WRITE_WATCH` outright (large-page
// PTEs vs the per-4 KiB-page write-watch bitmap;
// `MMAP_OPTIMIZATION_RESEARCH.md` §17.9). Large-page commits are the
// single carve-out in the libc tree; no other PAL path skips
// write-watch.
//
// **Section-backed by kernel mandate.** A private-VM
// `MEM_LARGE_PAGES + MEM_REPLACE_PLACEHOLDER` is also kernel-rejected
// (§15.2 / §1.4 / §2.1: large-page private VM is one-shot and never
// transitions to placeholder state). The only viable path that
// participates in the placeholder lifecycle is `SEC_LARGE_PAGES` /
// `SEC_HUGE_PAGES` section views via `MEM_REPLACE_PLACEHOLDER`. The
// resulting mapping is `MEM_MAPPED`, dispatching uniformly through the
// section munmap path.
//
// **Probe-once.** `probe_se_lock_memory_privilege` is called from
// init / fork-reinit only. Hot-path callers consult
// `large_pages_available()` and short-circuit with `EPERM` when false.
// `MAP_HUGETLB` is forbidden from silent downgrade
// (`NTPOSIX_MEMORY_ARCHITECTURE_DESIGN` §11.2).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_LARGE_PAGES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_LARGE_PAGES_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/section.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

// Page-size selector for large-page entry points. The PAL maps these
// to the corresponding `SEC_*` flag internally so callers never name
// the raw kernel constants directly.
enum class LargePageKind : uint8_t {
  Large, // 2 MiB; SEC_LARGE_PAGES; alignment from GetLargePageMinimum.
  Huge,  // 1 GiB; SEC_HUGE_PAGES; alignment hard-coded.
};

// Probe `SeLockMemoryPrivilege` on the current process token. Returns
// true if the privilege is now enabled, false if the token does not
// hold it (`STATUS_PRIVILEGE_NOT_HELD`) or any other failure.
//
// Side effect: enables the privilege if held. The privilege stays
// enabled for the process lifetime — `MAP_HUGETLB` callers may rely on
// the kernel allowing the commit. If a future call site needs to
// disable, it must do so explicitly via `RtlAdjustPrivilege` with
// Enable=FALSE.
//
// Caller is `pal_init.cpp` and `pal_fork.cpp` only.
[[nodiscard]] LIBC_INLINE bool probe_se_lock_memory_privilege() {
  BOOLEAN was_enabled = FALSE;
  // CurrentThread = FALSE → adjust the process token (not a thread
  // impersonation token), so the change is process-wide.
  NTSTATUS st = ::RtlAdjustPrivilege(SeLockMemoryPrivilege,
                                      /* Enable= */ TRUE,
                                      /* CurrentThread= */ FALSE,
                                      &was_enabled);
  return NT_SUCCESS(st);
}

// Alignment / minimum size for the requested page kind. Reads
// `KUSER_SHARED_DATA.LargePageMinimum` for `Large` (typically 2 MiB on
// x64; 0 on systems without HW backing) and returns a 1 GiB constant
// for `Huge`. The caller rounds the user-requested mapping size up to
// this alignment before calling `reserve_large_pages` /
// `commit_replace_large`.
[[nodiscard]] LIBC_INLINE size_t large_page_alignment(LargePageKind kind) {
  if (kind == LargePageKind::Huge)
    return static_cast<size_t>(1) << 30; // 1 GiB
  return static_cast<size_t>(::GetLargePageMinimum());
}

// Reserve a placeholder of `size` bytes aligned for the requested
// page kind. `size` must already be a multiple of
// `large_page_alignment(kind)`. `addr` is a hint (or nullptr to let
// the kernel pick at the page-kind alignment).
//
// Returns the placeholder base on success; nullptr on failure (caller
// retries / maps to ENOMEM). Privilege is NOT checked here — callers
// must short-circuit on `large_pages_available()` per design §11.2 to
// produce the EPERM mandated for MAP_HUGETLB without privilege.
[[nodiscard]] LIBC_INLINE void *
reserve_large_pages(void *addr, size_t size, LargePageKind kind) {
  size_t align = large_page_alignment(kind);
  if (align == 0)
    return nullptr;
  if (addr != nullptr)
    return reserve_placeholder(addr, size);
  return reserve_placeholder_aligned(size, align).base;
}

// Replace a placeholder at `[base, base+size)` with a large-page-backed
// anonymous section view. Internal sequence:
//
//   NtCreateSectionEx(SEC_LARGE_PAGES | SEC_HUGE_PAGES,
//                     PAGE_EXECUTE_READWRITE)
//   NtMapViewOfSectionEx(MEM_REPLACE_PLACEHOLDER, prot)
//
// The section is created at PAGE_EXECUTE_READWRITE so subsequent
// `mprotect` to any combination is permitted; `prot` is the initial
// view protection. On success `*out_section` receives the section
// handle (caller stores in the mapping table — sections must outlive
// their views).
//
// On failure the placeholder is untouched (caller may retry, release,
// or fall through to the destructor); on success the placeholder VA
// is consumed by the view and the caller must NOT release it via
// `free_placeholder`.
//
// Returns `STATUS_SUCCESS` on success. Common failure modes:
//   * `STATUS_PRIVILEGE_NOT_HELD` — `SeLockMemoryPrivilege` not held.
//   * `STATUS_INVALID_PARAMETER`  — size not a multiple of the
//                                    page-kind alignment, or `Huge` on
//                                    a kernel without 1 GiB support.
//   * `STATUS_COMMITMENT_LIMIT`   — large-page commit charge exhausted.
//
// **Documented exception to the P1.A "always MEM_WRITE_WATCH"
// invariant.** Section views never carry write-watch (kernel rejects
// the combination on `MEM_MAPPED`); large-page sections doubly so.
// Callers that need fork-CoW dirty tracking on large-page regions
// must fall back to full copy.
[[nodiscard]] LIBC_INLINE NTSTATUS
commit_replace_large(void *base, size_t size, DWORD prot, LargePageKind kind,
                     HANDLE *out_section) {
  ULONG sec_flags =
      (kind == LargePageKind::Huge) ? SEC_HUGE_PAGES : SEC_LARGE_PAGES;
  HANDLE section = nullptr;
  // Section max-prot is PAGE_EXECUTE_READWRITE so views can later
  // mprotect to any combination; the actual initial protection is
  // applied by the map call below.
  NTSTATUS st = create_section_anon(size, PAGE_EXECUTE_READWRITE, &section,
                                    sec_flags);
  if (NT_ERROR(st))
    return st;
  LARGE_INTEGER offset;
  offset.QuadPart = 0;
  st = map_section_replace(section, base, size, offset, prot);
  if (NT_ERROR(st)) {
    close_section(section);
    return st;
  }
  *out_section = section;
  return STATUS_SUCCESS;
}

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_LARGE_PAGES_H
