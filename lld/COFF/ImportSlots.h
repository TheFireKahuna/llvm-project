//===- ImportSlots.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_IMPORTSLOTS_H
#define LLD_COFF_IMPORTSLOTS_H

#include "Chunks.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include <optional>
#include <vector>

namespace lld::coff {
class COFFLinkerContext;
class IdataContents;
class ImportFile;
class ObjFile;
struct SlotScan;

// Under -import-slots, the import whose address a word of static data holding
// the address of s takes in place: the import of an import thunk, or of data
// that resolved to its import; or null.
DefinedImportData *getAddressedImport(Symbol *s);

// The import that a word holding the address of s takes in place, if any,
// which a delay-loaded import never is: its address is its thunk's until its
// DLL is loaded.
DefinedImportData *getInPlaceImport(Symbol *s);

// An executable with a copy of its thread-local import slots names it in the
// reserved words of its MZ header, as {tag u32, rva u32} at this offset, so
// that the code of a DLL that may reach the executable while the process
// initializes can run it first.
inline constexpr uint32_t threadLocalSlotCopyOffset = 0x1C;
inline constexpr uint32_t threadLocalSlotCopyTag = 0x9E3779B9;

// A DLL whose vague entities another image may bind to at run time, or that
// delay-loads a DLL, names its table of them in the reserved words of its MZ
// header, as {tag u32, rva u32} at this offset, where a DLL that the
// executable's link did not see finds it through the loader's list of modules.
// An executable keeps its exit-list state's locator there instead, and names
// its own table, of the vague entities it exports, by its RVA after it; the
// table starts with a magic of its own.
inline constexpr uint32_t vagueOfferTableOffset = 0x30;
inline constexpr uint32_t vagueOfferTableTag = 0x6C8E9CF5;
inline constexpr uint32_t executableOfferTableOffset = 0x38;
inline constexpr uint32_t vagueOfferTableMagic = 0x4F7A1D2B;

// The addend that the word of rel, an address as wide as the image's pointers,
// holds; none if the word extends past the end of sc.
std::optional<int64_t> getAddressWordAddend(SectionChunk *sc,
                                            const coff_relocation &rel);

// A word of static data that holds an address inside an import for which the
// import's DLL exports no name: the import's address plus addend. Sym is the
// symbol that the word's relocation names.
struct ResidualWord {
  SectionChunk *chunk;
  uint32_t offset;
  DefinedImportData *imp;
  int64_t addend;
  Symbol *sym;
};

// Under -import-slots, an import bound by name when the image starts: its
// cell, which code reads in place of an import address table entry, the
// import's name, and, for a weak import from a DLL, the DLL's name in UTF-16.
struct RunTimeCell {
  DefinedImportData *imp;
  Chunk *cell;
  Chunk *name;
  Chunk *dllName;
};

// Under -import-slots, the words of static data that hold imports' addresses:
// the in-place import slots, which the loader writes, and the residual words,
// which the image's fill writes. The writer lays out the chunks it holds back,
// and the fill's.
class ImportSlotContents {
public:
  ImportSlotContents(COFFLinkerContext &ctx, IdataContents &idata)
      : ctx(ctx), idata(idata) {}
  void bind();
  // Collects a DLL's cells, the words through which it reaches the
  // overridable defaults it imports and the vague entities another image's
  // instance may preempt.
  void collectCells();
  // Builds the table of the vague entities the image shares, through which a
  // DLL loaded at run time binds to its instances.
  void createOfferTable();
  // Builds the image's fill.
  void createFill();
  // Builds the copy of an executable's thread-local import slots into the main
  // thread's block.
  void createThreadLocalSlotCopy();
  void check(Chunk *iatStart, uint64_t iatSize);

