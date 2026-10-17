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

// What the ABI contracts of a link need from one phase to the next. A contract
// is the layout of a type, or of the type of a datum, that an object provides
// to other images or requires of them; its locator and digest are the
// base64url text by which a DLL's exports name it.
struct ContractState {
  struct Contract {
    bool data;
    StringRef identity;
    StringRef locator;
    StringRef digest;
  };

  // The contract of each identity that the link's objects provide or
  // require, by kind and locator, with the first file to give it.
  struct Known {
    StringRef digest;
    InputFile *file;
    StringRef identity;
  };
  llvm::StringMap<Known> known;

  // The witnesses of the contracts that each file's definitions provide, the
  // names a DLL exports them by, in the order the files were loaded.
  llvm::MapVector<InputFile *, SmallVector<StringRef, 0>> provides;

  // Which of the symbols of each bitcode file that provides contracts the file
  // defines, as its symbol table says, which LTO takes.
  llvm::DenseMap<InputFile *, llvm::BitVector> bitcodeDefinitions;

  // The contracts that the code in each section of an object requires of the
  // image that provides a symbol, by section number.
  struct Requirement {
    uint32_t section;
    Symbol *sym;
    SmallVector<const Contract *, 2> contracts;
  };
  llvm::MapVector<ObjFile *, std::vector<Requirement>> requirements;

  // The byte that every witness the image exports names.
  Defined *witness = nullptr;
};

// Reads a /abiprovide: or, with data, /abiprovidedata: directive of file.
void addContractProvides(COFFLinkerContext &ctx, InputFile *file,
                         StringRef rows, bool data);

// Reads the contracts that an object's references require, from its
// LinkRecordContracts group, returning false if the group is malformed.
bool readContractRequirements(ObjFile *file, ArrayRef<uint8_t> payload);

// A DLL exports a witness of each contract that the objects whose definitions
// it exports provide.
void addContractWitnessExports(COFFLinkerContext &ctx);

// Whether e is the export of a witness, which shares its ordinal with every
// other.
bool isContractWitness(COFFLinkerContext &ctx, const Defined *sym);

// Adds to the import library of a DLL, whose exports, as the writer would put
// them in the library, describe, the imports of its witnesses, named for the
// DLL.
void addContractWitnessImports(
    COFFLinkerContext &ctx, StringRef dllName,
    std::vector<llvm::object::COFFShortExport> &exports);

// An image imports the witness of each contract that its code requires of a
// DLL that exports one for the contract's identity, and a link that requires
// a contract a DLL exports another for is an error.
void bindContractWitnesses(COFFLinkerContext &ctx);
} // namespace lld::coff

#endif
