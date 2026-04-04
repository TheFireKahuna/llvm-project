//===-- CSPRNG-seeded XOR canary derivation ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Unifies the "derive a per-object canary from a CSPRNG seed XOR'd with
// stable address/size bits" pattern across the tree. Four call sites,
// three distinct usage shapes:
//
//   slab_pool.h:163    freelist_cookie     — per-slab XOR cookie for
//                                             encoded freelist pointers
//                                             (slab_pool.h:657-663).
//   slab_pool.h:173    canary_key          — per-slab canary key,
//                                             INDEPENDENT of freelist_cookie
//                                             (documented contract: keep
//                                             keys separate so freelist
//                                             corruption cannot forge a
//                                             valid canary).
//   thread_scratch.h:143 canary_seed       — per-arena seed, used for
//                                             BOTH freelist encoding
//                                             (fl_encode/fl_decode at
//                                             thread_scratch.h:562-570)
//                                             AND block canary
//                                             (thread_scratch.h:260).
//                                             Reuses one seed because the
//                                             arena is thread-local —
//                                             no freelist-forges-canary
//                                             threat model.
//   fcntl_lock_table.h:99 canary_key_      — per-table seed, SIZE-FREE
//                                             canary: key ^ &record
//                                             (no payload size in the
//                                             mix because LockRecord has
//                                             a fixed layout).
//
// Lessons encoded in the primitive:
//
// 1. Two-seed vs one-seed is a POLICY the caller picks, not a forced
//    shape. SlabPool pays for two independent seeds because its freelist
//    lives inside user-controllable memory (a UAF can rewrite the next
//    pointer, and a separate canary key defeats a freelist-to-canary
//    forge chain). thread_scratch pays for only one because the data
//    never escapes a single thread. The primitive exposes both via
//    CanarySeed (two draws) and SingleCanarySeed (one draw); neither is
//    promoted as "the default."
//
// 2. derive_canary accepts size as an OPTIONAL parameter (default 0) so
//    the lock-table size-free variant composes without a second entry
//    point. When size == 0 the expression degenerates to
//    seed ^ reinterpret_cast<uintptr_t>(address), which is exactly
//    fcntl_lock_table.h:112-114's make_canary and slab_pool.h:1496's
//    slab canary expression.
//
// 3. init_seed_or_trap is fail-closed for all callers. On Windows 11,
//    ntdll's ProcessPrng is a userland DRBG with no syscall, no I/O,
//    and no transient-unavailability window. A runtime failure indicates
//    process-integrity loss (heap corruption, module tampering, or
//    self-test failure that would have aborted module load). Silently
//    substituting a compile-time sentinel is strictly worse than
//    trapping: the sentinel is public knowledge, so any path that
//    reaches the fallback gives an attacker a known-plaintext seed and
//    defeats the entire hardening scheme. A previous design supported
//    fail-open with sentinels for thread_scratch / fcntl_lock_table;
//    that was a theoretical defence against a failure mode that does
//    not occur on the target platform, and it weakened every caller
//    that reached it. Trap instead.
//
// 4. xor_encode_next/xor_decode_next bind the STORAGE ADDRESS of the
//    pointer, not its target. A freelist corruption that moves the
//    encoded bytes to a different slot decodes to garbage — matches
//    slab_pool.h:657-663 and thread_scratch.h:562-565 exactly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_CANARY_SEED_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_CANARY_SEED_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace alloc_primitives {

// Two-seed variant. SlabPool shape. primary is the freelist XOR cookie,
// secondary is the independent canary key. Callers that read both must
// use them for disjoint purposes — mixing defeats the point.
struct CanarySeed {
  uintptr_t primary = 0;
  uintptr_t secondary = 0;

  LIBC_INLINE bool initialized() const {
    return primary != 0 && secondary != 0;
  }
};

// Single-seed variant. thread_scratch / fcntl_lock_table shape. Used when
// the caller does not need cryptographic separation between the freelist
// XOR cookie and the canary key (single-owner data, fixed-layout record).
struct SingleCanarySeed {
  uintptr_t seed = 0;

