//===- posix_validation.h - POSIX-flag/prot validation helpers --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure-validation surface for the POSIX memory ops. Each helper accepts
/// raw POSIX inputs (page-rounded or not) and returns either 0 or a
/// positive POSIX errno value naming the failure mode. No syscalls, no
/// substrate touch — these run before any `va_tracker` or `nt_pal` call.
///
/// The validation contracts here are POSIX-spec exactly, with two
/// deliberate Windows-specific hardening choices preserved from the
/// legacy code:
///
///   * W^X (`PROT_WRITE | PROT_EXEC` → `EACCES` unless `MAP_WX` is set).
///     Matches Linux SELinux W^X errno; the explicit opt-in avoids
///     silently weakening JIT call sites that haven't been audited.
///   * `PROT_EXEC` without `PROT_READ` → `ENOTSUP`. Windows has no
///     execute-only PTE; silently promoting to `PAGE_EXECUTE_READ`
///     would lose the caller's expressed read-deny intent.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_VALIDATION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_VALIDATION_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/windows/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/nt_pal/large_pages.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory_posix {

//===----------------------------------------------------------------------===//
// Page / alloc-granularity helpers — thin re-exports of `windows::*` so the
// POSIX layer has one include for every rounding / alignment question.
//===----------------------------------------------------------------------===//

/// Round `addr` down to the system page boundary.
[[nodiscard]] LIBC_INLINE uintptr_t round_down_to_page(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_down_to_page(addr);
}

/// Round `addr` up to the system page boundary. Returns 0 on overflow.
[[nodiscard]] LIBC_INLINE uintptr_t round_up_to_page(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_up_to_page(addr);
}

/// True if `addr` is page-aligned.
[[nodiscard]] LIBC_INLINE bool is_page_aligned(const void *addr) {
  return ::LIBC_NAMESPACE::windows::is_page_aligned(addr);
}

/// True if `addr` is 64 KiB (allocation-granularity) aligned.
[[nodiscard]] LIBC_INLINE bool is_alloc_aligned(const void *addr) {
  return ::LIBC_NAMESPACE::windows::is_alloc_aligned(addr);
}

/// Round `addr` down to allocation-granularity.
[[nodiscard]] LIBC_INLINE uintptr_t align_down_to_granularity(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_down_to_granularity(addr);
}

/// Round `addr` up to allocation-granularity. Returns 0 on overflow.
[[nodiscard]] LIBC_INLINE uintptr_t align_up_to_granularity(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_up_to_granularity(addr);
}

//===----------------------------------------------------------------------===//
// Overflow-safe size / range checks.
//===----------------------------------------------------------------------===//

/// Round `len` up to a multiple of the page size; returns 0 if the round
/// would overflow `size_t`. Callers translate 0 to `ENOMEM` per POSIX.
[[nodiscard]] LIBC_INLINE size_t rounded_len_or_zero(size_t len) {
  if (len == 0)
    return 0;
  return static_cast<size_t>(::LIBC_NAMESPACE::windows::round_to_page(
      static_cast<SIZE_T>(len)));
}

/// True if `addr + len` would wrap past `UINTPTR_MAX`. Captures the
/// pre-round overflow case; combine with `rounded_len_or_zero` to catch
/// the post-round overflow as well.
[[nodiscard]] LIBC_INLINE bool addr_plus_len_overflows(uintptr_t addr,
                                                       size_t len) {
  return len > 0 && addr > (UINTPTR_MAX - len);
}

//===----------------------------------------------------------------------===//
// Protection-flag validation.
//===----------------------------------------------------------------------===//

/// Validate the protection / flag combination passed to mmap.
///
/// Rules (in evaluation order):
///   1. Reject prot bits outside `PROT_READ | PROT_WRITE | PROT_EXEC` →
///      `EINVAL`. The strict mask blocks `PROT_GROWSDOWN` / `PROT_GROWSUP`
///      / `PROT_SEM` so portable apps detect the gap explicitly.
///   2. `PROT_WRITE | PROT_EXEC` without `MAP_WX` → `EACCES`. JIT call
///      sites must opt in via the dedicated flag.
///   3. `PROT_EXEC` without `PROT_READ` → `ENOTSUP`. Windows has no
///      execute-only PTE.
///
/// Returns 0 on valid combinations.
[[nodiscard]] LIBC_INLINE int validate_posix_prot(int prot, int flags) {
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return EINVAL;
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC) && !(flags & MAP_WX))
    return EACCES;
  if ((prot & PROT_EXEC) && !(prot & PROT_READ))
    return ENOTSUP;
  return 0;
}

/// Validate the protection passed to mprotect. Same prot-bit rules as
/// mmap, minus the `MAP_WX` JIT opt-in — mprotect never permits W+X
/// since the caller already chose the mapping's content.
[[nodiscard]] LIBC_INLINE int validate_mprotect_prot(int prot) {
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return EINVAL;
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
    return EACCES;
  if ((prot & PROT_EXEC) && !(prot & PROT_READ))
    return ENOTSUP;
  return 0;
}

