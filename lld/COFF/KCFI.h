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
  // Whether an object's KCFI lists name imports by their import address
  // table entries alone.
  bool listsImports = false;

  // The __cfi_ symbols of the KCFI prefixes whose word before the marker is a
  // membership tag rather than a second type.
  std::vector<Symbol *> memberTags;

  // The KCFI thunks that objects' records describe, in the order the objects
  // were read, with the facts of the first copy of each.
  struct Thunk {
    ObjFile *file;
    llvm::COFF::LinkKCFIThunkKind kind;
    uint32_t type;
    uint32_t marker;
    uint32_t offset;
    Symbol *mismatch;
  };
  llvm::MapVector<Symbol *, Thunk> thunks;

  // The routines and list words of the KCFI types that openKCFITypes opened,
  // each to be placed in its section.
  std::vector<Chunk *> chunks;
  // The imports whose address those lists name by its import address table
  // entry, which the open routines read, each with its list's section.
  std::vector<std::pair<DefinedImportData *, StringRef>> listedImports;
  // The routines that openKCFITypes made dynamic only because foreign code in
  // the image, which references no import, reaches them, each with the static
  // scanner of its kind.
  std::vector<std::pair<KCFIOpenChunk *, Defined *>> localRoutines;

  // A DLL whose objects have KCFI prefixes exports the bounds of its KCFI code
  // range, which these symbols take, and its import library describes the
  // range by the KCFI types, and second types, of the unsealed functions in
  // it, with how many carry each, when the range is not empty.
  Defined *rangeExports[2] = {};
  bool rangeDefined = false;
  llvm::MapVector<uint32_t, uint32_t> rangeTypes;
  llvm::MapVector<uint32_t, uint32_t> rangeVfnTypes;

  // The KCFI code ranges of the DLLs the image imports statically, in load
  // order, that its KCFI checks may take a target inside directly, by the
  // imports of each range's bounds; and by type, and by second type, the
  // ranges that a thunk of the type tests, as indices of those.
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

// A DLL whose objects have KCFI prefixes exports the bounds of its KCFI code
// range.
void addKCFIRangeExports(COFFLinkerContext &ctx);

// A sealed image imports the bounds of the KCFI code ranges of the DLLs it
// imports statically, where its thunks may test them.
void bindKCFIImportedRanges(COFFLinkerContext &ctx);

// Adds to the import library of a DLL with a KCFI code range, which exports,
// as the writer would put them in the library, describe, the record of the
// range and the imports of its bounds, the first held in buffer.
void addKCFIRangeImports(COFFLinkerContext &ctx, StringRef dllName,
                         std::vector<llvm::object::COFFShortExport> &exports,
                         std::vector<llvm::NewArchiveMember> &members,
                         std::vector<uint8_t> &buffer);

// Whether rel, in sc, is an entry of a KCFI type's list, the address of a word
// that the type's open routine reads whole.
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
  void replaceThunks();
  void boundMismatches(OutputSection *textSec, OutputSection *rdataSec);
  void narrowMemberMisses();

  COFFLinkerContext &ctx;
  TypePrefixContents &prefixes;
  // The output section that holds the code range, if there is one, and the
  // bounds of the range in it.
  OutputSection *kcfiCodeSec = nullptr;
  Defined *kcfiCodeStart = nullptr;
  Defined *kcfiCodeEnd = nullptr;
  // The first chunk after the range, or null if the range ends the section.
  Chunk *kcfiCodeEndChunk = nullptr;
  // Whether a KCFI thunk the linker made tests the range by its size.
  bool kcfiThunksTestRange = false;
};
} // namespace lld::coff

#endif
