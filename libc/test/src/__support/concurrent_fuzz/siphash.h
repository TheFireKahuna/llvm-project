//===--- concurrent_fuzz siphash ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SipHash-2-4 (Aumasson & Bernstein, 2012). Used to:
//
//   * Hash walk_range visit results (a sequence of (lo, hi, value_id)
//     tuples) to a single u64 fingerprint — keeps history entries
//     fixed-size while preserving a strong "same visit ⇔ same hash"
//     equality property (collision rate ~2^-64).
//
//   * Hash oracle states for memoization in the linearizability checker.
//
// Keyed by the schedule seed so collisions that masquerade as bugs
// reproduce on the failing seed only and don't pollute regression seeds
// from a different run.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SIPHASH_H
#define LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SIPHASH_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

struct SipHasher {
  uint64_t v0, v1, v2, v3;

  // Initialize from a 128-bit key (k0, k1).
  void reset(uint64_t k0, uint64_t k1);

  // Absorb 8 bytes of input (compressed in one round-pair).
  void absorb_u64(uint64_t word);

  // Finalize and return the 64-bit hash. Subsequent calls before reset
  // are undefined — call `reset` between hashes.
  [[nodiscard]] uint64_t finalize(uint8_t tail_byte_count, uint64_t tail);
};

// Convenience: hash a buffer of u64 words. Tail-byte count is always 0
// because the input is u64-aligned by construction.
[[nodiscard]] uint64_t siphash_u64s(uint64_t k0, uint64_t k1,
                                    const uint64_t *words, size_t count);

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC___SUPPORT_CONCURRENT_FUZZ_SIPHASH_H
