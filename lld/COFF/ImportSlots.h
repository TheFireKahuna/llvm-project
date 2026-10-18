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
struct SlotScanPending;

// Under -import-slots, the import a word holding s's address takes in place:
// an import thunk's import, or data resolved to its import; else null.
DefinedImportData *getAddressedImport(Symbol *s);

// The import a word holding s's address takes in place, if any. Never a
// delay-loaded import: its address is its thunk's until the DLL is loaded.
DefinedImportData *getInPlaceImport(Symbol *s);

// An executable with a copy of its thread-local import slots names the copy's
// list in its MZ header reserved words as {tag u32, rva u32} at this offset,
// so a DLL reaching it during process init can run the copy first.
inline constexpr uint32_t threadLocalSlotCopyOffset = 0x1C;
inline constexpr uint32_t threadLocalSlotCopyTag = 0x9E3779B9;

// A DLL whose vague entities another image may bind at run time, or that
// delay-loads a DLL, names its table of them in its MZ header reserved words as
// {tag u32, rva u32} at this offset, found through the loader's module list. An
// executable keeps its exit-list locator there and names its own table by RVA
// after it; the table starts with a magic of its own.
inline constexpr uint32_t vagueOfferTableOffset = 0x30;
inline constexpr uint32_t vagueOfferTableTag = 0x6C8E9CF5;
inline constexpr uint32_t executableOfferTableOffset = 0x38;
inline constexpr uint32_t vagueOfferTableMagic = 0x4F7A1D2B;

// The addend that the word of rel, an address as wide as the image's pointers,
// holds; none if the word extends past the end of sc.
std::optional<int64_t> getAddressWordAddend(SectionChunk *sc,
                                            const coff_relocation &rel);

// A word of static data holding an address inside an import whose DLL exports
// no name: the import's address plus addend. sym names the word's relocation.
struct ResidualWord {
  SectionChunk *chunk;
  uint32_t offset;
  DefinedImportData *imp;
  int64_t addend;
  Symbol *sym;
};

// Under -import-slots, an import bound by name at image start: its cell (read
// in place of an IAT entry), its name, and, for a weak DLL import, the DLL's.
struct RunTimeCell {
  DefinedImportData *imp;
  Chunk *cell;
  Chunk *name;
  Chunk *dllName;
};

// Under -import-slots, the words of static data holding imports' addresses:
// in-place import slots the loader writes, and residual words the image's fill
// writes. The writer lays out the held-back chunks and the fill's.
class ImportSlotContents {
public:
  ImportSlotContents(COFFLinkerContext &ctx, IdataContents &idata)
      : ctx(ctx), idata(idata) {}
  // Scans symtab's relocations once: decides which local import pointers a
  // reference still reads, and under -import-slots gathers what bind needs.
  void scan(SymbolTable &symtab);
  // Decides, once fused uses are bound, the in-place slots, the rewritten
  // address-takes and the imports' entries.
  void bind();
  // Leaves out, once code is relaxed, each thread-local variable's record the
  // image writes that nothing reads any longer.
  void leaveOutUnreadRecords();
  // Collects a DLL's cells: the words through which it reaches imported
  // overridable defaults and vague entities another image may preempt.
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
  Defined *getFill() const { return fill; }
  Chunk *getFillPointer() const { return fillPointer; }
  Chunk *getFillList() const { return fillList; }
  Chunk *getVagueOfferTable() const { return vagueOfferTable; }

  Defined *getThreadLocalSlotCopy() const { return threadLocalSlotCopy; }
  Chunk *getThreadLocalSlotCopyPointer() const {
    return threadLocalSlotCopyPointer;
  }
  Chunk *getThreadLocalSlotCopyList() const { return threadLocalSlotCopyList; }

private:
  void addRunTimeCells();
  void scanReference(SlotScan &scan, SectionChunk *sc, size_t &i,
                     SlotScanPending &p);
  void resolve(SlotScan &scan);
  void useThunkAsAddress(SlotScan &scan, ObjFile *file, Symbol *s,
                         DefinedImportData *imp, SectionChunk *data = nullptr);
  void addResidualWord(SectionChunk *sc, uint32_t offset, Symbol *s,
                       DefinedImportData *imp, int64_t addend);
  void addImport(DefinedImportData *imp);

  COFFLinkerContext &ctx;
  IdataContents &idata;
  SlotScan *slotScan = nullptr;
  // Read-only chunks holding in-place import slots, laid out with the address
  // tables; writable ones go together at .data's end. In their order there.
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
  // The builtins' fill, its pointer in the C initializer table and the list it
  // works through; and the table through which another DLL's search finds
  // this image's instances.
  Defined *fill = nullptr;
  Chunk *fillPointer = nullptr;
  Chunk *fillList = nullptr;
  Chunk *vagueOfferTable = nullptr;

  // The builtins' copy of an executable's thread-local slots, its pointer in
  // the TLS callback array and its list.
  Defined *threadLocalSlotCopy = nullptr;
  Chunk *threadLocalSlotCopyPointer = nullptr;
  Chunk *threadLocalSlotCopyList = nullptr;
  // Read-only output sections other than .rdata that hold in-place import
  // slots, laid out before .rdata, and whether a .rdata $-group holds one.
  // Either puts the address tables at .rdata's start so the directory covers
  // them all.
  llvm::SetVector<StringRef> slotSections;
  bool slotRdataGroups = false;
};
// Whether the bytes around the REL32 field at off match the form an object
// describes there, verifying the description that locates the instruction.
bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off,
                llvm::COFF::LinkSiteForm form);

// The form the object of sc describes for rel's instruction, if its bytes
// match; none for a site of no form.
std::optional<llvm::COFF::LinkSiteForm>
getDescribedSite(const SectionChunk *sc,
                 const llvm::object::coff_relocation &rel);

// Whether adrp and add (ARM64 relocations in sc against one symbol) compute
// the symbol's address with no addend, the pair lld/ELF can relax.
bool isArm64AddressPair(const SectionChunk *sc,
                        const llvm::object::coff_relocation &adrp,
                        const llvm::object::coff_relocation &add);

// Whether rel is the adrp, or a 64-bit ldr with no offset, of a load of a
// pointer.
bool isArm64PointerLoad(const SectionChunk *sc,
                        const llvm::object::coff_relocation &rel);

// Whether the relocation at index i of sc, a code section, is the field of a
// pointer load of the given form whose register only memory reads read, so the
// load may read another pointer to equal data. On ARM64 (an ldr) sets page to
// the adrp relocation distance instructions before it; otherwise to i.
bool isValueOnlyLoad(const SectionChunk *sc, size_t i,
                     llvm::COFF::LinkSiteForm form, uint16_t distance,
                     size_t &page);

// Whether the references of file to local import pointers are reported only
// once local imports are bound, and only if they still read the pointer.
bool defersLocalImportWarning(const ObjFile *file, bool arm64);

// Binds each .refptr.X pointer to a local import to X (in the image) or to
// zero (absent weak X), whose references bindLocalImports rewrites directly.
void bindPointerCells(SymbolTable &symtab);

// Decides, once local imports are bound, which fused calls go through their
// register, which accesses and adds reach data directly, and which loads are
// dead; makes the edits of each.
void bindFusedCalls(SymbolTable &symtab);

// Writes into the ARM64 chunk sc's contents at buf the fused accesses and adds
// bindFusedCalls decided reach the data directly, where the data-address
// offsets fit their fields.
void writeFusedArm64(const SectionChunk *sc, uint8_t *buf);
} // namespace lld::coff

#endif
