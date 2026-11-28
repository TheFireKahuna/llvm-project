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
class SectionChunk;
class Symbol;

void markLive(COFFLinkerContext &ctx);

// Marks what roots and the sections of worklist, which are live, reach, and
// appends each section that it makes live to worklist.
void markLive(COFFLinkerContext &ctx, ArrayRef<Symbol *> roots,
              llvm::SmallVectorImpl<SectionChunk *> &worklist);

} // namespace lld::coff

#endif // LLD_COFF_MARKLIVE_H
