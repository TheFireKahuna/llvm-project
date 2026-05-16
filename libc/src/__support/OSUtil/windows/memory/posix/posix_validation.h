//===- posix_validation.h - POSIX-flag/prot validation helpers --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pure-validation surface for the POSIX memory ops. Every helper accepts raw
// POSIX inputs and returns 0 or a positive POSIX errno; no syscalls, no
// substrate touch — these run before any `va_tracker` or `nt_pal` call so a
// rejected request never holds a VA reservation or section handle.
//
// Two deliberate hardening choices diverge from a literal POSIX read and stay
// here regardless of layer:
//
//   * W^X: `PROT_WRITE | PROT_EXEC` without `MAP_WX` → `EACCES`. POSIX permits
//     the combination; we follow the SELinux-on-Linux errno so portable code
//     that assumed deny-by-default keeps working. JITs opt in explicitly.
//   * `PROT_EXEC` without `PROT_READ` → `ENOTSUP`. Windows has no
//     execute-only PTE; the kernel would promote to PAGE_EXECUTE_READ and
//     silently violate the caller's read-deny intent.
//
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
// Page / alloc-granularity helpers — thin re-exports so the POSIX layer has
// one include for every rounding / alignment question.
//===----------------------------------------------------------------------===//

[[nodiscard]] LIBC_INLINE uintptr_t round_down_to_page(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_down_to_page(addr);
}

// Returns 0 on overflow.
[[nodiscard]] LIBC_INLINE uintptr_t round_up_to_page(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_up_to_page(addr);
}

[[nodiscard]] LIBC_INLINE bool is_page_aligned(const void *addr) {
  return ::LIBC_NAMESPACE::windows::is_page_aligned(addr);
}

// True iff `addr` is 64 KiB (NT allocation-granularity) aligned.
[[nodiscard]] LIBC_INLINE bool is_alloc_aligned(const void *addr) {
  return ::LIBC_NAMESPACE::windows::is_alloc_aligned(addr);
}

[[nodiscard]] LIBC_INLINE uintptr_t align_down_to_granularity(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_down_to_granularity(addr);
}

// Returns 0 on overflow.
[[nodiscard]] LIBC_INLINE uintptr_t align_up_to_granularity(uintptr_t addr) {
  return ::LIBC_NAMESPACE::windows::align_up_to_granularity(addr);
}

//===----------------------------------------------------------------------===//
// Overflow-safe size / range checks.
//===----------------------------------------------------------------------===//

// Round `len` up to a multiple of the page size. Returns 0 if the round would
// overflow `size_t` OR if `len` was already 0; callers translate 0 to
// `ENOMEM` per POSIX.1-2017 mmap §2 ("length is 0" is implementation-defined,
// and Linux returns EINVAL/ENOMEM — we keep ENOMEM for the overflow case so
// the entry validator can distinguish via its own length pre-check).
[[nodiscard]] LIBC_INLINE size_t rounded_len_or_zero(size_t len) {
  if (len == 0)
    return 0;
  return static_cast<size_t>(::LIBC_NAMESPACE::windows::round_to_page(
      static_cast<SIZE_T>(len)));
}

// True if `addr + len` would wrap past `UINTPTR_MAX`. Captures the pre-round
// overflow; combine with `rounded_len_or_zero` to catch the post-round
// overflow as well — both are required because rounding can lift a non-
// overflowing length into one that overflows the address span.
[[nodiscard]] LIBC_INLINE bool addr_plus_len_overflows(uintptr_t addr,
                                                       size_t len) {
  return len > 0 && addr > (UINTPTR_MAX - len);
}

//===----------------------------------------------------------------------===//
// Protection-flag validation.
//===----------------------------------------------------------------------===//

