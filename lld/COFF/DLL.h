//===- DLL.h ----------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_DLL_H
#define LLD_COFF_DLL_H

#include "Chunks.h"
#include "Relocations.h"
#include "Symbols.h"
#include "llvm/ADT/StringMap.h"

namespace lld::coff {

// Windows-specific.
// IdataContents creates all chunks for the DLL import table.
// You are supposed to call add() to add symbols and then
// call create() to populate the chunk vectors.
class IdataContents {
public:
  void add(DefinedImportData *sym) { imports.push_back(sym); }
  bool empty() { return imports.empty(); }

  void create(COFFLinkerContext &ctx);

  std::vector<DefinedImportData *> imports;
  std::vector<Chunk *> dirs;
  std::vector<Chunk *> lookups;
  std::vector<Chunk *> addresses;
  std::vector<Chunk *> hints;
  std::vector<Chunk *> dllNames;
  std::vector<Chunk *> auxIat;
  std::vector<Chunk *> auxIatCopy;
};

// Where the delay-load IAT goes. Each value is the GuardFlags that tell the
// loader about it.
enum class DelayIatPlacement : uint32_t {
  // Unprotected, in .data.
  Data = 0,
  // Write-protected, in .idata's output section.
  Idata = uint32_t(llvm::COFF::GuardFlags::PROTECT_DELAYLOAD_IAT),
  // Write-protected, at the start of a writable .didat of its own.
  Didat = Idata |
          uint32_t(llvm::COFF::GuardFlags::DELAYLOAD_IAT_IN_ITS_OWN_SECTION),
};

// The delay-load IAT's placement, decided as link.exe does.
DelayIatPlacement getDelayIatPlacement(COFFLinkerContext &ctx);

// The names of what the link gives dll, a delay-loaded DLL with a gate: its
// ensure stub, its gate, and the thunk that the gate holds until it is bound.
// An object that refers to one of them asks for the gate; defines them for
// the references where define says so.
bool namesDelayGate(COFFLinkerContext &ctx, StringRef dll, bool define = false);

// The guard of the x86-64 sites that read one value or replay one
// instruction: a value is the address of a delay-loaded function, which its
// entry holds, or what a delay-bound section holds or its address. Each DLL
// whose words a site may reach has a gate, the thunk the gate holds until
// bound and an ensure stub. The leaf that a site of the same size calls
// saves its scratch, tests each gate without the flags, calls the ensure stub
// of one that is unbound and then leaves the value in the site's register,
// or restores its scratch and runs the site's instruction after its
// function's end, which a leaf without a function table entry is; it is
// empty while no site calls it.
class DelayGuardChunk : public NonSectionCodeChunk {
public:
  struct Gate {
    Defined *gate, *thunk, *ensure;
  };
  DelayGuardChunk(const DelayGuardSite &g, uint8_t reg,
                  SmallVector<Gate, 1> gates)
      : value(g.value), addend(g.addend), address(g.address), reg(reg),
        before(g.before), insn(g.insn), gates(std::move(gates)) {}
  // A gate's test is 24 bytes and its call of the ensure stub 7. REX2 adds
  // one to each load of a gate, each test's lea and the value's instruction;
  // a replay pushes and pops two registers, jumps to the instruction and
  // returns from it.
  size_t getSize() const override {
    size_t n = gates.size();
    return !called       ? 0
           : insn.empty() ? 41 + 31 * (n - 1) + (2 * n + 1) * (reg >> 4)
                          : 7 + 31 * n + insn.size();
  }
  // Where the function the leaf's table entry covers ends.
  size_t getEnd() const {
    return getSize() - (insn.empty() ? 0 : insn.size() + 1);
  }
  void writeTo(uint8_t *buf) const override;

  Defined *value;
  int32_t addend;
  bool address;
  uint8_t reg, before;
  bool called = false;
  StringRef insn;
  SmallVector<Gate, 1> gates;
};

// The guard of site g, as the symbol of its leaf, which called says that a
// site calls.
Defined *getDelayGuard(COFFLinkerContext &ctx, const DelayGuardSite &g,
                       bool called);
inline DelayGuardChunk *toDelayGuard(Symbol *leaf) {
  return static_cast<DelayGuardChunk *>(
      cast<DefinedSynthetic>(leaf)->getChunk());
}

// The guard of an ARM64 adrp, relocation index of sc, of a delay-loaded
// function's address into reg, placed after sc. A site in code that moves is
// the gate's adrp and ldr and a tbnz of its bit 0 to the guard's +12, ahead
// of the adrp; any other is a b to the guard, which tests the gate itself.
// Unbound, the guard saves x30 and the site's address, calls the ensure stub
// and restores x30; then it is the site's adrp of target and a b back.
class DelayGuardA64Chunk : public NonSectionCodeChunk {
public:
  DelayGuardA64Chunk(SectionChunk *sc, uint32_t index, Defined *gate,
                     Defined *ensure, Defined *target, uint8_t reg, bool far)
      : sc(sc), index(index), gate(gate), ensure(ensure), target(target),
        reg(reg), far(far) {
    setAlignment(4);
  }
  size_t getSize() const override { return 36; }
  MachineTypes getMachine() const override { return ARM64; }
  void writeTo(uint8_t *buf) const override;

