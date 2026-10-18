//===- Contracts.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_CONTRACTS_H
#define LLD_COFF_CONTRACTS_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include <vector>

namespace llvm::object {
struct COFFShortExport;
} // namespace llvm::object

namespace lld::coff {
class COFFLinkerContext;
class Defined;
class InputFile;
class ObjFile;
class Symbol;

// ABI-contract state carried across link phases. A contract is a type's (or a
// datum type's) layout, named by base64url locator and digest in DLL witnesses.
struct ContractState {
  struct Contract {
    bool data;
    StringRef identity;
    StringRef locator;
    StringRef digest;
  };

  // A provided or required contract by kind and locator, with its first file.
  struct Known {
    StringRef digest;
    InputFile *file;
    StringRef identity;
  };
  llvm::StringMap<Known> known;

  // Per file, the witness names its definitions provide, in load order.
  llvm::MapVector<InputFile *, SmallVector<StringRef, 0>> provides;

  // Which symbols each bitcode file defines, recorded before LTO consumes it.
  llvm::DenseMap<InputFile *, llvm::BitVector> bitcodeDefinitions;

  // Per object, the contracts its code requires of a provider, by section.
  struct Requirement {
    uint32_t section;
    Symbol *sym;
    SmallVector<const Contract *, 2> contracts;
  };
  llvm::MapVector<ObjFile *, std::vector<Requirement>> requirements;

  // The read-only byte every exported witness names.
  Defined *witness = nullptr;
};

// Reads a /abiprovide: (or /abiprovidedata: when data) directive.
void addContractProvides(COFFLinkerContext &ctx, InputFile *file,
                         StringRef rows, bool data);

// Reads an object's required contracts from its link records, or the error.
llvm::Error readContractRequirements(ObjFile *file, ArrayRef<uint8_t> payload);

// Exports a witness for each contract the image's exported definitions
// provide; a type_info object it owns is exported under its qualified name.
void addContractWitnessExports(COFFLinkerContext &ctx);

// Whether sym is the witness byte all witnesses export through one ordinal.
bool isContractWitness(COFFLinkerContext &ctx, const Defined *sym);

// Whether name names a witness, which code never calls: a plain witness or a
// type_info name qualified by its type's contract.
bool isContractWitnessName(StringRef name);

// Adds the imports of a DLL's witnesses, named for the DLL, to the exports the
// writer puts in its import library.
void addContractWitnessImports(
    COFFLinkerContext &ctx, StringRef dllName,
    std::vector<llvm::object::COFFShortExport> &exports);

// Imports the witness of each contract the image's code requires of a DLL that
// exports one; a DLL built with another layout makes the link fail.
void bindContractWitnesses(COFFLinkerContext &ctx);
} // namespace lld::coff

#endif
