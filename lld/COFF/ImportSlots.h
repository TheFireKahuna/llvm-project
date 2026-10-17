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

// An executable with a copy of its thread-local import slots names it in the
// reserved words of its MZ header, as {tag u32, rva u32} at this offset, so
// that the code of a DLL that may reach the executable while the process
// initializes can run it first.
inline constexpr uint32_t threadLocalSlotCopyOffset = 0x1C;
inline constexpr uint32_t threadLocalSlotCopyTag = 0x9E3779B9;

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
// which the residual fill writes. The writer lays out the chunks it holds
// back, and the fill's.
class ImportSlotContents {
public:
  ImportSlotContents(COFFLinkerContext &ctx, IdataContents &idata)
      : ctx(ctx), idata(idata) {}
  void bind();
  void createResidualFill();
  // Builds the fill of a DLL's overridable-default cells, the words through
  // which it reaches the defaults it imports.
  void createOverrideCellFill();
  // Builds the copy of an executable's thread-local import slots into the main
  // thread's block.
  void createThreadLocalSlotCopy();
  // Builds a DLL's run of the copy of the thread-local slots that the
  // executable it imports from names.
  void createThreadLocalSlotCopyCall();
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
  Chunk *getFill() const { return residualFill; }
  Chunk *getFillPointer() const { return residualFillPointer; }
  Chunk *getFillUnwind() const { return residualFillUnwind; }
  Chunk *getFillPdata() const { return residualFillPdata; }
  Chunk *getOverrideCellFill() const { return overrideCellFill; }
  Chunk *getOverrideCellFillPointer() const { return overrideCellFillPointer; }
  Chunk *getOverrideCellFillUnwind() const { return overrideCellFillUnwind; }
  Chunk *getOverrideCellFillPdata() const { return overrideCellFillPdata; }
  Chunk *getThreadLocalSlotCopy() const { return threadLocalSlotCopy; }
  Chunk *getThreadLocalSlotCopyPointer() const {
    return threadLocalSlotCopyPointer;
  }
  Chunk *getThreadLocalSlotCopyCall() const { return threadLocalSlotCopyCall; }
  Chunk *getThreadLocalSlotCopyCallPointer() const {
    return threadLocalSlotCopyCallPointer;
  }
  Chunk *getThreadLocalSlotCopyCallUnwind() const {
    return threadLocalSlotCopyCallUnwind;
  }
  Chunk *getThreadLocalSlotCopyCallPdata() const {
    return threadLocalSlotCopyCallPdata;
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
  // The words of static data that the residual fill writes, and the read-only
  // chunks among theirs, which are laid out in .sealed, in their order there.
  std::vector<ResidualWord> residualWords;
  llvm::SetVector<SectionChunk *> sealedChunks;
  // The imports the residual fill binds by name, which are laid out in
  // .sealed before sealedChunks, and the names it binds them by.
  std::vector<RunTimeCell> runTimeCells;
  std::vector<Chunk *> runTimeNames;
  // The imports some of whose words take an interior name instead, with the
  // first such word.
  llvm::MapVector<ImportFile *, std::pair<SectionChunk *, uint32_t>>
      interiorBases;
  // The residual fill, its pointer in the C initializer table and, when it has
  // a frame, its unwind information and exception table entry.
  Chunk *residualFill = nullptr;
  Chunk *residualFillPointer = nullptr;
  Chunk *residualFillUnwind = nullptr;
  Chunk *residualFillPdata = nullptr;
  // The fill of this image's overridable-default cells, its pointer in the C
  // initializer table, and, when it has a frame, its unwind information and
  // exception table entry.
  Chunk *overrideCellFill = nullptr;
  Chunk *overrideCellFillPointer = nullptr;
  Chunk *overrideCellFillUnwind = nullptr;
  Chunk *overrideCellFillPdata = nullptr;
  // The copy of an executable's thread-local slots and its pointer in the C
  // initializer table.
  Chunk *threadLocalSlotCopy = nullptr;
  Chunk *threadLocalSlotCopyPointer = nullptr;
  // A DLL's run of that copy, its pointer in the C initializer table, and, on
  // AArch64, where it has a frame, its unwind information and exception table
  // entry.
  Chunk *threadLocalSlotCopyCall = nullptr;
  Chunk *threadLocalSlotCopyCallPointer = nullptr;
  Chunk *threadLocalSlotCopyCallUnwind = nullptr;
  Chunk *threadLocalSlotCopyCallPdata = nullptr;
  // The read-only output sections other than .rdata that hold in-place import
  // slots, which are laid out before .rdata, and whether a $-group of .rdata
  // holds one; either places the import address tables at the start of
  // .rdata, so that the directory covers them all.
  llvm::SetVector<StringRef> slotSections;
  bool slotRdataGroups = false;
};
} // namespace lld::coff

#endif