/// Wrapper for the canonical POSIX → `PAGE_*` lookup. Mentioned here so
/// no POSIX-layer file imports the substrate's `page_size.h` directly —
/// keeps the include graph aimed at this header.
///
/// Note: `PAGE_TARGETS_NO_UPDATE` / `PAGE_TARGETS_INVALID` (CFG modifier
/// bits) are NEVER ORed in. CFG suppression is a JIT-controlled concern
/// owned elsewhere; folding it into the base mprotect path would change
/// caller-visible semantics for indirect-call protection.
[[nodiscard]] LIBC_INLINE DWORD posix_prot_to_page(int prot) {
  return ::LIBC_NAMESPACE::windows::prot_to_page_flags(prot);
}

/// MAP_PRIVATE file view: `PROT_WRITE` becomes `PAGE_WRITECOPY` so writes
/// trigger kernel-side CoW without requiring section write access.
[[nodiscard]] LIBC_INLINE DWORD posix_prot_to_page_cow(int prot) {
  return ::LIBC_NAMESPACE::windows::prot_to_page_flags_cow(prot);
}

//===----------------------------------------------------------------------===//
// HUGETLB / large-page validation.
//===----------------------------------------------------------------------===//

/// True iff `SeLockMemoryPrivilege` was acquired by the libc at init.
/// Probe-once result is cached in PCB Zone 0; no syscall here.
[[nodiscard]] LIBC_INLINE bool large_pages_privilege_available() {
  return ::LIBC_NAMESPACE::nt_pal::large_pages_available();
}

