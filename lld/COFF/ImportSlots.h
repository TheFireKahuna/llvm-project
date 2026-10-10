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
  Chunk *getFill() const { return residualFill; }
  Chunk *getFillPointer() const { return residualFillPointer; }
  Chunk *getFillUnwind() const { return residualFillUnwind; }
  Chunk *getFillPdata() const { return residualFillPdata; }

private:
  void scanChunk(SlotScan &scan, SectionChunk *sc);
  void resolve(SlotScan &scan);
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
  // The read-only output sections other than .rdata that hold in-place import
  // slots, which are laid out before .rdata, and whether a $-group of .rdata
  // holds one; either places the import address tables at the start of
  // .rdata, so that the directory covers them all.
  llvm::SetVector<StringRef> slotSections;
  bool slotRdataGroups = false;
};
} // namespace lld::coff

#endif
