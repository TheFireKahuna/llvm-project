//===- Relocations.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_RELOCATIONS_H
#define LLD_COFF_RELOCATIONS_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Object/COFF.h"
#include <cstdint>
#include <memory>

namespace lld::aarch64 {
struct Ldr;
}

namespace lld::coff {
class COFFLinkerContext;
struct COFFSyncStream;
class Chunk;
class Defined;
class DefinedImportData;
class DefinedRegular;
class SectionChunk;
class Symbol;

// What the link does at a relocation of a section it rewrites, decided by the
// scan. R_TYPE applies the relocation as its type says.
enum RelExpr : uint8_t {
  R_TYPE,
  // Writes nothing: its instruction is another site's.
  R_NONE,
  // Alignment padding, an SREL32 field shifted by its target's movement, a
  // jump whose width the link chooses, a thread-local base.
  R_ALIGN,
  R_SHIFT,
  R_JUMP,
  R_TLS_BASE,
  // From here on the site's instructions are written in a form of their own.
  R_LOCAL_LOAD,
  R_LOCAL_CALL,
  R_LOCAL_JUMP,
  R_ABS_LOAD,
  R_ZERO_CALL,
  R_AARCH64_LOCAL,
  R_IMPORT_SLOT,
  // A word of data the image's fill writes, zero until then, which the loader
  // leaves.
  R_FILL_WORD,
  R_IMPORT_LOAD,
  // R_IMPORT_LOAD of a delay-loaded function behind its DLL's gate: inline,
  // or as a call of the guard's leaf in the load's bytes.
  R_GUARD,
  R_GUARD_CALL,
  R_LOCAL_ACCESS,
  // A record chain's site; its select's mov; an access onto the thread
  // pointer's register.
  R_TLS,
  R_TLS_MOVE,
  R_TLS_TP,
  R_DEAD,
  R_PAGEOFF,
  R_SITE_PAGE,
  // An ARM64 access of a pointer the section keeps but has no fused read of:
  // a literal load of what the pointer names where in reach, else as it is.
  R_LITERAL_ACCESS,
};

// A relocation of a section the link rewrites, index-aligned with the
// section's relocation table. gone says that a resized section's rewrite
// writes its bytes.
struct Relocation {
  uint32_t offset;
  uint16_t type;
  RelExpr expr;
  bool gone = false;
  Symbol *sym;
};

// What an edit of a resized section writes in place of the object's bytes:
// padding, a jump in the width the link chose or none, a direct call, a dead
// instruction or the one instruction of an ARM64 fold, an x86-64 xor of a
// register, room for an x86-64 access written at its relocation, a
// thread-local base site, an ARM64 pair folded or with a direct site's add, an
// x86-64 load behind a delay-load guard, an ARM64 instruction given the base
// of the adds of a thread-local's zero parts before it.
enum class EditKind : uint8_t {
  Align,
  Jump,
  Call,
  Dead,
  Zero,
  Grow,
  Scale,
  Pair,
  Guard,
  Rebase
};

// A point of a resizable section at which its size may change: the bytes
// [offset - before, offset - before + size) become newSize bytes. offset is
// the relocation's, where the shared core keys it.
struct Edit {
  uint32_t offset;
  // The relocation that states the site, or -1 for a jump a span states.
  uint32_t index;
  // The bytes removed up to and including it; negative where code grew.
  int32_t delta = 0;
  uint16_t size, newSize;
  uint8_t before;
  EditKind kind;
  // An ARM64 pair or dead read, or an x86-64 jcc and the tail jump it jumps
  // over, written as one instruction; or an ARM64 one that a layout unfolded.
  bool fold = false, unfolded = false;
  // A jcc that took the target of the jmp after it, whose bytes it holds.
  uint8_t skip = 0;
  // A jump to another chunk that a span states: the most bytes the chunks'
  // order may put between it and its target, or their complement where the
  // target precedes it; -128 where that is out of 8 bits' reach.
  int8_t between = -128;
};

struct SymbolAnchor {
  uint32_t offset;
  llvm::object::coff_symbol_generic *record;
  void apply(uint64_t newOffset) const { record->Value = newOffset; }
};

// The state of a section the link may resize.
struct RelaxAux {
  SmallVector<SymbolAnchor, 0> anchors;
  SmallVector<Edit, 0> edits;
  struct {
    SmallVector<Edit, 0> &edits;
    int32_t &operator[](size_t i) const { return edits[i].delta; }
  } relocDeltas{edits};
  uint32_t size;
  // The loops of the section that the link places within a line.
  StringRef loops;
  uint8_t places = 0;
  bool resized = false, fixed = false, fallsThrough = false;
  // The relocation of the end jump the link deletes, or -1.
  uint32_t tail = -1;
  // The jumps the chunks' order puts in reach, by relocation index, until
  // the section's edits are made.
  SmallVector<std::pair<uint32_t, int8_t>, 0> betweens;
};

struct ScannedSection {
  SmallVector<Relocation, 0> relocs;
  std::unique_ptr<RelaxAux> aux;
  // The next scanned part of the section's function's code, which the link
  // decides as one: the parts are a ring, and a part alone has none.
  SectionChunk *next = nullptr;
};

// A word of data that the loader binds as an import address table entry.
struct ImportSlot {
  SectionChunk *chunk;
  DefinedImportData *sym;
  uint32_t offset;
  bool startsRun;
};

// A site behind a delay-load guard, its relocation at index of chunk, and
// what it leaves in register reg: the value at value plus addend, or with
// address that address; or with reg -1 the instruction that the guard
// replays; with the bytes of its instruction before the field. An ARM64 site
// is the adrp of value's page.
struct DelayGuardSite {
  SectionChunk *chunk;
  Defined *value;
  uint32_t index;
  int32_t addend;
  bool address;
  int8_t reg = -1;
  StringRef insn = {};
  uint8_t before = 0;
};

// Whether s goes on with the run of prev, the word before it: both are of one
// DLL, and s is the next word of prev's section or, where across allows, the
// first of a section that follows prev's last word with no padding.
bool continuesRun(const ImportSlot *prev, const ImportSlot &s, bool across);

// A word the image's fill writes, and the import from whose address table
// entry it computes the word.
struct FillWord {
  Chunk *chunk;
  uint32_t offset;
  Defined *source;
  // What the object has in the word, which the fill adds.
  int64_t addend;
  // What the fill writes: the address that the entry holds, for a word that
  // names an address inside the import; the word at that address, for
  // __imp_X$val; or what the function there returns for the image's base,
  // for __imp_X$init. The fill's list states the kind in the low bits of the
  // entry's address, and the fill takes the greater kind first; its words of
  // the kind Import are the pointers of the imports that it binds by name.
  // A word of the kind Cell holds the address of an overridable default,
  // which the fill writes from the executable's override table, or of a
  // vague entity: its addend is a CellUse with the slot above it, and its
  // source is the word itself, an import's entry or a pointer of the image,
  // or for a word of data null, or the import of a vague entity. A word of
  // the kind TlsOffset is __imp_X$tpoff, which the fill computes once the
  // functions have run from the record that its source points at: the place
  // the record names plus the record's word at the addend.
  enum Kind : uint8_t { Address, Value, Init, Import, Cell, TlsOffset } kind;
};

// What a cell takes where the table names something for its slot: a call
// cell what a call of the default reaches, an address cell the default's
// address in every image, and the check cell of a default that the image
// defines what the default's check transfers to. Where the table names
// nothing, the image's own pointer takes the default's address, and any
// other cell keeps what the loader bound it to. A vague cell, which has no
// slot, takes the instance that the executable's offer table names for its
// entity, and otherwise keeps its value; a name word, the word at 8 of a
// type_info object that the image keeps preemptible, takes the name word of
// that instance where the two name one blob.
enum CellUse : uint8_t {
  CallCell,
  AddressCell,
  CheckCell,
  VagueCell,
  NameCell
};

// Whether w is a vague cell or a name word, which a DLL's fill binds to what
// the executable's offer table names or what a search finds.
inline bool isVagueCell(const FillWord &w) {
  return w.kind == FillWord::Cell && (w.addend & 31) >= VagueCell;
}

// Begins an error about w, a word the image's fill writes.
COFFSyncStream reportFillWord(COFFLinkerContext &ctx, const FillWord &w);

// The sites' facts that the scan and the relaxation both read, which
// Relocations.cpp states.
bool isX64(const SectionChunk &sc);
uint32_t read32(const SectionChunk &sc, uint32_t o);
bool isRead(uint16_t t);
// Whether s is the import of the runtime's key.
bool isKeyImport(Symbol *s);
bool isLoad(uint16_t t);
bool isMemJump(uint16_t t);
llvm::COFF::FusedUse getUse(const SectionChunk &sc, uint16_t type);
unsigned getBytesBefore(uint16_t type);
unsigned getBytesFrom(const SectionChunk &sc,
                      const llvm::object::coff_relocation &rel);
int64_t getFusedAddend(const llvm::object::coff_relocation &rel);
unsigned getLoadRegister(const SectionChunk &sc, uint32_t o, bool rex2);
// The relocation at offset o of sc as the scan keeps it, which it keeps from
// then on where it kept none of sc.
Relocation &getScannedAt(SectionChunk &sc, uint32_t o);
uint32_t getAlignPadding(uint32_t index, uint64_t pos, int shrink = 0);
COFFSyncStream errAt(const SectionChunk &sc, uint32_t offset);
bool isAdrpPair(uint32_t adrp, uint32_t low, aarch64::Ldr *ldr);
const llvm::object::coff_relocation *getTestJump(const SectionChunk &sc,
                                                 size_t i);
bool deletesEndJump(COFFLinkerContext &ctx, const SectionChunk &c,
                    const Relocation &r, uint32_t &begin, uint32_t &end,
                    llvm::function_ref<bool(uint32_t, uint32_t)> inside);

// The form of the image's base sites, which the compiler emits as the wide
// DLL's: that form; a direct image's place loaded and the thread pointer
// added; a link-time place added to it; or a wide executable's word at tp + H
// decoded with its key. A record chain of the image's own relaxes to it.
enum class TlsForm : uint8_t { Wide, Direct, Place, ExeWide };
TlsForm getTlsForm(const SectionChunk &sc);

// The instruction at relocation i of an owned-model record chain that reads
// its place word or a value it derives, as SiteFactIndex's word there states
// (spec section 15.1): x86-64 mov place, [p] and ARM64 ldp place, form, [p];
// the form's test; the thread pointer added to the place; the wide word's
// load; its rotate; its decode with the key; the select of the two; or a
// second marker of the select, which the first's rewrite writes.
enum class ChainSite : uint8_t {
  None,
  Place,
  Form,
  Base,
  Wide,
  Rotate,
  Decode,
  Select,
  Second
};
ChainSite getChainSite(const SectionChunk &sc, size_t i);
// The destination of the select at relocation i of sc and the register of
// the arm that the form keeps.
std::pair<unsigned, unsigned> getSelect(const SectionChunk &sc, size_t i,
                                        TlsForm form);

// Gives each relocation of the sections that need it its expression, decides
// the local import pointers the image keeps, binds the in-place slots and
// retargets branches past jumps.
void scanRelocations(COFFLinkerContext &ctx);

// Calls fn on each branch from one section to another that a fork object
// states, or with chainsOnly on each that chains. chains: a jump that ends a
// resizable section, laid out short beside its target; deletes: where the
// target follows, the jump goes.
void forEachBranch(
    COFFLinkerContext &ctx, bool chainsOnly,
    llvm::function_ref<void(const SectionChunk &from, uint32_t offset,
                            const SectionChunk &to, bool chains, bool deletes)>
        fn);

// Whether rel, of sc with the expression expr, is a branch Windows can
// rewrite to the body it selects for a function override: one to its symbol,
// with a field of 32 bits on x86-64 and a b or bl on ARM64, as its type states
// or as the scan made a call or jump through a pointer or register direct; in
// a foreign object's x86-64 code, a REL32 with no addend the walker finds a
// branch.
bool isFunctionOverrideSite(const SectionChunk &sc,
                            const llvm::object::coff_relocation &rel,
                            RelExpr expr);

// Whether rel, a reference of sc, takes its symbol's address where a load of
// the address fits (section 9.1).
bool takesAddress(const SectionChunk &sc,
                  const llvm::object::coff_relocation &rel);

// Whether rel, a reference of code, may take the address of an imported
// function: a site that takes it, or a read of its import's entry that no
// call or jump makes.
bool observesFunction(const SectionChunk &sc,
                      const llvm::object::coff_relocation &rel);

// Whether relocation i writes nothing in the image.
inline bool isDeleted(const ScannedSection *s, size_t i) {
  return s && (s->relocs[i].gone || s->relocs[i].expr == R_NONE ||
               s->relocs[i].expr == R_DEAD);
}

// The relaxation of resizable sections, run inside address assignment:
// initRelax decides every section's smallest size, relaxOnce lengthens what
// the layout left out of reach, or with refold folds again what it reaches,
// fixMisfits gives up the resizing of a section a value does not fit.
bool initRelax(COFFLinkerContext &ctx);
bool relaxOnce(COFFLinkerContext &ctx, bool refold = false);
bool fixMisfits(COFFLinkerContext &ctx);

// Writes a section the scan kept, resized or in place.
void writeScanned(const SectionChunk &sc, uint8_t *buf,
                  const ScannedSection &s);

// The output offset of an input offset of a resized section, or with end of
// the end of what precedes it.
uint32_t getOutputOffset(const SectionChunk &sc, const ScannedSection &s,
                         uint32_t offset, bool end = false);
uint32_t getInputOffset(const ScannedSection &s, DefinedRegular *d);

// Where relocation i of sc lies in the image, the type of its form there and
// the symbol it reaches.
uint32_t getRelaxedSite(const SectionChunk &sc, size_t i, uint16_t &type,
                        Symbol *&sym);
// The output offset in sc of the field Windows rewrites at relocation i, a
// function override's site.
uint32_t getFunctionOverrideField(const SectionChunk &sc, size_t i);

// The A - B fields of the span types and ARM64 page offsets.
bool spanFits(uint16_t type, int64_t v, ArrayRef<uint8_t> field, bool &aligned);
int64_t readSpan(uint16_t type, ArrayRef<uint8_t> field);
void writeSpan(uint16_t type, uint8_t *off, int64_t v, ArrayRef<uint8_t> field);
// Writes the fields sc's spans state over resized code.
void writeSpans(const SectionChunk &sc, uint8_t *buf);
// The addend in an ARM64 instruction's field at a relocation of type: an adr
// or adrp's byte offset, or a page offset.
int64_t getPageOffsetImm(uint32_t insn, uint16_t type);
} // namespace lld::coff

#endif
