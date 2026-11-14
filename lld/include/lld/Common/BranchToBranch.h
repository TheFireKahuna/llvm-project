//===- BranchToBranch.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_BRANCHTOBRANCH_H
#define LLD_COMMON_BRANCHTOBRANCH_H

#include <cstddef>
#include <cstdint>
#include <utility>

namespace lld {
// The branch-to-branch optimization for one relocation: if r, a control
// transfer relocation with the given effective addend, transfers control to a
// branch instruction, make it branch to where that branch goes.
//
// getBranchInfo(r, addend): If the relocation directly transfers control to a
// relocated branch instruction, return the relocation for that branch as well
// as its effective addend. Otherwise return {nullptr, 0}.
//
// redirect(r1, r2): Modify r1 so that it branches directly to the target of r2.
template <typename RelTy, typename GetBranchInfo, typename Redirect>
void redirectBranchToBranch(RelTy &r, uint64_t addend,
                            GetBranchInfo getBranchInfo, Redirect redirect) {
  std::pair<RelTy *, uint64_t> targetAndAddend = getBranchInfo(r, addend);
  if (!targetAndAddend.first)
    return;
  // Avoid getting stuck in an infinite loop if we encounter a branch
  // that (possibly indirectly) branches to itself. It is unlikely
  // that more than 5 iterations will ever be needed in practice.
  size_t iterations = 5;
  while (iterations--) {
    std::pair<RelTy *, uint64_t> nextTargetAndAddend =
        getBranchInfo(*targetAndAddend.first, targetAndAddend.second);
    if (!nextTargetAndAddend.first)
      break;
    targetAndAddend = nextTargetAndAddend;
  }
  redirect(r, *targetAndAddend.first);
}
} // namespace lld

#endif
