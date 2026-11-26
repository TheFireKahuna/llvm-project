//===- ICF.h --------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_ICF_H
#define LLD_COFF_ICF_H

#include "Config.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include <utility>
#include <vector>

namespace lld::coff {

class COFFLinkerContext;
class SectionChunk;

void doICF(COFFLinkerContext &ctx);

// Each chunk whose replacement a pass changed, with the one it had. A chunk
// that was its own was live.
using UnwindFolds = std::vector<std::pair<SectionChunk *, SectionChunk *>>;

// After relaxation, where the link folds sections: folds the unwind records of
// resized code that are equal once their spans have their values. The caller
// takes the records that were live out of the layout, and gives every chunk
// its replacement back to unfold them.
UnwindFolds mergeUnwindRecords(COFFLinkerContext &ctx);
} // namespace lld::coff

#endif
