//===- RelExprMask.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_RELEXPRMASK_H
#define LLD_COMMON_RELEXPRMASK_H

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace lld {
// Build a bitmask with one bit set for each 64 subset of RelExpr.
inline constexpr uint64_t buildMask() { return 0; }

template <typename... Tails>
inline constexpr uint64_t buildMask(int head, Tails... tails) {
  return (0 <= head && head < 64 ? uint64_t(1) << head : 0) |
         buildMask(tails...);
}

// Return true if `Expr` is one of `Exprs`, enumerators of a linker's RelExpr.
// There are more than 64 but less than 128 RelExprs, so we divide the set of
// exprs into [0, 64) and [64, 128) and represent each range as a constant
// 64-bit mask. Then we decide which mask to test depending on the value of
// expr and use a simple shift and bitwise-and to test for membership.
template <auto... Exprs>
bool oneof(std::common_type_t<decltype(Exprs)...> expr) {
  assert(0 <= expr && (int)expr < 128 &&
         "RelExpr is too large for 128-bit mask!");

  if (expr >= 64)
    return (uint64_t(1) << (expr - 64)) & buildMask((Exprs - 64)...);
  return (uint64_t(1) << expr) & buildMask(Exprs...);
}
} // namespace lld

#endif
