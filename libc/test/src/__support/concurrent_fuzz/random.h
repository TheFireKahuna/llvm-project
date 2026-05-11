//===--- concurrent_fuzz random ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SplitMix64 — small, deterministic, repeatable PRNG used by every layer
// of the concurrent_fuzz framework. We pick SplitMix64 specifically
// because it is stateless modulo a single u64 counter — a single seed
// reproduces a schedule end-to-end, including derived sub-streams (each
// worker, each shrinker step) seeded by mixing the parent state with a
// stable role tag.
//
// Not cryptographic. Used only for input generation and shrink-step
// selection; nothing security-sensitive depends on its statistical
// properties beyond "different seeds produce visibly different
// schedules."
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_RANDOM_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_RANDOM_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

struct SplitMix64 {
  uint64_t state;

  LIBC_INLINE constexpr explicit SplitMix64(uint64_t seed) : state(seed) {}

  LIBC_INLINE uint64_t next_u64() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  LIBC_INLINE uint32_t next_u32() {
    return static_cast<uint32_t>(next_u64() >> 32);
  }

  // Uniformly distributed in [0, n). Lemire's Fast-Range trick at u32
  // width, biased <2^-32 — irrelevant for fuzzing.
  LIBC_INLINE uint32_t next_below(uint32_t n) {
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(next_u32()) * static_cast<uint64_t>(n)) >> 32);
  }

  // Geometric distribution with parameter p = 1/(1<<bits). Used by the
  // tower-jump bias to bias addresses toward high `ctz(va)`.
  LIBC_INLINE uint32_t next_geometric(uint32_t bits) {
    uint32_t k = 0;
    while (k < 32 && (next_u32() >> (32 - bits)) != 0)
      ++k;
    return k;
  }
};

// Derive a child PRNG from a parent state plus a stable role tag. The
// child is independent of further parent draws (mixing tag in resists
// trivial collisions across (parent, tag) pairs).
LIBC_INLINE SplitMix64 derive(uint64_t parent, uint64_t role_tag) {
  uint64_t z = parent ^ (role_tag * 0xD1B54A32D192ED03ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return SplitMix64(z ^ (z >> 31));
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_RANDOM_H
