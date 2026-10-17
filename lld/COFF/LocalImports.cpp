//===- LocalImports.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A reference to the import pointer of a symbol that the image defines reads
// a local import pointer, which the linker makes. Where the linker knows the
// instruction holding such a reference, it rewrites the instruction to reach
// the symbol directly, and leaves out a pointer that only rewritten
// instructions read.
//
//===----------------------------------------------------------------------===//

#include "LocalImports.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;

namespace lld::coff {
// Whether the bytes around a REL32 field at off are those of the form an
// object describes there. They verify the description, which is the only
// means of finding the instruction.
static bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off,
                       LinkSiteForm form) {
  // A displacement that names the pointer itself has no addend.
  if (off < 2 || off + 4 > data.size() || read32le(&data[off]) != 0)
    return false;
  uint8_t opcode = data[off - 2], modrm = data[off - 1];
  uint8_t prefix = off >= 3 ? data[off - 3] : 0;
  switch (form) {
  case LinkSiteCall:
    return opcode == 0xFF && modrm == 0x15;
  case LinkSiteJump:
    return opcode == 0xFF && modrm == 0x25;
  case LinkSiteJumpOnePrefix:
    // A REX prefix, or a legacy prefix.
    return off >= 3 && opcode == 0xFF && modrm == 0x25 &&
           ((prefix & 0xF0) == 0x40 ||
            is_contained(
                {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF2, 0xF3},
                prefix));
  case LinkSiteLoad:
  case LinkSiteLoadREX2:
  case LinkSiteAddress: {
    // mov r64, [rip+d] or lea r64, [rip+d], with REX.W, or with REX2 and its
    // W bit. A load's form says which.
    bool rex = (prefix & 0xF8) == 0x48;
    bool rex2 = off >= 4 && data[off - 4] == 0xD5 && (prefix & 0x88) == 0x08;
    return off >= 3 && opcode == (form == LinkSiteAddress ? 0x8D : 0x8B) &&
           (modrm & 0xC7) == 0x05 &&
           (form == LinkSiteLoad       ? rex
            : form == LinkSiteLoadREX2 ? rex2
                                       : rex || rex2);
  }
  default:
    return false;
  }
}

// The form that the object of sc describes for rel, a REL32 field.
static std::optional<LinkSiteForm> getSiteForm(const SectionChunk *sc,
                                               const coff_relocation &rel) {
  if (!sc->file->describesSites() || rel.Type != IMAGE_REL_AMD64_REL32)
    return std::nullopt;
  return sc->file->getLinkSiteForm(sc, rel.VirtualAddress);
}

// Whether a local import pointer holds zero, the default of an absent weak
// reference, which an instruction can materialise instead of loading.
static bool isZeroPointer(DefinedLocalImport *li) {
  auto *abs = dyn_cast<DefinedAbsolute>(li->getTarget());
  return abs && abs->getVA() == 0;
}

// Whether a reference to a local import pointer can reach the pointer's
// symbol directly instead. An absolute symbol is not at an address relative to
// the image, but zero can be materialised.
static bool canBypass(ObjFile *file, const coff_relocation &rel) {
  auto *li = dyn_cast_or_null<DefinedLocalImport>(
      file->getSymbol(rel.SymbolTableIndex));
  return li && (!isa<DefinedAbsolute>(li->getTarget()) || isZeroPointer(li));
}

// The form of the instruction at rel, a REL32 relocation against a local
// import pointer, if the object describes it as a call, jump or pointer load,
// which the linker rewrites to reach the pointer's symbol directly. A site
// whose bytes are not its form's sets *mismatch, if given, and is not
// rewritten.
static std::optional<LinkSiteForm>
getLocalImportRewrite(const SectionChunk *sc, const coff_relocation &rel,
                      bool *mismatch = nullptr) {
  ObjFile *file = sc->file;
  if (!canBypass(file, rel))
    return std::nullopt;
  std::optional<LinkSiteForm> form = getSiteForm(sc, rel);
  if (!form || *form == LinkSiteOther || *form == LinkSiteAddress)
    return std::nullopt;
  if (!isSiteForm(sc->getContents(), rel.VirtualAddress, *form)) {
    if (mismatch)
      *mismatch = true;
    return std::nullopt;
  }
  // Zero is materialised for a load or a call. A jump through it keeps
  // reading the pointer, since the unwinder takes an epilogue to end only in a
  // jump to an immediate or through memory.
  if (isZeroPointer(
          cast<DefinedLocalImport>(file->getSymbol(rel.SymbolTableIndex))) &&
      (*form == LinkSiteJump || *form == LinkSiteJumpOnePrefix))
    return std::nullopt;
  return form;
}