  LIBC_INLINE bool initialized() const { return seed != 0; }
};

// Populate a CanarySeed from ProcessPrng. Two independent 64-bit draws.
// Traps via __builtin_trap on ProcessPrng failure OR on either draw
// returning zero. See the header block for why fail-closed is the only
// correct policy on Windows 11.
LIBC_INLINE void init_seed_or_trap(CanarySeed &seed) {
  if (!::ProcessPrng(reinterpret_cast<unsigned char *>(&seed.primary),
                     sizeof(seed.primary)) ||
      seed.primary == 0)
    __builtin_trap();
  if (!::ProcessPrng(reinterpret_cast<unsigned char *>(&seed.secondary),
                     sizeof(seed.secondary)) ||
      seed.secondary == 0)
    __builtin_trap();
}

// Single-seed init. One ProcessPrng draw, same fail-closed policy.
LIBC_INLINE void init_seed_or_trap(SingleCanarySeed &seed) {
  if (!::ProcessPrng(reinterpret_cast<unsigned char *>(&seed.seed),
                     sizeof(seed.seed)) ||
      seed.seed == 0)
    __builtin_trap();
}

// Derive a canary value from (seed, address, optional size). Mixes via
// XOR. size=0 collapses to the size-free form used by fcntl_lock_table
// (make_canary: canary_key_ ^ &record) and SlabPool (canary_key ^ slot).
// All non-zero size values produce a canary that depends on both the
// object's address and its declared payload size — corruption that
// relocates an object with a matching address but different size fails
// verification. Matches thread_scratch.h:260
//   hdr->canary = seed ^ (uintptr_t)hdr ^ user_size
// and fcntl_lock_table.h:112-114 / slab_pool.h:1496 (size=0 path).
[[nodiscard]] LIBC_INLINE uintptr_t derive_canary(uintptr_t seed,
                                                  const void *address,
                                                  size_t size = 0) {
  return seed ^ reinterpret_cast<uintptr_t>(address) ^
         static_cast<uintptr_t>(size);
}

// Verify a previously-derived canary. Recomputes and compares. Returns
// true on match. Callers trap via LIBC_ASSERT / __builtin_trap on mismatch
// — the primitive does not trap itself because SlabPool and LockTable
// use different trap disciplines.
[[nodiscard]] LIBC_INLINE bool verify_canary(uintptr_t expected,
                                             uintptr_t seed,
                                             const void *address,
                                             size_t size = 0) {
  return expected == derive_canary(seed, address, size);
}

// XOR-encode a freelist next-pointer bound to its STORAGE ADDRESS. The
// storage_addr argument is where the encoded pointer will be written —
// binding it in means a corruption that moves the encoded bytes decodes
// to garbage, not a valid redirect.
//
// Matches slab_pool.h:657-663 encode_next and thread_scratch.h:562-565
// fl_encode exactly. The XOR is an involution: a single expression
// serves both encode and decode; the two entry points exist only to
// document intent at callsites and to give the type system a hook for
// future wrappers (e.g. a typed FreeNode variant).
//
// A nullptr next encodes to (seed ^ storage_addr), which decodes back
// to nullptr; callers can still use nullptr as an end-of-list sentinel.
[[nodiscard]] LIBC_INLINE void *
xor_encode_next(void *next, uintptr_t seed, const void *storage_addr) {
  return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(next) ^ seed ^
                                  reinterpret_cast<uintptr_t>(storage_addr));
}

// Inverse of xor_encode_next. Same seed + same storage address must be
// supplied. The caller is responsible for bounds-validating the decoded
// pointer — see thread_scratch.h:575-584 fl_decode_validated for the
// arena-bounds check pattern, which traps via __builtin_trap on a
// decoded pointer outside the arena's data region.
[[nodiscard]] LIBC_INLINE void *
xor_decode_next(void *encoded, uintptr_t seed, const void *storage_addr) {
  return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(encoded) ^ seed ^
                                  reinterpret_cast<uintptr_t>(storage_addr));
}

} // namespace alloc_primitives
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PRIMITIVES_CANARY_SEED_H