  // Whether sc is laid out apart from the other chunks of its section.
  bool isHeldBack(const SectionChunk *sc) const {
    return (sc->hasImportSlots || !sealedChunks.empty()) &&
           heldBackChunks.contains(sc);
  }
  ArrayRef<SectionChunk *> getReadOnlyChunks() const { return slotChunks; }
  ArrayRef<SectionChunk *> getWritableChunks() const {
    return writableSlotChunks;
  }
  ArrayRef<SectionChunk *> getSealedChunks() const {
    return sealedChunks.getArrayRef();
  }
  const llvm::SetVector<StringRef> &getSlotSections() const {
    return slotSections;
  }
  bool hasRdataGroupSlots() const { return slotRdataGroups; }
  ArrayRef<RunTimeCell> getRunTimeCells() const { return runTimeCells; }
  // The names the fill resolves its cells by.
  ArrayRef<Chunk *> getRunTimeNames() const { return runTimeNames; }
  Chunk *getFill() const { return fill; }
  Chunk *getFillPointer() const { return fillPointer; }
  Chunk *getFillUnwind() const { return fillUnwind; }
  Chunk *getFillPdata() const { return fillPdata; }
  Chunk *getFillList() const { return fillList; }
  Chunk *getVagueOfferTable() const { return vagueOfferTable; }

  Chunk *getThreadLocalSlotCopy() const { return threadLocalSlotCopy; }
  Chunk *getThreadLocalSlotCopyPointer() const {
    return threadLocalSlotCopyPointer;
  }

private:
  void scanChunk(SlotScan &scan, SectionChunk *sc);
  void resolve(SlotScan &scan);
  void keepUndescribedFunctionAddresses();
  void useThunkAsAddress(SlotScan &scan, ObjFile *file, Symbol *s,
                         DefinedImportData *imp, SectionChunk *data = nullptr);
  void addResidualWord(SectionChunk *sc, uint32_t offset, Symbol *s,
                       DefinedImportData *imp, int64_t addend);
  void addImport(DefinedImportData *imp);

  COFFLinkerContext &ctx;
  IdataContents &idata;
  // The read-only chunks holding in-place import slots that are laid out with
  // the import address tables, and the writable ones of .data that are laid
  // out together at its end, in their order there.
  std::vector<SectionChunk *> slotChunks;
  std::vector<SectionChunk *> writableSlotChunks;
  // The imports whose address code reads from one of their in-place slots
  // instead of an import address table entry.
  std::vector<DefinedImportData *> slotReadImports;
  // The chunks laid out apart from the other chunks of their sections: those
  // above, and those of sealedChunks.
  llvm::DenseSet<const SectionChunk *> heldBackChunks;
  // The words of static data that the fill writes, and the read-only
  // chunks among theirs, which are laid out in .sealed, in their order there.
  std::vector<ResidualWord> residualWords;
  llvm::SetVector<SectionChunk *> sealedChunks;
  // The imports the fill binds by name, which are laid out in
  // .sealed before sealedChunks, and the names it binds them by.
  std::vector<RunTimeCell> runTimeCells;
  std::vector<Chunk *> runTimeNames;
  // The imports some of whose words take an interior name instead, with the
  // first such word.
  llvm::MapVector<ImportFile *, std::pair<SectionChunk *, uint32_t>>
      interiorBases;
  // The fill, its pointer in the C initializer table, its unwind information
  // and exception table entry, and the lists it works through; and the table
  // through which another DLL's search finds this image's instances.
  Chunk *fill = nullptr;
  Chunk *fillPointer = nullptr;
  Chunk *fillUnwind = nullptr;
  Chunk *fillPdata = nullptr;
  Chunk *fillList = nullptr;
  Chunk *vagueOfferTable = nullptr;

  // The copy of an executable's thread-local slots and its pointer in the C
  // initializer table.
  Chunk *threadLocalSlotCopy = nullptr;
  Chunk *threadLocalSlotCopyPointer = nullptr;
  // The read-only output sections other than .rdata that hold in-place import
  // slots, which are laid out before .rdata, and whether a $-group of .rdata
  // holds one; either places the import address tables at the start of
  // .rdata, so that the directory covers them all.
  llvm::SetVector<StringRef> slotSections;
  bool slotRdataGroups = false;
};
} // namespace lld::coff

#endif