std::optional<LinkSiteForm> getDescribedSite(const SectionChunk *sc,
                                             const coff_relocation &rel) {
  std::optional<LinkSiteForm> form = getSiteForm(sc, rel);
  if (form && isSiteForm(sc->getContents(), rel.VirtualAddress, *form))
    return form;
  return std::nullopt;
}

namespace {
// The fields of the ARM64 instructions that compute an address from a page
// and an offset in it, decoded as lld/MachO decodes them for its linker
// optimization hints.
struct Adrp {
  uint32_t destRegister;
  int64_t addend;
};

struct Add {
  uint8_t destRegister;
  uint8_t srcRegister;
  uint32_t addend;
};

struct Ldr {
  uint8_t destRegister;
  uint8_t baseRegister;
  uint32_t offset;
};
} // namespace

static bool parseAdrp(uint32_t insn, Adrp &adrp) {
  if ((insn & 0x9f000000) != 0x90000000)
    return false;
  adrp.destRegister = insn & 0x1f;
  uint64_t immHi = (insn >> 5) & 0x7ffff;
  uint64_t immLo = (insn >> 29) & 0x3;
  adrp.addend = SignExtend64<21>(immLo | (immHi << 2)) * 4096;
  return true;
}

// add xd, xn, #imm, with a 64-bit destination and an unshifted immediate.
static bool parseAdd(uint32_t insn, Add &add) {
  if ((insn & 0xffc00000) != 0x91000000)
    return false;
  add.destRegister = insn & 0x1f;
  add.srcRegister = (insn >> 5) & 0x1f;
  add.addend = (insn >> 10) & 0xfff;
  return true;
}

// ldr xt, [xn, #imm], a 64-bit load with an unsigned offset.
static bool parseLdr(uint32_t insn, Ldr &ldr) {
  if ((insn & 0xffc00000) != 0xf9400000)
    return false;
  ldr.destRegister = insn & 0x1f;
  ldr.baseRegister = (insn >> 5) & 0x1f;
  ldr.offset = ((insn >> 10) & 0xfff) << 3;
  return true;
}

bool isArm64AddressPair(const SectionChunk *sc, const coff_relocation &adrp,
                        const coff_relocation &add) {
  ArrayRef<uint8_t> data = sc->getContents();
  Adrp first;
  Add second;
  return parseAdrp(read32le(&data[adrp.VirtualAddress]), first) &&
         first.addend == 0 &&
         parseAdd(read32le(&data[add.VirtualAddress]), second) &&
         second.addend == 0 && second.destRegister == first.destRegister &&
         second.srcRegister == first.destRegister;
}

bool isArm64PointerLoad(const SectionChunk *sc, const coff_relocation &rel) {
  ArrayRef<uint8_t> data = sc->getContents();
    return false;
  uint32_t insn = read32le(&data[rel.VirtualAddress]);
  switch (rel.Type) {
  case IMAGE_REL_ARM64_PAGEBASE_REL21: {
    Adrp adrp;
    return parseAdrp(insn, adrp) && adrp.addend == 0;
  }
  case IMAGE_REL_ARM64_PAGEOFFSET_12L: {
    Ldr ldr;
    return parseLdr(insn, ldr) && ldr.offset == 0;
  }
  default:
    return false;
  }
}

// Rewrites the described load whose REL32 field is at off, `mov r64,
// [rip+d]` with a REX or REX2 prefix, to materialise zero instead, in the same
// length: `mov r/m64, imm32` with the register moved from ModRM.reg to
// ModRM.rm, and so its high bits from R to B in the prefix, as lld/ELF's
// relaxGotNoPic moves them. REX is 0100WRXB, and REX2's payload is
// M R4 X4 B4 W R3 X3 B3; the X and B bits, which a RIP-relative operand
// ignores, are cleared.
static void rewriteZeroLoad(uint8_t *off, bool isRex2) {
  uint8_t rex = off[-3];
  if (isRex2)
    off[-3] = (rex & 0x08) | (rex & 0x44) >> 2;
  else
    off[-3] = (rex & 0xF8) | (rex & 0x4) >> 2;
  off[-2] = 0xC7;
  off[-1] = 0xC0 | (off[-1] & 0x38) >> 3;
  write32le(off, 0);
}

// Rewrites the described call whose REL32 field is at off, `call [rip+d]`
// through a pointer that holds zero, to call zero in the same length:
// `xor r11d, r11d; call r11`. The call pushes the same return address and
// faults at the same address. R11 carries no argument to a callee that does
// not exist, and is the linker's scratch register at a call.
static void rewriteZeroCall(uint8_t *off) {
  static const uint8_t call[] = {0x45, 0x31, 0xDB, 0x41, 0xFF, 0xD3};
  memcpy(off - 2, call, sizeof(call));
}

