//===- KCFI.h ---------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_KCFI_H
#define LLD_COFF_KCFI_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include <vector>

namespace llvm {
struct NewArchiveMember;
} // namespace llvm

namespace lld::coff {
class COFFLinkerContext;
class Chunk;
class Defined;
class DefinedImportData;
class KCFIOpenChunk;
class ObjFile;
class OutputSection;
class SectionChunk;
class Symbol;
class SymbolTable;
class TypePrefixContents;

// What the KCFI checks of a link carry from one phase to the next.
struct KCFIState {
  // Whether objects' KCFI lists name imports by their IAT entry alone.
  bool listsImports = false;
  // __cfi_ symbols whose word before the marker is a membership tag.
  std::vector<Symbol *> memberTags;
  // The KCFI thunks and member misses objects' records describe, in read
  // order, with the first copy's facts.
  struct Thunk {
    ObjFile *file;
    llvm::COFF::LinkKCFIThunkKind kind;
    uint32_t type, marker, offset;
    Symbol *mismatch;
  };
  llvm::MapVector<Symbol *, Thunk> thunks;

  // The routines and list words openKCFITypes made; the imports its lists
  // name by IAT entry, each with its list's section; the types a function in
  // another image may validly target; and the routines only import-free
  // foreign code in the image opened, each with its static scanner.
  std::vector<Chunk *> chunks;
  std::vector<std::pair<DefinedImportData *, StringRef>> listedImports;
  llvm::DenseSet<uint32_t> outsideTypes;
  std::vector<std::pair<KCFIOpenChunk *, Defined *>> localRoutines;

  // A DLL's exported code-range bounds, whether it has a range, and the types
  // and second types of the unsealed functions in it, with how many carry each.
  Defined *rangeExports[2] = {};
  bool rangeDefined = false;
  llvm::MapVector<uint32_t, uint32_t> rangeTypes[2];

  // The code ranges of statically imported DLLs that a thunk may take a target
  // inside directly, in load order, and by type and second type the indices of
  // those a thunk of the type tests.
  struct ImportedRange {
    DefinedImportData *start, *end;
  };
  std::vector<ImportedRange> importedRanges;
  llvm::DenseMap<uint32_t, llvm::SmallVector<uint32_t, 4>> rangeHolders[2];
};

// In a link whose objects have KCFI prefixes, opens the KCFI types through
// which code without a prefix, which the link brings in, can be reached.
void openKCFITypes(SymbolTable &symtab);

// A DLL with KCFI prefixes exports the bounds of its KCFI code range.
void addKCFIRangeExports(COFFLinkerContext &ctx);

// A sealed image imports the bounds of the KCFI code ranges of the DLLs it
// imports statically, where its thunks may test them.
void bindKCFIImportedRanges(COFFLinkerContext &ctx);

// Adds to a DLL's import library the record of its KCFI code range, held in
// buffer.
void addKCFIRangeRecord(COFFLinkerContext &ctx, StringRef dllName,
                        std::vector<llvm::NewArchiveMember> &members,
                        std::vector<uint8_t> &buffer);

// Whether rel in sc is a KCFI list entry: the address of a word the type's open
// routine reads whole.
bool isKCFIListEntry(SectionChunk *sc,
                     const llvm::object::coff_relocation &rel);

// The KCFI thunks, routines and lists the writer adds, replaces and completes,
// and the code range it defines, in an image whose prefixes it seals.
class KCFIContents {
public:
  KCFIContents(COFFLinkerContext &ctx, TypePrefixContents &prefixes)
      : ctx(ctx), prefixes(prefixes) {}
  void listThunks();
  void layOut(OutputSection *textSec, OutputSection *rdataSec);
  void bound(OutputSection *textSec, OutputSection *rdataSec);
  void checkCodeRange();

private:
  void defineCodeRange();
  void replaceThunks();

  COFFLinkerContext &ctx;
  TypePrefixContents &prefixes;
  // The output section holding the code range, if any, the range's bounds,
  // and the first chunk after it, or null if it ends the section.
  OutputSection *codeSec = nullptr;
  Symbol *codeStart = nullptr;
  Symbol *codeEnd = nullptr;
  Chunk *codeEndChunk = nullptr;
  // Whether a KCFI thunk the linker made tests the range by its size.
  bool thunksTestRange = false;
};
} // namespace lld::coff

#endif
