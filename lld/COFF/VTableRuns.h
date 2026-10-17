//===- VTableRuns.h -----------------------------------------------*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_VTABLERUNS_H
#define LLD_COFF_VTABLERUNS_H

#include "llvm/ADT/STLFunctionalExtras.h"

namespace lld::coff {
class COFFLinkerContext;
class OutputSection;
class SectionChunk;

// Builds the vtable runs the objects' records ask for, in
// COFFLinkerContext::vtableRuns, and chooses each run's stride. A run holds the
// vtables of a class and of the classes derived from it through primary bases.
void resolveVTableRuns(COFFLinkerContext &ctx);

// Defines, as an empty range, each vtable range symbol an object references
// before the runs are known, so a guard thunk's reference is not undefined.
// defineVTableRuns gives the ranges the runs place their real values.
void registerVTableRunSymbols(COFFLinkerContext &ctx);

// Reorders the read-only section so that each run's vtables are consecutive and
// in the order their slots want, which assignAddresses then places at the run's
// stride.
void placeVTableRuns(COFFLinkerContext &ctx, OutputSection *rdataSec,
                     llvm::function_ref<bool(const SectionChunk *)> heldBack);

// Defines the range symbols the guard thunks test, once addresses are final: a
// run whose vtables the layout did not place at its stride is given an empty
// range, so its calls fall back to the slow path. Any range symbol no run
// defines is made an empty range too.
void defineVTableRuns(COFFLinkerContext &ctx);

} // namespace lld::coff

#endif