// Rewrites the described instruction whose REL32 field is at off, a reference
// through the import pointer of a symbol in the image at s, to reach the
// symbol directly, keeping the instruction's length and the address of the
// instruction after it. A load becomes `lea` and a call `addr32 call rel32`.
// A jump becomes `jmp rel32` at its first byte, followed by int3: the Windows
// unwinder recognises an epilogue only by the instruction that ends it, and
// accepts no prefix but BND before a direct jump.
bool rewriteLocalImportSite(uint8_t *off, LinkSiteForm form, uint64_t s,
                            uint64_t p) {
  switch (form) {
  case LinkSiteLoad:
  case LinkSiteLoadREX2:
    off[-2] = 0x8D;
    return true;
  case LinkSiteCall:
    off[-2] = 0x67;
    off[-1] = 0xE8;
    return true;
  case LinkSiteJump:
  case LinkSiteJumpOnePrefix: {
    uint8_t *start = off - (form == LinkSiteJump ? 2 : 3);
    uint64_t end = p - (off - start) + 5;
    start[0] = 0xE9;
    write32le(start + 1, s - end);
    memset(start + 5, 0xCC, off + 4 - (start + 5));
    return false;
  }
  default:
    llvm_unreachable("not a rewritten form");
  }
}

// A described call, jump or pointer load through the import pointer of a
// symbol in the image reaches the symbol directly. Its bytes were verified
// when local imports were bound.
// On ARM64, every adrp and ldr of a pointer that no other instruction reads
// becomes adrp and add of its symbol; data still reads the pointer.
bool relaxLocalImport(const SectionChunk *sc, const coff_relocation &rel,
                      uint8_t *off, DefinedLocalImport *li, Defined *&sym,
                      uint16_t &type, std::optional<LinkSiteForm> &rewrite) {
  if (sc->getArch() == Triple::aarch64) {
    if (li->getChunk()->bypassed && (type == IMAGE_REL_ARM64_PAGEBASE_REL21 ||
                                     type == IMAGE_REL_ARM64_PAGEOFFSET_12L)) {
      // Zero is materialised by movz into the register each writes.
      if (isZeroPointer(li)) {
        setArm64Opcode(off, 0xD2800000); // movz
        return true;
      }
      sym = li->getTarget();
      if (type == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
        setArm64Opcode(off, 0x91000000); // add
        type = IMAGE_REL_ARM64_PAGEOFFSET_12A;
      }
    }
  } else if ((rewrite = getLocalImportRewrite(sc, rel))) {
    if (isZeroPointer(li)) {
      if (*rewrite == LinkSiteCall)
        rewriteZeroCall(off);
      else
        rewriteZeroLoad(off, *rewrite == LinkSiteLoadREX2);
      return true;
    }
    sym = li->getTarget();
  }
  return false;
}

// Whether the references of file to local import pointers are reported only if
// they still read the pointer once local imports are bound, rather than each,
// as link.exe reports them. A compiler for the Windows Itanium and NT-POSIX
// targets reaches every symbol it cannot place in the image through its
// import pointer, which the linker rewrites, so for its objects the
// diagnostic is only about the references that keep the pointer. On x86-64
// those objects describe their sites, the only references rewritten there; on
// ARM64, where any object's loads of a pointer are rewritten, they carry
// link-only records.
bool defersLocalImportWarning(const ObjFile *file, bool arm64) {
  return file->describesSites() || (arm64 && file->hasLinkRecords());
}