/// Decode the page-size encoding inside an mmap `flags` argument
/// (`MAP_HUGE_2MB` / `MAP_HUGE_1GB` — Linux's `(log2(size) << 26)`
/// encoding). Returns:
///   * `nt_pal::LargePageKind::Huge` when the shift is 30 (1 GiB).
///   * `nt_pal::LargePageKind::Large` otherwise (default 2 MiB and any
///     unsupported shift coerce to 2 MiB, matching Linux x86_64).
[[nodiscard]] LIBC_INLINE ::LIBC_NAMESPACE::nt_pal::LargePageKind
decode_map_huge_shift(int flags) {
  unsigned shift =
      static_cast<unsigned>((flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
  if (shift == 30)
    return ::LIBC_NAMESPACE::nt_pal::LargePageKind::Huge;
  return ::LIBC_NAMESPACE::nt_pal::LargePageKind::Large;
}

//===----------------------------------------------------------------------===//
// mmap flag-bit validation.
//===----------------------------------------------------------------------===//

/// Linux numeric values for flags deliberately undefined in the Windows
/// `sys-mman-macros.h`. Recognised here so portable apps that pass them
/// get the spec-mandated `EINVAL` rather than silent acceptance.
inline constexpr int kMapGrowsdownNumericValue = 0x100;
inline constexpr int kMapSyncNumericValue = 0x80000;

/// Validate exactly-one-of `MAP_SHARED | MAP_PRIVATE` plus the rejected
/// flag bits. Returns 0 on success, positive errno otherwise.
///
/// Rules:
///   * Both `MAP_SHARED` and `MAP_PRIVATE` set, or neither, → `EINVAL`.
///   * `MAP_GROWSDOWN` (Linux 0x100) → `EINVAL` (stack-grow unsupported).
///   * `MAP_SYNC` (Linux 0x80000) → `EINVAL` (DAX synchronous mapping,
///     no NT equivalent).
[[nodiscard]] LIBC_INLINE int validate_mmap_flags(int flags) {
  const int sharing = flags & (MAP_SHARED | MAP_PRIVATE);
  if (sharing == 0 || sharing == (MAP_SHARED | MAP_PRIVATE))
    return EINVAL;
  if (flags & kMapGrowsdownNumericValue)
    return EINVAL;
  if (flags & kMapSyncNumericValue)
    return EINVAL;
  return 0;
}

/// Validate the `MAP_FIXED_NOREPLACE` precondition. Returns:
///   * 0 if the flag is not set, or if it is and `addr` is non-null and
///     page-aligned.
///   * `EINVAL` if the flag is set with null or unaligned `addr`.
[[nodiscard]] LIBC_INLINE int
validate_fixed_noreplace_addr(int flags, const void *addr) {
  if (!(flags & MAP_FIXED_NOREPLACE))
    return 0;
  if (addr == nullptr)
    return EINVAL;
  if (!is_page_aligned(addr))
    return EINVAL;
  return 0;
}

//===----------------------------------------------------------------------===//
// mremap flag-matrix validation.
//===----------------------------------------------------------------------===//

/// Validate the mremap flag matrix per Linux 5.7+ rules.
///
/// Rejection cases (each `EINVAL`):
///   * Unknown bits outside `{MREMAP_MAYMOVE, MREMAP_FIXED, MREMAP_DONTUNMAP}`.
///   * `MREMAP_DONTUNMAP` without `MREMAP_MAYMOVE`.
///   * `MREMAP_DONTUNMAP` with `old_size != new_size`.
///   * `MREMAP_FIXED` without `MREMAP_MAYMOVE`.
///
/// Returns 0 on a valid combination.
[[nodiscard]] LIBC_INLINE int
validate_mremap_flags(int flags, size_t old_size, size_t new_size) {
  const int known =
      MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP;
  if ((flags & ~known) != 0)
    return EINVAL;
  if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
    return EINVAL;
  if ((flags & MREMAP_DONTUNMAP) && old_size != new_size)
    return EINVAL;
  if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
    return EINVAL;
  return 0;
}

//===----------------------------------------------------------------------===//
// msync flag validation.
//===----------------------------------------------------------------------===//

/// Validate the msync flag combination. Exactly one of `MS_ASYNC` and
/// `MS_SYNC` must be set; `MS_INVALIDATE` is optional. Other bits are
/// rejected with `EINVAL`. Returns 0 on success.
[[nodiscard]] LIBC_INLINE int validate_msync_flags(int flags) {
  const int known = MS_ASYNC | MS_SYNC | MS_INVALIDATE;
  if ((flags & ~known) != 0)
    return EINVAL;
  const int sync_bits = flags & (MS_ASYNC | MS_SYNC);
  if (sync_bits == 0 || sync_bits == (MS_ASYNC | MS_SYNC))
    return EINVAL;
  return 0;
}

//===----------------------------------------------------------------------===//
// madvise advice-code validation.
//===----------------------------------------------------------------------===//

/// Linux-numeric advice codes recognised by the rebuild, beyond what the
/// Windows `sys-mman-macros.h` already defines. Declared inline so the
/// dispatch validator below can name each one without a header touch.
///
/// Recognition implies "handled or deliberately ENOTSUP/EPERM/EINVAL by
/// the dispatcher" — the dispatch table in P5 owns the per-code reply.
inline constexpr int kMadviseRemove = 9;
inline constexpr int kMadviseDontfork = 10;
inline constexpr int kMadviseDofork = 11;
inline constexpr int kMadviseWipeOnFork = 18;
inline constexpr int kMadviseKeepOnFork = 19;
inline constexpr int kMadviseCollapse = 25;
inline constexpr int kMadviseHwpoison = 100;
inline constexpr int kMadviseSoftOffline = 101;
inline constexpr int kMadviseGuardInstall = 102;
inline constexpr int kMadviseGuardRemove = 103;

/// True iff `advice` is a code the rebuild's dispatch table claims an
/// answer for. Unrecognised codes return `EINVAL` from the entry point
/// per Linux behaviour — apps detect kernel-feature absence via this
/// errno, so silent acceptance is the most insidious regression vector.
[[nodiscard]] LIBC_INLINE bool madvise_advice_recognised(int advice) {
  switch (advice) {
  case MADV_NORMAL:
  case MADV_RANDOM:
  case MADV_SEQUENTIAL:
  case MADV_WILLNEED:
  case MADV_DONTNEED:
  case MADV_FREE:
  case MADV_HUGEPAGE:
  case MADV_NOHUGEPAGE:
  case MADV_MERGEABLE:
  case MADV_UNMERGEABLE:
  case MADV_DONTDUMP:
  case MADV_DODUMP:
  case MADV_COLD:
  case MADV_PAGEOUT:
  case MADV_POPULATE_READ:
  case MADV_POPULATE_WRITE:
  case kMadviseRemove:
  case kMadviseDontfork:
  case kMadviseDofork:
  case kMadviseWipeOnFork:
  case kMadviseKeepOnFork:
  case kMadviseCollapse:
  case kMadviseHwpoison:
  case kMadviseSoftOffline:
  case kMadviseGuardInstall:
  case kMadviseGuardRemove:
    return true;
  default:
    return false;
  }
}

/// Validate the madvise advice + range arguments at entry. Mirrors the
/// legacy entry-validation pattern.
///
/// Order (each early-exit on first match):
///   1. `addr == nullptr` → `ENOMEM` (Linux glibc semantic — distinct
///      from `EINVAL` because apps test for unmapped-address detection).
///   2. `addr` unaligned → `EINVAL`.
///   3. `size == 0` → 0 (success no-op).
///   4. Page-round overflow → `ENOMEM`.
///   5. `addr + rounded_size` overflows → `ENOMEM`.
///   6. Advice not recognised → `EINVAL`.
///
/// On success, returns 0 and writes the page-rounded size to
/// `*rounded_out`.
[[nodiscard]] LIBC_INLINE int validate_madvise_entry(void *addr, size_t size,
                                                     int advice,
                                                     size_t *rounded_out) {
  if (addr == nullptr)
    return ENOMEM;
  if (!is_page_aligned(addr))
    return EINVAL;
  if (size == 0) {
    *rounded_out = 0;
    return 0;
  }
  const size_t rounded = rounded_len_or_zero(size);
  if (rounded == 0)
    return ENOMEM;
  if (addr_plus_len_overflows(reinterpret_cast<uintptr_t>(addr), rounded))
    return ENOMEM;
  if (!madvise_advice_recognised(advice))
    return EINVAL;
  *rounded_out = rounded;
  return 0;
}

} // namespace memory_posix
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_POSIX_POSIX_VALIDATION_H
