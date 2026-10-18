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

// Builds the vtable runs the objects' records ask for in ctx.vtableRuns and
// chooses each run's stride. A run holds a class's vtables and those of the
// classes derived from it through primary bases.
void resolveVTableRuns(COFFLinkerContext &ctx);

// Defines each referenced vtable range symbol as an empty range before the runs
// are known, so a guard thunk's reference is not undefined; defineVTableRuns
// gives them their real values.
void registerVTableRunSymbols(COFFLinkerContext &ctx);

// Reorders the read-only section so each run's vtables are consecutive in slot
// order for assignAddresses to place at the run's stride, unless packing laid
// them out already.
void placeVTableRuns(COFFLinkerContext &ctx, OutputSection *rdataSec,
                     llvm::function_ref<bool(const SectionChunk *)> heldBack,
                     bool packed);

// Defines the range symbols the guard thunks test, once addresses are final. A
// run the layout did not place at its stride, or a symbol no run defines, gets
// an empty range, so its calls take the slow path.
void defineVTableRuns(COFFLinkerContext &ctx);

// Under -import-slots, a DLL exports the run of each class whose vtable or
// type_info it exports, as three words (start, end, start plus stride less one)
// an importer tests vtable pointers of the DLL's objects against. Start is
// named by the class's address point, the others by names hashed from class.
void addVTableRunExports(COFFLinkerContext &ctx);

// Adds to a DLL's import library the imports of each exported run's words,
// named for the class's range symbols so an importer finds them by class.
void addVTableRunImports(COFFLinkerContext &ctx, llvm::StringRef dllName,
                         std::vector<llvm::object::COFFShortExport> &exports);

// Binds the owner run of each class the image's checks read to the first DLL it
// imports statically whose import library holds it and from which it imports
// the class's vtable or type_info; every other class gets an empty range.
void bindVTableOwnerRuns(COFFLinkerContext &ctx);

} // namespace lld::coff

#endif