// Validate the protection / flag combination passed to mmap. Returns 0 on
// success.
//
// Rules, in evaluation order (first match wins):
//   1. Prot bits outside `PROT_READ | PROT_WRITE | PROT_EXEC` → `EINVAL`.
//      POSIX.1-2017 mmap §3 leaves additional bits implementation-defined;
//      the strict mask blocks Linux `PROT_GROWSDOWN`/`PROT_GROWSUP`/`PROT_SEM`
//      so portable apps detect the gap rather than receive a silent grant.
//   2. `PROT_WRITE | PROT_EXEC` without `MAP_WX` → `EACCES` (see banner).
//   3. `PROT_EXEC` without `PROT_READ` → `ENOTSUP` (see banner).
[[nodiscard]] LIBC_INLINE int validate_posix_prot(int prot, int flags) {
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return EINVAL;
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC) && !(flags & MAP_WX))
    return EACCES;
  if ((prot & PROT_EXEC) && !(prot & PROT_READ))
    return ENOTSUP;
  return 0;
}

// mprotect variant — same prot-bit rules as mmap, minus the `MAP_WX`
// opt-in. POSIX.1-2017 mprotect §2 does not provide a flags argument, so
// there is no place for a JIT opt-in: W+X is unconditionally rejected.
[[nodiscard]] LIBC_INLINE int validate_mprotect_prot(int prot) {
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return EINVAL;
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
    return EACCES;
  if ((prot & PROT_EXEC) && !(prot & PROT_READ))
    return ENOTSUP;
  return 0;
}

// POSIX → `PAGE_*` lookup forwarder. Re-exported so no POSIX-layer file
// imports the substrate's `page_size.h` directly. `PAGE_TARGETS_NO_UPDATE`
// / `PAGE_TARGETS_INVALID` (CFG modifier bits) are NEVER ORed in: CFG
// suppression is a JIT-owned concern, and folding it into the base path
// would change caller-visible indirect-call protection semantics.
[[nodiscard]] LIBC_INLINE DWORD posix_prot_to_page(int prot) {
  return ::LIBC_NAMESPACE::windows::prot_to_page_flags(prot);
}

// MAP_PRIVATE file-view variant: `PROT_WRITE` becomes `PAGE_WRITECOPY` so
// writes trigger kernel-side CoW without requiring section write access —
// MAP_PRIVATE per POSIX.1-2017 mmap §3 forbids propagation back to the file.
[[nodiscard]] LIBC_INLINE DWORD posix_prot_to_page_cow(int prot) {
  return ::LIBC_NAMESPACE::windows::prot_to_page_flags_cow(prot);
}

//===----------------------------------------------------------------------===//
// HUGETLB / large-page validation.
//===----------------------------------------------------------------------===//

// True iff `SeLockMemoryPrivilege` was acquired by the libc at init.
// Probe-once result; no syscall here.
[[nodiscard]] LIBC_INLINE bool large_pages_privilege_available() {
  return ::LIBC_NAMESPACE::nt_pal::large_pages_available();
}

// Decode the page-size shift inside an mmap `flags` argument, matching
// Linux's `(log2(size) << MAP_HUGE_SHIFT)` encoding (sys-mman-macros.h
// defines `MAP_HUGE_2MB = 21<<26`, `MAP_HUGE_1GB = 30<<26`). Returns
// `Huge` for shift == 30 (1 GiB) and `Large` otherwise — including
// shift == 0 (default 2 MiB) and any unsupported shift, both of which
// coerce to 2 MiB to match Linux x86_64.
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

// Linux numeric values for flags deliberately undefined in the Windows
// `sys-mman-macros.h`. Named here so portable apps that pass them get the
// spec-mandated `EINVAL` rather than silent acceptance — leaving the bits
// nameless would let them slip through the unknown-bit mask once a third
// flag started occupying the same value.
inline constexpr int kMapGrowsdownNumericValue = 0x100;
inline constexpr int kMapSyncNumericValue = 0x80000;