  SectionChunk *sc;
  uint32_t index;
  Defined *gate, *ensure, *target;
  uint8_t reg;
  // Whether an inline site reaches the guard by tbz over a b.
  bool far;
  // The unwind codes that ntdll runs for the site.
  std::string codes;
};

// The guard of site s, an ARM64 adrp of a delay-loaded function's entry, as
// the symbol of its chunk; far as DelayGuardA64Chunk states it. None where
// the site's function has packed unwind data, which has no codes to read.
Defined *addDelayGuardA64(COFFLinkerContext &ctx, const DelayGuardSite &s,
                          bool far);

// Leaves ImportFile::observed set on the functions of delay-loaded DLLs whose
// address is their DLL's own in this image: those an object observes, where
// the loader's protected path binds the image's delay-load tables and the DLL
// does not keep its thunks' addresses.
// Leaves sym, a function that the link asked the runtime for, out of the
// image, which has it again once markLive is given it as a root: only the
// link refers to such a function, where the image has a fact that needs it.
void dropBuiltin(Symbol *sym);

void selectDelayBoundImports(COFFLinkerContext &ctx);

// Decides who binds each import, the loader or the image's fill by name,
// and whether the link delay-loads it.
void classifyImports(COFFLinkerContext &ctx);

// Decides, once the scan has found what the image's fill writes, whether the
// image has the runtime's fill, which runs as it starts, and keeps it with
// what it reaches, giving kept the sections this adds to the image.
void selectFill(COFFLinkerContext &ctx,
                llvm::SmallVectorImpl<SectionChunk *> &kept);

// Windows-specific.
// DelayLoadContents creates all chunks for the delay-load DLL import table.
//
// A delay-loaded function whose address is its DLL's own has it in three
// kinds of word, all of which hold a thunk of the image until the DLL is
// bound. Its entry of the DLL's address table is its cell, which code loads.
// A word of read-only data that holds the address belongs to a run: words of
// one DLL a word apart, the address table of a delay descriptor of their own,
// which the loader binds in place as it binds any table, sizing it by the
// first zero word after it and leaving its pages read-only. A word of
// writable data is exchanged for the cell's value by the runtime. A DLL with
// any such word or loaded cell has a gate, an entry with a descriptor of its
// own that is bound last, so that it tells whether all of them are; a list
// of what to bind, in order; and an ensure stub, which keeps every register
// and calls the runtime with the list.
class DelayLoadContents {
public:
  DelayLoadContents(COFFLinkerContext &ctx) : ctx(ctx) {}
  void add(DefinedImportData *sym) { imports.push_back(sym); }
  bool empty() { return imports.empty(); }
  void create();
  std::vector<Chunk *> getChunks();
  // Takes the words of read-only data, those of the sections that the image
  // gathers first and then those of the sections that keep their place, each
  // section's together, and the words of writable data, and forms the runs.
  void placeWords(std::vector<ImportSlot> readOnly, size_t gathered,
                  std::vector<ImportSlot> writable);
  // The gathered sections in their order, with the padding between and
  // around them, which begin and end on a page.
  ArrayRef<Chunk *> getRunChunks() { return runChunks; }
  // The sections that keep their place and hold no zero word after their
  // last run, each with the zero word that is to follow it.
  ArrayRef<std::pair<SectionChunk *, Chunk *>> getPlacedPads() {
    return placedPads;
  }
  // Whether the image has to add a zero word after the section of last, the
  // last of its words that hold such an address.
  static bool endsSection(const ImportSlot &last);
  // The words of the runs, run by run.
  ArrayRef<ImportSlot> getRunWords() { return runs; }
  // Reports the words that the image's layout left where they cannot be
  // bound.
  void checkWords();
  ArrayRef<Chunk *> getModuleHandles() { return moduleHandles; }
  ArrayRef<Chunk *> getAddresses() { return addresses; }
  ArrayRef<Chunk *> getCodeChunks() { return thunks; }
  ArrayRef<Chunk *> getCodePData() { return pdata; }
  ArrayRef<Chunk *> getCodeUnwindInfo() { return unwindinfo; }
  ArrayRef<Chunk *> getAuxIat() { return auxIat; }
  ArrayRef<Chunk *> getAuxIatCopy() { return auxIatCopy; }

  uint64_t getDirRVA() { return dirs[0]->getRVA(); }
  uint64_t getDirSize();

private:
  Chunk *newThunkChunk(DefinedImportData *s, Chunk *tailMerge);
  Chunk *newTailMergeChunk(SymbolTable &symtab, Chunk *dir);
  Chunk *newTailMergePDataChunk(SymbolTable &symtab, Chunk *tm);
  Chunk *newEnsureChunk(Chunk *list);

  std::vector<DefinedImportData *> imports;
  std::vector<Chunk *> dirs;
  std::vector<Chunk *> moduleHandles;
  std::vector<Chunk *> addresses;
  std::vector<Chunk *> names;
  std::vector<Chunk *> hintNames;
  std::vector<Chunk *> thunks;
  std::vector<Chunk *> pdata;
  std::vector<Chunk *> unwindinfo;
  std::vector<Chunk *> dllNames;
  std::vector<Chunk *> auxIat;
  std::vector<Chunk *> auxIatCopy;
  std::vector<Chunk *> unloads;
  std::vector<Chunk *> lists;
  std::vector<ImportSlot> runs, words;
  std::vector<Chunk *> runChunks;
  llvm::DenseMap<Chunk *, Chunk *> runPads;
  std::vector<std::pair<SectionChunk *, Chunk *>> placedPads;
  Chunk *ensureUnwind = nullptr;

  COFFLinkerContext &ctx;
};

// Gives an image whose fill has something to write the list that the
// runtime's fill walks, with sealed the section of the words that are the
// fill's own.
void createFill(COFFLinkerContext &ctx, OutputSection *rdata,
                OutputSection *sealed);

// Create all chunks for the DLL export table.
void createEdataChunks(SymbolTable &symtab, std::vector<Chunk *> &chunks);

} // namespace lld::coff

#endif