// A .refptr.X section holds only a pointer to X, which the compiler reads
// where X may be outside the image or absent. When X is defined in the image,
// or is an absent weak reference, the pointer binds a local import pointer to
// X or to zero, whose rewritten references reach X or zero directly. That
// costs nothing and changes no value, but pays only where references can be
// rewritten: on ARM64, and on x86-64 in objects that describe their sites. The
// section stays the pointer whenever the image needs one, as bindLocalImports
// decides.
void bindPointerCells(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  if (symtab.pointerCells.empty() || symtab.isEC() || ctx.hybridSymtab)
    return;
  if (symtab.machine != ARM64 &&
      (symtab.machine != AMD64 ||
       llvm::none_of(ctx.objFileInstances,
                     [](ObjFile *f) { return f->describesSites(); })))
    return;
  llvm::TimeTraceScope timeScope("Bind pointer cells");
  for (Symbol *s : symtab.pointerCells) {
    // A symbol bound already, as one that several objects define is.
    auto *cell = dyn_cast<DefinedRegular>(s);
    if (!cell)
      continue;
    SectionChunk *sc = cell->getChunk();
    if (!sc || sc->getSize() != ctx.config.wordsize ||
        sc->getRelocs().size() != 1 || sc->getRelocs()[0].VirtualAddress != 0 ||
        !sc->children().empty())
      continue;
    StringRef name = cell->getName().substr(strlen(".refptr."));
    Symbol *x = sc->file->getSymbol(sc->getRelocs()[0].SymbolTableIndex);
    if (!x || x->getName() != name)
      continue;

    // An unresolved weak reference resolves to its default after mark-live.
    Defined *target = x->getDefined();
    auto *abs = dyn_cast_or_null<DefinedAbsolute>(target);
    bool absentWeak = isa<Undefined>(x) && abs && abs->getVA() == 0;
    if (!target || isa<DefinedImportThunk>(target) ||
        isa<DefinedImportData>(target) || (abs && !absentWeak))
      continue;
    replaceSymbol<DefinedLocalImport>(cell, ctx, cell->getName(), target);
    LocalImportChunk *c = cast<DefinedLocalImport>(cell)->getChunk();
    c->cell = sc;
    // Mark-live keeps the pointer when something live refers to it.
    c->live = !ctx.config.doGC;
    symtab.localImportChunks.push_back(c);
  }
}

// The pointer, of those bound to the compiler's pointers in cells, whose
// section s, a symbol of the file that holds them, is defined in.
static LocalImportChunk *findCell(ArrayRef<LocalImportChunk *> cells,
                                  Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedRegular>(s);
  if (!d)
    return nullptr;
  for (LocalImportChunk *c : cells)
    if (d->getChunk() == c->cell)
      return c;
  return nullptr;
}

