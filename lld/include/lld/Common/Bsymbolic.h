//===- Bsymbolic.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_BSYMBOLIC_H
#define LLD_COMMON_BSYMBOLIC_H

namespace lld {
// For -Bno-symbolic, -Bsymbolic-non-weak-functions, -Bsymbolic-functions,
// -Bsymbolic-non-weak, -Bsymbolic.
enum class BsymbolicKind { None, NonWeakFunctions, Functions, NonWeak, All };

// Returns true if -Bsymbolic* binds a symbol locally, given whether it is a
// function and whether it is weak. `symbolic` covers -Bsymbolic and any option
// with its effect, such as ELF's --dynamic-list.
inline bool isBoundBySymbolic(BsymbolicKind kind, bool symbolic, bool isFunc,
                              bool isWeak) {
  return symbolic || (kind == BsymbolicKind::NonWeak && !isWeak) ||
         (kind == BsymbolicKind::Functions && isFunc) ||
         (kind == BsymbolicKind::NonWeakFunctions && isFunc && !isWeak);
}
} // namespace lld

#endif
