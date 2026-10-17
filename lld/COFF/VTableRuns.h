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
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace llvm::object {
struct COFFShortExport;
} // namespace llvm::object

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
// stride, unless the packing of the read-only section laid them out already.
void placeVTableRuns(COFFLinkerContext &ctx, OutputSection *rdataSec,
                     llvm::function_ref<bool(const SectionChunk *)> heldBack,
                     bool packed);

// Defines the range symbols the guard thunks test, once addresses are final: a
// run whose vtables the layout did not place at its stride is given an empty
// range, so its calls fall back to the slow path. Any range symbol no run
// defines is made an empty range too.
void defineVTableRuns(COFFLinkerContext &ctx);

// Under -import-slots, a DLL exports the run of each class whose vtable or
// type_info it exports, as three words that an importer tests a vtable pointer
// of an object the DLL built against: the run's start, under the name of the
// class's primary address point, and its end and its start plus the stride
// less one, under names hashed from the class.
void addVTableRunExports(COFFLinkerContext &ctx);

// Adds to a DLL's import library the imports of each exported run's words,
// named for the class's range symbols and the DLL, so that an importer finds
// them by the class's name.
void addVTableRunImports(COFFLinkerContext &ctx, llvm::StringRef dllName,
                         std::vector<llvm::object::COFFShortExport> &exports);

// Binds the run of each class whose owner run the image's checks read from the
// first DLL it imports statically whose import library holds it and from which
// it imports the class's vtable or type_info, and gives every other class an
// empty range.
void bindVTableOwnerRuns(COFFLinkerContext &ctx);

} // namespace lld::coff

#endif