// Validate the sharing-mode + rejected-flag matrix for mmap.
//
// Rules:
//   * Exactly one of `MAP_SHARED | MAP_PRIVATE` must be set
//     (POSIX.1-2017 mmap §3 "MAP_PRIVATE and MAP_SHARED are mutually
//     exclusive"; the standard does not name an errno for the zero-flags
//     case, but every conforming implementation returns EINVAL).
//   * `MAP_GROWSDOWN` (Linux 0x100) → `EINVAL`: stack-grow semantics
//     require kernel-side guard-page extension we do not implement.
//   * `MAP_SYNC` (Linux 0x80000) → `EINVAL`: DAX synchronous mapping has
//     no NT-side equivalent (no DAX filesystem on NTFS/ReFS).
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

// `MAP_FIXED_NOREPLACE` precondition check. Returns 0 if the flag is unset
// or if it is set with non-null page-aligned `addr`; `EINVAL` otherwise.
// Unlike plain `MAP_FIXED`, NOREPLACE callers cannot fall back to "kernel
// picks an address" on failure (POSIX-2024 mmap, Linux 4.17+ extension),
// so a null/unaligned hint is unambiguously a caller bug.
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

// Validate the mremap flag matrix per Linux 5.7+ rules (mremap is not in
// POSIX; we track Linux semantics so portable code behaves identically).
// Returns 0 on success, `EINVAL` for any rejection.
//
// Rejection cases:
//   * Bits outside `{MREMAP_MAYMOVE, MREMAP_FIXED, MREMAP_DONTUNMAP}`.
//   * `MREMAP_DONTUNMAP` without `MREMAP_MAYMOVE` — DONTUNMAP needs a
//     second VA to leave the old one zero-filled; without MAYMOVE there
//     is no second VA.
//   * `MREMAP_DONTUNMAP` with `old_size != new_size` — Linux requires
//     same-size because the new mapping must exactly mirror the old VA
//     span before zero-fill.
//   * `MREMAP_FIXED` without `MREMAP_MAYMOVE` — FIXED requests a specific
//     new address; MAYMOVE is the umbrella consent for relocation.
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

// Validate msync flags per POSIX.1-2017 msync §3: exactly one of `MS_ASYNC`
// and `MS_SYNC` must be set ("flags shall be a bitwise-inclusive OR of"),
// `MS_INVALIDATE` is optional, no other bits are accepted. Returns 0 on
// success, `EINVAL` otherwise.
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

// Linux-numeric advice codes recognised by the rebuild beyond what the
// Windows `sys-mman-macros.h` already defines. Named here so the recognition
// switch below can list them without a header touch. Recognition implies
// "the dispatcher claims an answer" — handled or deliberately
// ENOTSUP/EPERM/EINVAL by the per-code branch; it does NOT imply success.
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

// True iff `advice` is a code the dispatch table claims an answer for.
// Unrecognised codes return `EINVAL` from the entry point per Linux
// behaviour — apps probe kernel-feature availability via this errno, so
// silent acceptance would mask both the missing feature AND the apps'
// probe, making the regression invisible until production.
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

// Validate madvise entry args. On success writes the page-rounded size to
// `*rounded_out` and returns 0; on failure returns the errno and leaves
// `*rounded_out` untouched (except in the size==0 short-circuit, which
// writes 0 and returns 0 — caller's no-op fast path).
//
// Order is load-bearing — caller errno disambiguation relies on it:
//   1. `addr == nullptr` → `ENOMEM`. The Linux kernel returns ENOMEM
//      (not EINVAL) for unmapped addresses so apps can distinguish
//      "no mapping at addr" from "bad flags"; NULL is unmapped by
//      definition on every supported target.
//   2. `addr` unaligned → `EINVAL`. POSIX.1-2017 posix_madvise §3 and
//      Linux madvise(2) both require page-aligned addr.
//   3. `size == 0` → 0 (no-op). Both POSIX and Linux say this is a
//      success no-op; we return early so step 4 doesn't see a 0 round
//      as an overflow.
//   4. Page-round overflow → `ENOMEM`.
//   5. `addr + rounded_size` overflows the address space → `ENOMEM`.
//   6. Advice not recognised → `EINVAL`.
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
