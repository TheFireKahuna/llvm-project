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
namespace object {
struct COFFShortExport;
} // namespace object
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

// What the KCFI checks of a link need from one phase to the next.
struct KCFIState {
  // Whether objects' KCFI lists name imports by their IAT entry alone.
  bool listsImports = false;

  // __cfi_ symbols whose word before the marker is a membership tag, not a
  // second type.
  std::vector<Symbol *> memberTags;

  // The KCFI thunks objects' records describe, in read order, with the first
  // copy's facts.
  struct Thunk {
    ObjFile *file;
    llvm::COFF::LinkKCFIThunkKind kind;
    uint32_t type;
    uint32_t marker;
    uint32_t offset;
    Symbol *mismatch;
  };
  llvm::MapVector<Symbol *, Thunk> thunks;

  // The routines and list words openKCFITypes opened, each for its section.
  std::vector<Chunk *> chunks;
  // The imports those lists name by IAT entry (read by the open routines), each
  // with its list's section.
  std::vector<std::pair<DefinedImportData *, StringRef>> listedImports;
  // The KCFI types a function in another image may validly target (opened for
  // an import, or dynamically beyond just import-free in-image foreign code).
  llvm::DenseSet<uint32_t> outsideTypes;
  // Routines made dynamic only because import-free in-image foreign code
  // reaches them, each with the static scanner of its kind.
  std::vector<std::pair<KCFIOpenChunk *, Defined *>> localRoutines;

  // A DLL with KCFI prefixes exports its code-range bounds (these symbols) and,
  // when the range is non-empty, its import library records the types and
  // second types of the unsealed functions in it, with how many carry each.
  Defined *rangeExports[2] = {};
  bool rangeDefined = false;
  llvm::MapVector<uint32_t, uint32_t> rangeTypes;
  llvm::MapVector<uint32_t, uint32_t> rangeVfnTypes;

  // The code ranges of statically imported DLLs, in load order, that KCFI
  // checks may take a target inside directly, by each range's bound imports;
  // and, by type and second type, the ranges a thunk of the type tests, as
  // indices.
  struct ImportedRange {
    DefinedImportData *start;
    DefinedImportData *end;
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

// Adds to a DLL's import library the record of its KCFI code range and the
// imports of its bounds (the record held in buffer), given its exports.
void addKCFIRangeImports(COFFLinkerContext &ctx, StringRef dllName,
                         std::vector<llvm::object::COFFShortExport> &exports,
                         std::vector<llvm::NewArchiveMember> &members,
                         std::vector<uint8_t> &buffer);

// Whether rel in sc is a KCFI list entry: the address of a word the type's open
// routine reads whole.
bool isKCFIListEntry(SectionChunk *sc,
                     const llvm::object::coff_relocation &rel);

// The imports whose import address table entries KCFI's routines read whole,
// and those whose entries its thunks use otherwise.
void getKCFIImportUses(COFFLinkerContext &ctx,
                       llvm::SmallVectorImpl<DefinedImportData *> &read,
                       llvm::SmallVectorImpl<DefinedImportData *> &other);

// The KCFI thunks, routines and lists the writer adds, replaces and completes,
// and the code range it defines, in an image whose prefixes it seals.
class KCFIContents {
public:
  KCFIContents(COFFLinkerContext &ctx, TypePrefixContents &prefixes)
      : ctx(ctx), prefixes(prefixes) {}
  void listThunks();
  void bound(OutputSection *textSec, OutputSection *rdataSec);
  void layOut(OutputSection *textSec, OutputSection *rdataSec);
  void checkCodeRange();

private:
  void defineCodeRange();
  void placeCodeRange();
  void defineRangeExports(OutputSection *textSec, OutputSection *rdataSec);
  const Chunk *getMismatchRoutine(Symbol *mismatch) const;
  void placeMismatchRoutines();
  void replaceThunks();
  void boundMismatches(OutputSection *textSec, OutputSection *rdataSec);
  void narrowMemberMisses();

  COFFLinkerContext &ctx;
  TypePrefixContents &prefixes;
  // The output section holding the code range, if any, with its bounds in it.
  OutputSection *kcfiCodeSec = nullptr;
  Defined *kcfiCodeStart = nullptr;
  Defined *kcfiCodeEnd = nullptr;
  // The first chunk after the range, or null if the range ends the section.
  Chunk *kcfiCodeEndChunk = nullptr;
  // Whether a KCFI thunk the linker made tests the range by its size.
  bool kcfiThunksTestRange = false;
  // The chunks the linker opened for KCFI types, used to recognise a synthetic
  // mismatch routine without a per-thunk scan.
  llvm::DenseSet<const Chunk *> openChunks;
};
} // namespace lld::coff

#endif