// A reference to a local import pointer is rewritten to reach the pointer's
// symbol directly where the linker knows the instruction holding it, and a
// pointer that only rewritten references read is left out of the image.
// On x86-64 that is a described call, jump or pointer load, in an object
// that describes its instruction sites, decided per reference. On ARM64 it
// is every adrp and 64-bit ldr of a pointer, known from the relocation types,
// decided per symbol, since an adrp may serve several loads: one other
// reference keeps them all. Every other reference reads the pointer, as do
// a GC root and any reference from an x86-64 object that does not describe
// its sites. Under -import-slots, a reference from data reads the pointer but
// keeps no instruction from being rewritten, and is not reported: it is a
// pointer that the compiler made, such as a catch-type entry, never a
// dllimport declaration. Only the objects that refer to a local import are
// scanned, and only when the link has one; of those that do not describe
// their sites, only the ones that list a pointer bound to a compiler's.
void bindLocalImports(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  std::vector<Chunk *> &localImportChunks = symtab.localImportChunks;
  if (localImportChunks.empty())
    return;
  // Where no reference can be rewritten, every one reads its pointer, and the
  // undefined symbols' report has named each.
  bool arm64 = symtab.machine == ARM64 && !ctx.hybridSymtab;
  if (!arm64 && llvm::none_of(ctx.objFileInstances, [&](ObjFile *f) {
        return &f->symtab == &symtab && f->describesSites();
      }))
    return;
  llvm::TimeTraceScope timeScope("Bind local imports");
  SmallPtrSet<DefinedLocalImport *, 8> bypassed, read, readByData;
  for (Symbol *b : ctx.config.gcroot)
    if (auto *li = dyn_cast<DefinedLocalImport>(b))
      read.insert(li);

  // The references to report if they still read the pointer, once per file
  // and symbol, with whether they can be rewritten.
  struct Ref {
    ObjFile *file;
    DefinedLocalImport *li;
    bool rewritable;
  };
  std::vector<Ref> refs;

  // Without mark-live, the pointers bound to a compiler's pointers, by the
  // file that holds each compiler's pointer. A scan of that file finds whether
  // another name reaches the pointer's section: a reference to a local symbol
  // in it, since no other file can refer to one, or any external symbol
  // defined in it.
  DenseMap<ObjFile *, TinyPtrVector<LocalImportChunk *>> cellsByFile;
  if (!ctx.config.doGC)
    for (Chunk *c : localImportChunks)
      if (SectionChunk *cell = cast<LocalImportChunk>(c)->cell)
        cellsByFile[cell->file].push_back(cast<LocalImportChunk>(c));
  SmallPtrSet<LocalImportChunk *, 4> named;

  SmallVector<DefinedLocalImport *, 0> fileImports;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &symtab)
      continue;
    // An object that does not describe its sites reads every local import
    // pointer it refers to. Its references are scanned only if it lists the
    // symbol of a pointer bound to a compiler's pointer, which is left out
    // unless something live reads it.
    bool describes = file->describesSites() || arm64;
    if (describes) {
      if (llvm::none_of(file->getSymbols(),
                        IsaAndPresentPred<DefinedLocalImport>))
        continue;
    } else {
      fileImports.clear();
      bool listsCell = false;
      for (Symbol *s : file->getSymbols())
        if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s)) {
          fileImports.push_back(li);
          listsCell |= li->getChunk()->cell != nullptr;
        }
      if (!listsCell) {
        read.insert_range(fileImports);
        continue;
      }
    }

    ArrayRef<LocalImportChunk *> cells;
    if (auto it = cellsByFile.find(file); it != cellsByFile.end())
      cells = it->second;
    if (!cells.empty())
      for (Symbol *s : file->getSymbols())
        if (LocalImportChunk *c = findCell(cells, s);
            c && cast<DefinedRegular>(s)->getCOFFSymbol().isExternal())
          named.insert(c);

    MapVector<DefinedLocalImport *, bool> seen;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      bool fromData = ctx.config.importSlots &&
                      !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE);
      for (const coff_relocation &rel : sc->getRelocs()) {
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        if (!isa_and_nonnull<DefinedLocalImport>(s)) {
          if (LocalImportChunk *c = findCell(cells, s))
            named.insert(c);
          continue;
        }
        auto *li = cast<DefinedLocalImport>(s);
        if (fromData) {
          readByData.insert(li);
          continue;
        }
        if (!describes) {
          read.insert(li);
          continue;
        }
        bool mismatch = false;
        bool rewritable =
            arm64 ? canBypass(file, rel) && isArm64PointerLoad(sc, rel)
                  : getLocalImportRewrite(sc, rel, &mismatch).has_value();
        if (mismatch)
          Err(ctx) << file << ": the instruction at offset 0x"
                   << Twine::utohexstr(rel.VirtualAddress) << " in "
                   << sc->getSectionName()
                   << " is not the one its link-only record describes";
        if (rewritable)
          bypassed.insert(li);
        else
          read.insert(li);
        auto [it, inserted] = seen.try_emplace(li, rewritable);
        it->second &= rewritable;
      }
    }
    // A .refptr pointer is the compiler's, not a dllimport declaration, and
    // a pointer to an absent weak symbol's zero imports nothing. The
    // references of another object were reported with the undefined symbols.
    if (!defersLocalImportWarning(file, arm64))
      continue;
    for (auto [li, rewritable] : seen) {
      auto *abs = dyn_cast<DefinedAbsolute>(li->getTarget());
      if (!li->getName().starts_with(".refptr.") && !(abs && abs->getVA() == 0))
        refs.push_back({file, li, rewritable});
    }
  }

  // A pointer that a compiler made stays where it is, as the pointer, if
  // something live reads it other than through a rewritten instruction, or if
  // another name reaches its section, as mark-live or else the scan finds.
  for (auto &[file, cells] : cellsByFile)
    for (LocalImportChunk *c : cells)
      c->cell->live = named.contains(c);
  for (SmallPtrSet<DefinedLocalImport *, 8> *set : {&read, &readByData})
    for (DefinedLocalImport *li : *set)
      if (LocalImportChunk *c = li->getChunk(); c->cell && c->live)
        c->cell->live = true;

  for (DefinedLocalImport *li : bypassed) {
    if (read.contains(li))
      continue;
    li->getChunk()->bypassed = true;
    if (!readByData.contains(li))
      li->getChunk()->live = false;
  }
  llvm::erase_if(localImportChunks, [](Chunk *c) {
    auto *li = cast<LocalImportChunk>(c);
    if (li->cell)
      li->live = li->cell->live;
    return !li->live || li->cell;
  });
  // The pointers were made in the order of a walk of the symbol table, which
  // depends on its hash function; the image lays them out by their targets'
  // names instead.
  llvm::stable_sort(localImportChunks, [](Chunk *a, Chunk *b) {
    return cast<LocalImportChunk>(a)->getTarget()->getName() <
           cast<LocalImportChunk>(b)->getTarget()->getName();
  });

  if (!ctx.config.warnLocallyDefinedImported)
    return;
  for (const Ref &r : refs)
    if (!r.rewritable || (arm64 && !r.li->getChunk()->bypassed))
      Warn(ctx) << r.file << ": locally defined symbol imported: "
                << symtab.printSymbol(r.li->getTarget()) << " (defined in "
                << r.li->getTarget()->getFile() << ") [LNK4217]";
}
} // namespace lld::coff
