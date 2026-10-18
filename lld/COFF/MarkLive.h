//===- MarkLive.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_MARKLIVE_H
#define LLD_COFF_MARKLIVE_H

#include "lld/Common/LLVM.h"

namespace lld::coff {

class COFFLinkerContext;
class Symbol;

// Marks the chunks reachable from the GC roots live, or, given roots, those
// reachable from them.
void markLive(COFFLinkerContext &ctx, ArrayRef<Symbol *> roots = {});
} // namespace lld::coff

#endif // LLD_COFF_MARKLIVE_H
