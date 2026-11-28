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

namespace lld::coff {

class COFFLinkerContext;

void doICF(COFFLinkerContext &ctx);

// After relaxation, where the link folds sections: folds the unwind records of
// resized code that are equal once their spans have their values. Returns the
// number it folded away, which the caller takes out of the layout.
size_t mergeUnwindRecords(COFFLinkerContext &ctx);
} // namespace lld::coff

#endif
