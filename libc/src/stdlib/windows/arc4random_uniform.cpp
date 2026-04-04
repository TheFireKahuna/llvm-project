//===-- Windows implementation of arc4random_uniform -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/arc4random_uniform.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/unistd/getentropy.h"

namespace LIBC_NAMESPACE_DECL {

static uint32_t get_random_u32() {
  uint32_t val;
  LIBC_NAMESPACE::getentropy(&val, sizeof(val));
  return val;
}

// Lemire's nearly divisionless algorithm for unbiased bounded random numbers.
// See "Fast Random Integer Generation in an Interval" (D. Lemire, 2019).
//
// Standard rejection sampling discards values in the biased range
// [0, 2^32 % upper_bound), requiring a division per attempt. Lemire's
// method uses a 64-bit product to extract the result and only performs
// a division (to compute the threshold) when the fast path doesn't
// resolve — which is the uncommon case.
LLVM_LIBC_FUNCTION(uint32_t, arc4random_uniform, (uint32_t upper_bound)) {
  if (upper_bound <= 1)
    return 0;

  uint32_t raw = get_random_u32();

  uint64_t product = static_cast<uint64_t>(raw) * static_cast<uint64_t>(upper_bound);
  auto low = static_cast<uint32_t>(product);

  // Fast path: if the low 32 bits are >= upper_bound, no bias is possible.
  if (low < upper_bound) {
    // Compute rejection threshold: (2^32 - upper_bound) % upper_bound.
    // Equivalent to (-upper_bound) % upper_bound using unsigned wrap.
    uint32_t threshold = (-upper_bound) % upper_bound;
    while (low < threshold) {
      raw = get_random_u32();
      product = static_cast<uint64_t>(raw) * static_cast<uint64_t>(upper_bound);
      low = static_cast<uint32_t>(product);
    }
  }

  return static_cast<uint32_t>(product >> 32);
}

} // namespace LIBC_NAMESPACE_DECL
