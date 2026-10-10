//===- KCFI.cpp -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "KCFI.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Driver.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "TypePrefix.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Transforms/Utils/KCFIHash.h"
#include <map>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {
// The routine that a KCFI type check reaches on a mismatch when the linker
// opens the type: it points R10 or X16 at the type's list and jumps to the
// compiled scanner, which fails fast on a target the list does not hold for a
// statically open type, and continues into the guard function for a
// dynamically open one. On x86-64 the latter ends in a trap, as the compiler
// emits it.
//
// With an outside routine, it first jumps there with a target outside the
// image, which it finds in RCX for a check routine on x86-64, in RAX for a
// dispatch routine and in X15 on ARM64. Without a list, it leaves R10 or X16
// as it finds it.
class KCFIOpenChunk : public NonSectionCodeChunk {
public:
  KCFIOpenChunk(COFFLinkerContext &ctx, Defined *list, Defined *scanner,
                bool dynamic, Defined *outside = nullptr, bool check = false);
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override;
  // Makes the routine jump to the static scanner s, which fails fast where
  // the dynamic one continues into the guard function.
  void setStatic(Defined *s) {
    scanner = s;
    dynamic = false;
  }

private:
  Defined *list;
  Defined *scanner;
  bool dynamic;
  Defined *outside;
  bool check;
  COFFLinkerContext &ctx;
};

// The linker's form of one of clang's KCFI thunks, which replaces it in an
// image the linker sealed. A target inside a code range the image vouches for,
// its own if a function of the thunk's type is in it and that of each DLL it
// imports whose code holds one, takes clang's type check and then the jump, or
// the return of a check thunk. Any other target takes clang's page test, the
// type check and the guard function. A DLL's range is read from its two bounds
// in the import address table, start then end.
class KCFIThunkChunk : public NonSectionCodeChunk {
public:
  struct Range {
    Defined *start;
    Defined *end;
  };
  KCFIThunkChunk(COFFLinkerContext &ctx, bool check, ArrayRef<uint8_t> compare,
                 ArrayRef<uint8_t> pageTest, Symbol *mismatch, Symbol *guard,
                 Defined *codeStart, Defined *codeEnd,
                 ArrayRef<Range> imported);
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override;

private:
  // The offsets of the parts of the thunk, in the order they are laid out
  // in when the image's own range is tested.
  struct Layout {
    uint32_t own, hit, imported, outside, mismatch, size;
  };
  Layout getLayout() const;

  bool check;
  // Clang's type check, without its branch, and its page test.
  SmallVector<uint8_t, 24> compare;
  SmallVector<uint8_t, 6> pageTest;
  Symbol *mismatch;
  Symbol *guard;
  // The bounds of the image's own range, if the thunk tests it.
  Defined *codeStart;
  Defined *codeEnd;
  SmallVector<Range, 0> imported;
  COFFLinkerContext &ctx;
};

// A word of a KCFI type's list, in the section that sorts it among the
// compiler's pieces of the list, or a cell that an entry points to. It holds
// a value, or the address of a symbol, which is zero if the symbol is not in
// the image.
class KCFIListChunk : public NonSectionChunk {
public:
  KCFIListChunk(COFFLinkerContext &ctx, StringRef sectionName, Defined *sym,
                uint64_t value = 0);
  size_t getSize() const override { return 8; }
  void getBaserels(std::vector<Baserel> *res) override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }
  StringRef getSectionName() const override { return sectionName; }

private:
  StringRef sectionName;
  Defined *sym;
  uint64_t value;
  COFFLinkerContext &ctx;
};

KCFIOpenChunk::KCFIOpenChunk(COFFLinkerContext &ctx, Defined *list,
                             Defined *scanner, bool dynamic, Defined *outside,
                             bool check)
    : list(list), scanner(scanner), dynamic(dynamic), outside(outside),
      check(check), ctx(ctx) {
  setAlignment(ctx.config.machine == ARM64 ? 4 : 1);
}

size_t KCFIOpenChunk::getSize() const {
  if (ctx.config.machine == ARM64)
    return (outside ? 36 : 0) + (list ? 8 : 0) + 12;
  return (outside ? 32 : 0) + (list ? 7 : 0) + (dynamic ? 6 : 5);
}

MachineTypes KCFIOpenChunk::getMachine() const { return ctx.config.machine; }

void KCFIOpenChunk::writeTo(uint8_t *buf) const {
  // The scanner reads the list from its first word after the head.
  uint64_t first = list ? list->getRVA() + 8 : 0;
  // The image ends where its last section does, rounded up to the page that
  // the loader maps it in.
  uint64_t end = 0;
  if (outside) {
    for (OutputSection *sec : ctx.outputSections)
      end = std::max(end, sec->getRVA() + sec->getVirtualSize());
    end = alignTo(end, 4096);
  }
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    // Each address is reached at any distance, since the linker adds no range
    // extension thunk for a chunk of its own.
    auto setAddr = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);                // adrp
      write32le(buf + off + 4, 0x91000000 | reg << 5 | reg); // add
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
      off += 8;
    };
    auto branch = [&](uint64_t target) {
      setAddr(17, target);
      write32le(buf + off, 0xD61F0220); // br x17
      off += 4;
    };
    if (outside) {
      // adrp x17, start; cmp x15, x17; b.lo 1f
      // adrp x17, end; cmp x15, x17; b.hs 1f
      uint32_t tail = getSize() - 12;
      write32le(buf, 0x90000011);
      write32le(buf + 4, 0xEB1101FF);
      write32le(buf + 8, 0x54000003 | (tail - 8) / 4 << 5);
      write32le(buf + 12, 0x90000011);
      write32le(buf + 16, 0xEB1101FF);
      write32le(buf + 20, 0x54000002 | (tail - 20) / 4 << 5);
      applyArm64Addr(buf, 0, rva, 12);
      applyArm64Addr(buf + 12, end, rva + 12, 12);
      off = 24;
    }
    // adrp x16, first; add x16, x16, :lo12:first
    if (list)
      setAddr(16, first);
    // adrp x17, scanner; add x17, x17, :lo12:scanner; br x17
    branch(scanner->getRVA());
    // 1: adrp x17, outside; add x17, x17, :lo12:outside; br x17
    if (outside)
      branch(outside->getRVA());
    return;
  }
  if (outside) {
    static const uint8_t bound[] = {
        0x4C, 0x8D, 0x1D, 0, 0, 0, 0, // lea r11, [rip + start]
        0x4C, 0x39, 0xD8,             // cmp rax, r11
        0x0F, 0x82, 0,    0, 0, 0,    // jb outside
        0x4C, 0x8D, 0x1D, 0, 0, 0, 0, // lea r11, [rip + end]
        0x4C, 0x39, 0xD8,             // cmp rax, r11
        0x0F, 0x83, 0,    0, 0, 0,    // jae outside
    };
    memcpy(buf, bound, sizeof(bound));
    if (check)
      buf[9] = buf[25] = 0xD9; // cmp rcx, r11
    write32le(buf + 3, -(rva + 7));
    write32le(buf + 12, outside->getRVA() - (rva + 16));
    write32le(buf + 19, end - (rva + 23));
    write32le(buf + 28, outside->getRVA() - (rva + 32));
    off = sizeof(bound);
  }
  // lea r10, [rip + first]; jmp scanner; int3
  if (list) {
    static const uint8_t lea[] = {0x4C, 0x8D, 0x15, 0, 0, 0, 0};
    memcpy(buf + off, lea, sizeof(lea));
    write32le(buf + off + 3, first - (rva + off + 7));
    off += 7;
  }
  buf[off] = 0xE9;
  write32le(buf + off + 1, scanner->getRVA() - (rva + off + 5));
  if (dynamic)
    buf[off + 5] = 0xCC;
}

KCFIThunkChunk::KCFIThunkChunk(COFFLinkerContext &ctx, bool check,
                               ArrayRef<uint8_t> compare,
                               ArrayRef<uint8_t> pageTest, Symbol *mismatch,
                               Symbol *guard, Defined *codeStart,
                               Defined *codeEnd, ArrayRef<Range> imported)
    : check(check), compare(compare), pageTest(pageTest), mismatch(mismatch),
      guard(guard), codeStart(codeStart), codeEnd(codeEnd), imported(imported),
      ctx(ctx) {
  setAlignment(16);
}

MachineTypes KCFIThunkChunk::getMachine() const { return ctx.config.machine; }

// With the image's own range tested, the thunk is laid out as: the range test,
// the type check and jump that a target inside a range takes, the tests of the
// imported ranges, and the path of a target outside every range. Without it,
// the type check and jump follow that path instead, so that no branch skips
// them. On ARM64 the branch to the mismatch routine, which may be far, ends
// the thunk.
KCFIThunkChunk::Layout KCFIThunkChunk::getLayout() const {
  bool isARM64 = ctx.config.machine == ARM64;
  bool own = codeStart;
  bool hits = own || !imported.empty();
  uint32_t ownSize = own ? (isARM64 ? 28 : 22) : 0;
  uint32_t hitSize = 0;
  if (hits)
    hitSize = compare.size() + (isARM64 ? 8 : 6 + (check ? 1 : 2));
  uint32_t importedSize = imported.size() * (isARM64 ? 28 : 18);
  uint32_t outsideSize =
      pageTest.size() + compare.size() + (isARM64 ? 4 + 4 + 12 : 6 + 6 + 6);
  Layout l;
  l.own = 0;
  if (own) {
    l.hit = ownSize;
    l.imported = l.hit + hitSize;
    l.outside = l.imported + importedSize;
    l.mismatch = l.outside + outsideSize;
  } else {
    l.imported = 0;
    l.outside = importedSize;
    l.hit = l.outside + outsideSize;
    l.mismatch = l.hit + hitSize;
  }
  l.size = l.mismatch + (isARM64 ? 12 : 0);
  return l;
}

size_t KCFIThunkChunk::getSize() const { return getLayout().size; }

void KCFIThunkChunk::writeTo(uint8_t *buf) const {
  Layout l = getLayout();
  bool hits = codeStart || !imported.empty();
  uint64_t mismatchRVA = cast<Defined>(mismatch)->getRVA();
  uint64_t guardRVA = cast<Defined>(guard)->getRVA();
  uint64_t codeSize = codeStart ? codeEnd->getRVA() - codeStart->getRVA() : 0;

  if (ctx.config.machine == ARM64) {
    // The target is in X15; X16 and X17 are free, as for the guard function.
    auto insn = [&](uint32_t off, uint32_t v) { write32le(buf + off, v); };
    auto branch19 = [&](uint32_t off, uint32_t cond, uint32_t to) {
      int32_t delta = (int32_t(to) - int32_t(off)) / 4;
      insn(off, 0x54000000 | cond | (delta & 0x7FFFF) << 5);
    };
    auto adrpAdd = [&](uint32_t off, uint32_t reg, uint64_t target) {
      insn(off, 0x90000000 | reg);                // adrp reg, target
      insn(off + 4, 0x91000000 | reg << 5 | reg); // add reg, reg, :lo12:target
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
    };
    if (codeStart) {
      // adrp x16, start; add x16, x16, :lo12:start; sub x16, x15, x16
      // mov x17, #size; movk x17, #size, lsl #16; cmp x16, x17; b.hs outside
      adrpAdd(l.own, 16, codeStart->getRVA());
      insn(l.own + 8, 0xCB1001F0);
      insn(l.own + 12, 0xD2800011 | (codeSize & 0xFFFF) << 5);
      insn(l.own + 16, 0xF2A00011 | (codeSize >> 16) << 5);
      insn(l.own + 20, 0xEB11021F);
      branch19(l.own + 24, 2, l.imported);
    }
    if (hits) {
      // The type check; b.ne mismatch; ret
      memcpy(buf + l.hit, compare.data(), compare.size());
      uint32_t off = l.hit + compare.size();
      branch19(off, 1, l.mismatch);
      insn(off + 4, 0xD65F03C0);
    }
    for (size_t i = 0; i != imported.size(); ++i) {
      // adrp x16, start; add x16, x16, :lo12:start; ldp x16, x17, [x16]
      // cmp x15, x16; b.lo 1f; cmp x15, x17; b.lo hit; 1:
      uint32_t off = l.imported + i * 28;
      adrpAdd(off, 16, imported[i].start->getRVA());
      insn(off + 8, 0xA9404610);
      insn(off + 12, 0xEB1001FF);
      branch19(off + 16, 3, off + 28);
      insn(off + 20, 0xEB1101FF);
      branch19(off + 24, 3, l.hit);
    }
    // The page test; b.eq mismatch; the type check; b.ne mismatch
    // adrp x16, guard; ldr x16, [x16, :lo12:guard]; br x16
    uint32_t off = l.outside;
    memcpy(buf + off, pageTest.data(), pageTest.size());
    off += pageTest.size();
    branch19(off, 0, l.mismatch);
    off += 4;
    memcpy(buf + off, compare.data(), compare.size());
    off += compare.size();
    branch19(off, 1, l.mismatch);
    insn(off + 4, 0x90000010);
    insn(off + 8, 0xF9400210);
    insn(off + 12, 0xD61F0200);
    applyArm64Addr(buf + off + 4, guardRVA, rva + off + 4, 12);
    applyArm64Ldr(buf + off + 8, guardRVA & 0xfff);
    // adrp x17, mismatch; add x17, x17, :lo12:mismatch; br x17
    adrpAdd(l.mismatch, 17, mismatchRVA);
    insn(l.mismatch + 8, 0xD61F0220);
    return;
  }

  // The target is in RAX for a dispatch thunk and in RCX for a check thunk;
  // R10 and R11 are free, as for the guard function.
  uint8_t modrm = check ? 0x0D : 0x05;
  auto rel32 = [&](uint32_t off, uint64_t target) {
    write32le(buf + off, target - (rva + off + 4));
  };
  auto rel8 = [&](uint32_t off, uint32_t target) {
    buf[off] = target - (off + 1);
  };
  if (codeStart) {
    static const uint8_t ownTest[] = {
        0x4C, 0x8D, 0x15, 0, 0, 0, 0, // lea r10, [rip + start]
        0x49, 0x89, 0xC3,             // mov r11, rax
        0x4D, 0x29, 0xD3,             // sub r11, r10
        0x49, 0x81, 0xFB, 0, 0, 0, 0, // cmp r11, size
        0x73, 0,                      // jae imported
    };
    memcpy(buf + l.own, ownTest, sizeof(ownTest));
    rel32(l.own + 3, codeStart->getRVA());
    if (check)
      buf[l.own + 9] = 0xCB; // mov r11, rcx
    write32le(buf + l.own + 16, codeSize);
    rel8(l.own + 21, l.imported);
  }
  if (hits) {
    // The type check; jne mismatch; jmp rax or ret
    uint32_t off = l.hit;
    memcpy(buf + off, compare.data(), compare.size());
    off += compare.size();
    buf[off] = 0x0F;
    buf[off + 1] = 0x85;
    rel32(off + 2, mismatchRVA);
    if (check) {
      buf[off + 6] = 0xC3;
    } else {
      buf[off + 6] = 0xFF;
      buf[off + 7] = 0xE0;
    }
  }
  for (size_t i = 0; i != imported.size(); ++i) {
    // cmp rax, [rip + start]; jb 1f; cmp rax, [rip + end]; jb hit; 1:
    uint32_t off = l.imported + i * 18;
    const uint8_t test[] = {0x48, 0x3B, modrm, 0, 0, 0, 0, 0x72, 0x09,
                            0x48, 0x3B, modrm, 0, 0, 0, 0, 0x72, 0};
    memcpy(buf + off, test, sizeof(test));
    rel32(off + 3, imported[i].start->getRVA());
    rel32(off + 12, imported[i].end->getRVA());
    rel8(off + 17, l.hit);
  }
  // The page test; je mismatch; the type check; jne mismatch; jmp [rip + guard]
  uint32_t off = l.outside;
  memcpy(buf + off, pageTest.data(), pageTest.size());
  off += pageTest.size();
  buf[off] = 0x0F;
  buf[off + 1] = 0x84;
  rel32(off + 2, mismatchRVA);
  off += 6;
  memcpy(buf + off, compare.data(), compare.size());
  off += compare.size();
  buf[off] = 0x0F;
  buf[off + 1] = 0x85;
  rel32(off + 2, mismatchRVA);
  buf[off + 6] = 0xFF;
  buf[off + 7] = 0x25;
  rel32(off + 8, guardRVA);
}

KCFIListChunk::KCFIListChunk(COFFLinkerContext &ctx, StringRef sectionName,
                             Defined *sym, uint64_t value)
    : sectionName(sectionName), sym(sym), value(value), ctx(ctx) {
  setAlignment(8);
}

void KCFIListChunk::getBaserels(std::vector<Baserel> *res) {
  if (sym && sym->isLive())
    res->emplace_back(getRVA(), ctx.config.machine);
}

void KCFIListChunk::writeTo(uint8_t *buf) const {
  uint64_t v = value;
  if (sym)
    v = sym->isLive() ? sym->getRVA() + ctx.config.imageBase : 0;
  write64le(buf, v);
}

// The kinds of mismatch routine, one for each kind of thunk, with the target in
// the thunk's register, and the scanners they pass a type's list to: a static
// scanner fails fast where a dynamic one continues into the guard function.
// ARM64 has only check thunks.
struct RoutineKind {
  StringRef mismatch, staticScanner, dynamicScanner;
  bool check;
};

static ArrayRef<RoutineKind> getRoutineKinds(COFFLinkerContext &ctx) {
  static const RoutineKind kinds[] = {
      {KCFIMismatchPrefix, KCFIOpenScanner, KCFIOpenDynamicScanner, false},
      {KCFICheckMismatchPrefix, KCFICheckOpenScanner,
       KCFICheckOpenDynamicScanner, true}};
  return ArrayRef(kinds).drop_front(ctx.config.machine == AMD64 ? 0 : 1);
}

// Whether s, which an object references, reaches code without a prefix of
// ours, which the guard function table lists for an object without guard
// metadata. A reference through __imp_X, which may become a pointer the linker
// makes, reaches X.
static bool isUnprefixedCode(SymbolTable &symtab, Symbol *s,
                             function_ref<bool(Symbol *)> isPrefixedEntry) {
  if (auto *u = dyn_cast_or_null<Undefined>(s);
      u && u->getName().starts_with("__imp_"))
    s = symtab.find(u->getName().drop_front(strlen("__imp_")));
  if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s))
    s = li->getTarget();
  if (isa_and_nonnull<DefinedImportThunk>(s))
    return true;
  auto *r = dyn_cast_or_null<DefinedRegular>(s);
  return r && r->getChunk() &&
         (r->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE) &&
         !isPrefixedEntry(r);
}

// Whether the guard function table may list, for file, a target without a
// prefix of ours: anything but such an entry that its guard metadata lists, a
// function its address-taken import table reaches, or, where it has no guard
// metadata, or describes the sites that take addresses, a function any of its
// relocations reaches, as the writer lists every function such references
// reach. Liveness, which the writer also asks, is not yet known.
static bool mayListUnprefixed(SymbolTable &symtab, ObjFile *file,
                              function_ref<bool(Symbol *)> isPrefixedEntry) {
  auto readIndices = [&](SectionChunk *c, auto f) {
    ArrayRef<uint8_t> data = c->getContents();
    for (size_t i = 0; i + 4 <= data.size(); i += 4)
      if (uint32_t index = read32le(data.data() + i);
          index < file->getSymbols().size() && f(file->getSymbols()[index]))
        return true;
    return false;
  };
  if (file->hasGuardCF()) {
    for (SectionChunk *c : file->getGuardFidChunks())
      if (readIndices(c, [&](Symbol *s) {
            return isa_and_nonnull<Defined>(s) && !isPrefixedEntry(s);
          }))
        return true;
    for (SectionChunk *c : file->getGuardIATChunks())
      if (readIndices(c, [&](Symbol *s) {
            return !isa_and_nonnull<DefinedImportData>(s) &&
                   isUnprefixedCode(symtab, s, isPrefixedEntry);
          }))
        return true;
    if (!file->describesSites())
      return false;
  }
  for (Chunk *c : file->getChunks())
    if (auto *sc = dyn_cast_or_null<SectionChunk>(c))
      for (const coff_relocation &rel : sc->getRelocs())
        if (isUnprefixedCode(symtab, file->getSymbol(rel.SymbolTableIndex),
                             isPrefixedEntry))
          return true;
  return false;
}

// Opens a KCFI type where the compiler could not see that code without a KCFI
// prefix of ours reaches it, because the link brings that code in. Clang
// gives the facts as weak externals that name the type, in 8 lowercase hex
// digits, and a function:
//
// - __kcfi_typeid_<f> = <type>, for a declaration f whose address an object
//   takes. If f resolves to an import or to a definition without a prefix,
//   the type is open statically and f is added to its list of targets.
// - __kcfi_inflow_<type>_<g>, for a declaration g through which a pointer of
//   the type can come back. If g resolves so, or g is a variable that a DLL
//   provides or that an object with code and no prefix defines, the type is
//   open dynamically.
// - __kcfi_param_<type>_<g>, for a definition g that can receive a pointer of
//   the type. If an object with code and no prefix references g, directly or
//   through __imp_g, the type is open dynamically.
//
// Foreign code in the image that references no import can hand ours only
// functions in the image, where the writer may bound the mismatch. So a type
// that only parameter facts and inflow facts whose g is defined in the image
// open dynamically, directly or through tinflow facts, is remembered, for the
// writer to open statically instead.
//
// An inflow or parameter fact may name a node, n<node> in 16 lowercase hex
// digits, in place of a type, as __kcfi_inflow_n<node>_<g>, so that an object
// names the types a record holds once rather than once per function that
// reaches the record. It stands for the facts of each type the node holds,
// which __kcfi_node_<node>_<type> gives, and names nothing when the node holds
// no type.
//
// - __kcfi_tinflow_<type>_<called>, or __kcfi_tinflow_n<node>_<called>, for a
//   type that an object calls: a pointer of the type can come back from a
//   call through a pointer of the called type. If the called type is open
//   dynamically, because a compiled mismatch routine of it jumps to a dynamic
//   scanner or because a fact here opens it, the type is open dynamically,
//   and so on until nothing more opens.
//
// A type is opened only where a KCFI thunk refers to its mismatch routine,
// __llvm_kcfi_mismatch_<type> or __llvm_kcfi_check_mismatch_<type>. While the
// routine is the weak default that fails fast, it becomes one that points at
// the type's list and jumps to the compiled scanner of the type's kind, and a
// dynamic opening replaces a static routine too. The lists' words go in the
// sections that sort them among the compiler's. This runs after LTO, whose
// objects carry the facts, and before the garbage collector, so that what the
// routines refer to is kept.
void openKCFITypes(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  bool isX64 = ctx.config.machine == AMD64;
  if (!isX64 && ctx.config.machine != ARM64)
    return;
  // Every object with a KCFI thunk defines the scanners of the thunk's kind.
  if (!symtab.find(KCFIOpenDynamicScanner) &&
      !symtab.find(KCFICheckOpenDynamicScanner))
    return;
  llvm::TimeTraceScope timeScope("Open KCFI types");

  // Whether each object is foreign, and the functions with a KCFI prefix with
  // a marker: each the first function to follow its static __cfi_ label in
  // its chunk.
  DenseSet<std::pair<SectionChunk *, uint32_t>> prefixed;
  DenseMap<ObjFile *, bool> objInfo;
  auto scan = [&](ObjFile *file) {
    auto [it, inserted] = objInfo.try_emplace(file);
    if (!inserted)
      return it->second;
    for (const TypePrefixLabel &l : getTypePrefixes(file))
      if (l.entry)
        prefixed.insert({l.label->getChunk(), l.entry});
    return it->second = !hasTypePrefixes(file);
  };
  // A reference to a variable that a DLL provides stays undefined until
  // automatic import resolves it to the variable's __imp_ pointer.
  auto resolve = [&](Symbol *s) -> Defined * {
    if (!s)
      return nullptr;
    if (Defined *d = s->getDefined())
      return d;
    if (s->getName().starts_with("__imp_"))
      return nullptr;
    return dyn_cast_or_null<DefinedImportData>(
        symtab.find(("__imp_" + s->getName()).str()));
  };
  // A function is foreign where it has no prefix; a variable, which never
  // has one, where the object defining it is foreign.
  auto isForeign = [&](Symbol *s) {
    Defined *d = resolve(s);
    if (isa_and_nonnull<DefinedImportThunk, DefinedImportData>(d))
      return true;
    auto *c = dyn_cast_or_null<DefinedCOFF>(d);
    auto *file = c ? dyn_cast_or_null<ObjFile>(c->getFile()) : nullptr;
    if (!file)
      return false;
    bool foreign = scan(file);
    auto *r = dyn_cast<DefinedRegular>(c);
    if (r && r->getChunk() &&
        (r->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
      return !prefixed.contains({r->getChunk(), r->getValue()});
    return foreign;
  };

  // For each type, whether it is open dynamically, and whether only through
  // foreign code in the image, and the symbols to add to its list, each with
  // whether the entry points at a cell holding it rather than at the symbol
  // itself.
  struct Opening {
    bool dynamic = false;
    bool local = false;
    SmallVector<std::pair<Defined *, bool>, 0> entries;
  };
  std::map<uint32_t, Opening> openings;
  std::vector<Symbol *> typeids;
  // A fact names its precise key, 16 hex digits, and its value is the check
  // identifier, which a COFF absolute symbol holds.
  auto factCheck = [&](Symbol *s) -> uint32_t {
    if (auto *d = dyn_cast_or_null<DefinedAbsolute>(s->getDefined()))
      return d->getVA();
    return 0;
  };
  // The types that the inflow and parameter facts name, by the symbol each
  // names, the nodes that they name, and the types of each node, each as
  // (check, precise key). The precise types each object opens, keyed, with the
  // check identifier. The types that a call through a precise key can hand
  // back.
  DenseMap<Symbol *, SmallVector<std::pair<uint32_t, uint64_t>, 1>> inflows,
      params;
  SmallVector<std::pair<Symbol *, uint64_t>, 0> inflowNodes, paramNodes;
  DenseMap<uint64_t, SmallVector<std::pair<uint32_t, uint64_t>, 1>> tinflows;
  SmallVector<std::pair<uint64_t, uint64_t>, 0> tinflowNodes;
  DenseMap<uint64_t, SmallVector<std::pair<uint32_t, uint64_t>, 2>> nodes;
  SmallVector<std::pair<uint64_t, uint32_t>, 0> popens;
  // A fact names its precise key, 16 hex digits, then the symbol it concerns.
  auto parseFact = [&](StringRef rest, uint64_t &key) -> Symbol * {
    if (rest.size() < 18 || rest[16] != '_' ||
        rest.take_front(16).getAsInteger(16, key))
      return nullptr;
    return symtab.find(rest.drop_front(17));
  };
  auto parseNodeFact = [&](StringRef rest, uint64_t &node) -> Symbol * {
    if (rest.size() < 19 || rest[17] != '_' ||
        rest.substr(1, 16).getAsInteger(16, node))
      return nullptr;
    return symtab.find(rest.drop_front(18));
  };
  auto addParam = [&](Symbol *g, uint32_t check, uint64_t key) {
    params[g].push_back({check, key});
    // Code that declares g dllimport reaches our g through __imp_g, which
    // becomes a local import unless it is bound to something else.
    auto *imp = dyn_cast_or_null<Undefined>(
        symtab.find(("__imp_" + g->getName()).str()));
    if (imp && !imp->getWeakAlias())
      params[imp].push_back({check, key});
  };
  symtab.forEachSymbol([&](Symbol *s) {
    // A lazy fact is an archive member's that is not in the link. Its value
    // reads as zero, but its precise key would still reach types through the
    // facts of the objects that are.
    if (s->isLazy())
      return;
    StringRef name = s->getName();
    uint64_t key = 0;
    uint64_t node = 0;
    if (!name.consume_front("__kcfi_"))
      return;
    if (name.starts_with("typeid_")) {
      typeids.push_back(s);
    } else if (name.consume_front("inflow_")) {
      if (name.starts_with("n")) {
        if (Symbol *g = parseNodeFact(name, node))
          inflowNodes.push_back({g, node});
      } else if (Symbol *g = parseFact(name, key)) {
        inflows[g].push_back({factCheck(s), key});
      }
    } else if (name.consume_front("param_")) {
      if (name.starts_with("n")) {
        if (Symbol *g = parseNodeFact(name, node))
          paramNodes.push_back({g, node});
      } else if (Symbol *g = parseFact(name, key)) {
        addParam(g, factCheck(s), key);
      }
    } else if (name.consume_front("tinflow_")) {
      // The fact ends in the called type's precise key, 16 hex digits, rather
      // than in a symbol.
      uint64_t called = 0;
      if (name.size() < 18 || name[name.size() - 17] != '_' ||
          name.take_back(16).getAsInteger(16, called))
        return;
      name = name.drop_back(17);
      if (name.size() == 17 && name[0] == 'n' &&
          !name.drop_front().getAsInteger(16, node))
        tinflowNodes.push_back({called, node});
      else if (name.size() == 16 && !name.getAsInteger(16, key))
        tinflows[called].push_back({factCheck(s), key});
    } else if (name.consume_front("node_")) {
      if (name.size() == 33 && name[16] == '_' &&
          !name.take_front(16).getAsInteger(16, node) &&
          !name.drop_front(17).getAsInteger(16, key))
        nodes[node].push_back({factCheck(s), key});
    } else if (name.consume_front("popen_")) {
      if (name.size() == 16 && !name.getAsInteger(16, key))
        popens.push_back({key, factCheck(s)});
    }
  });
  for (auto [g, node] : inflowNodes)
    if (auto it = nodes.find(node); it != nodes.end())
      llvm::append_range(inflows[g], it->second);
  for (auto [g, node] : paramNodes)
    if (auto it = nodes.find(node); it != nodes.end())
      for (auto [check, key] : it->second)
        addParam(g, check, key);
  for (auto [called, node] : tinflowNodes)
    if (auto it = nodes.find(node); it != nodes.end())
      llvm::append_range(tinflows[called], it->second);

  // A type is open dynamically where a precise key reaches it; its routine is
  // keyed by the check identifier. The key is remembered, with whether only
  // local facts reached it, so that a call through it follows its tinflow
  // facts, again when a non-local fact reaches it later; a type is local,
  // narrowable by the writer, only where every precise key that reached it is.
  DenseMap<uint64_t, bool> reached;
  SmallVector<uint64_t, 0> worklist;
  auto reach = [&](uint32_t check, uint64_t key, bool local) {
    Opening &o = openings[check];
    o.local = (!o.dynamic || o.local) && local;
    o.dynamic = true;
    auto [it, inserted] = reached.try_emplace(key, local);
    if (inserted || (it->second && !local)) {
      it->second = local;
      worklist.push_back(key);
    }
  };
  auto openDynamically = [&](ArrayRef<std::pair<uint32_t, uint64_t>> types,
                             bool local) {
    for (auto [check, key] : types)
      reach(check, key, local);
  };
  for (auto &[g, types] : inflows)
    if (isForeign(g))
      openDynamically(
          types,
          !isa_and_nonnull<DefinedImportThunk, DefinedImportData>(resolve(g)));

  // An import is listed by its import address table entry, and the writer adds
  // a cell holding its thunk where static data holds the thunk; a definition
  // without a prefix is listed by a cell holding its address.
  llvm::sort(typeids,
             [](Symbol *a, Symbol *b) { return a->getName() < b->getName(); });
  for (Symbol *s : typeids) {
    auto *id = dyn_cast_or_null<DefinedAbsolute>(s->getDefined());
    Symbol *f = symtab.find(s->getName().substr(strlen("__kcfi_typeid_")));
    if (!id || !isForeign(f))
      continue;
    auto &entries = openings[uint32_t(id->getVA())].entries;
    Defined *d = resolve(f);
    if (auto *thunk = dyn_cast<DefinedImportThunk>(d))
      entries.push_back({thunk->wrappedSym, false});
    else
      entries.push_back({d, !isa<DefinedImportData>(d)});
  }

  // An object with code and no prefix is foreign.
  if (!params.empty()) {
    for (ObjFile *file : ctx.objFileInstances) {
      for (Symbol *s : file->getSymbols()) {
        auto it = params.find(s);
        if (it == params.end())
          continue;
        auto *r = dyn_cast_or_null<DefinedRegular>(s->getDefined());
        if (r && r->file == file)
          continue;
        if (!scan(file))
          break;
        openDynamically(it->second, /*local=*/true);
      }
    }
  }

  ArrayRef<RoutineKind> machineKinds = getRoutineKinds(ctx);
  // The scanner that m jumps to, if m is a compiled mismatch routine of the
  // kind.
  auto compiledScanner = [&](Symbol *m, const RoutineKind &k) -> Symbol * {
    auto *r = dyn_cast_or_null<DefinedRegular>(m);
    if (!r)
      return nullptr;
    auto refs = r->getChunk()->symbols();
    for (StringRef name : {k.staticScanner, k.dynamicScanner})
      if (Symbol *scanner = symtab.find(name);
          scanner && is_contained(refs, scanner))
        return scanner;
    return nullptr;
  };

  // Each object publishes the precise types it opens dynamically; a call
  // through one of them can hand back a foreign function too, and so on until
  // nothing more opens. An object opening is never local: it is a cast or an
  // import, which may carry a run-time address from any image.
  for (auto [key, check] : popens)
    reach(check, key, /*local=*/false);
  while (!worklist.empty()) {
    uint64_t key = worklist.pop_back_val();
    bool local = reached[key];
    if (auto it = tinflows.find(key); it != tinflows.end())
      for (auto [c, k] : it->second)
        reach(c, k, local);
  }

  auto keep = [&](Defined *d) {
    if (!d->isGCRoot) {
      d->isGCRoot = true;
      ctx.config.gcroot.push_back(d);
    }
  };
  for (auto &kv : openings) {
    uint32_t type = kv.first;
    Opening &opening = kv.second;
    std::string hex = utohexstr(type, /*LowerCase=*/true, /*Width=*/8);
    Defined *head =
        dyn_cast_or_null<Defined>(symtab.find(KCFIListPrefix + hex));
    auto addHead = [&] {
      auto *c = make<KCFIListChunk>(
          ctx, saver().save(KCFIListSectionPrefix + hex + "_a"), nullptr, type);
      ctx.kcfi.chunks.push_back(c);
      ctx.kcfi.chunks.push_back(make<KCFIListChunk>(
          ctx, saver().save(KCFIListSectionPrefix + hex + "_z"), nullptr,
          uint64_t(type) << 1 | 1));
      head = cast<Defined>(
          symtab.addSynthetic(saver().save(KCFIListPrefix + hex), c));
    };

    // The mismatch routine is the weak default, the trap, or a compiled
    // routine, which jumps to one of the scanners. Anything else, such as a
    // routine of another form, is left as it is.
    bool opened = false;
    for (const RoutineKind &k : machineKinds) {
      Symbol *m = symtab.find((Twine(k.mismatch) + hex).str());
      bool replace;
      if (auto *u = dyn_cast_or_null<Undefined>(m)) {
        Defined *d = u->getDefinedWeakAlias();
        if (!d || d->getName() != KCFITrap)
          continue;
        replace = true;
      } else if (Symbol *scanner = compiledScanner(m, k)) {
        replace = opening.dynamic && scanner->getName() == k.staticScanner;
      } else {
        continue;
      }
      opened = true;
      if (!replace)
        continue;
      StringRef name = opening.dynamic ? k.dynamicScanner : k.staticScanner;
      auto *scanner = dyn_cast_or_null<Defined>(symtab.find(name));
      if (!scanner) {
        Err(ctx) << "cannot open KCFI type " << hex << ": " << name
                 << " is not defined";
        continue;
      }
      if (!head)
        addHead();
      keep(scanner);
      keep(head);
      auto *routine = make<KCFIOpenChunk>(ctx, head, scanner, opening.dynamic);
      ctx.kcfi.chunks.push_back(routine);
      replaceSymbol<DefinedSynthetic>(m, m->getName(), routine);
      if (opening.local)
        if (auto *s = dyn_cast_or_null<Defined>(symtab.find(k.staticScanner)))
          ctx.kcfi.localRoutines.push_back({routine, s});
    }
    if (!opened || opening.entries.empty())
      continue;

    if (!head)
      addHead();
    StringRef section = saver().save(KCFIListSectionPrefix + hex + "_m");
    for (auto [target, viaCell] : opening.entries) {
      Defined *entry = target;
      if (viaCell) {
        auto *cell = make<KCFIListChunk>(ctx, ".rdata", target);
        ctx.kcfi.chunks.push_back(cell);
        entry = make<DefinedSynthetic>(target->getName(), cell);
      } else if (auto *imp = dyn_cast<DefinedImportData>(target)) {
        ctx.kcfi.listedImports.push_back({imp, section});
      }
      ctx.kcfi.chunks.push_back(make<KCFIListChunk>(ctx, section, entry));
    }
  }

  // In an image whose guard function table lists a function without a prefix
  // that foreign code in the image lists, the writer sends a mismatch that
  // would fail fast to the dynamic scanner, and the routines that only such
  // code opened, where it references no import, to the static scanner. The
  // table is known only once the garbage collector has run, so the scanners
  // are kept wherever a foreign object may list such a function.
  if (!(ctx.config.guardCF & GuardCFLevel::CF))
    return;
  // A function with a prefix of ours is the entry that follows the prefix.
  auto isPrefixedEntry = [&](Symbol *s) {
    auto *r = dyn_cast<DefinedRegular>(s);
    return r && r->getChunk() &&
           (r->getChunk()->getOutputCharacteristics() &
            IMAGE_SCN_MEM_EXECUTE) &&
           !isForeign(r);
  };
  SmallVector<ObjFile *, 0> foreign;
  bool mayList = false;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!scan(file))
      continue;
    foreign.push_back(file);
    mayList = mayList || mayListUnprefixed(symtab, file, isPrefixedEntry);
  }
  if (!mayList)
    return;
  for (const RoutineKind &k : machineKinds)
    if (auto *scanner =
            dyn_cast_or_null<Defined>(symtab.find(k.dynamicScanner)))
      keep(scanner);
  // Foreign code that references no import can hand ours only functions in
  // the image, which the bound accepts, so a type that only such code opened
  // need not be open dynamically. Foreign code that references one can hand
  // on pointers that it obtained from any DLL at run time.
  if (llvm::any_of(foreign, [](ObjFile *file) {
        return llvm::any_of(file->getSymbols(), [](Symbol *s) {
          return isa_and_nonnull<DefinedImportData, DefinedImportThunk>(s);
        });
      })) {
    ctx.kcfi.localRoutines.clear();
    return;
  }
  for (auto [routine, staticScanner] : ctx.kcfi.localRoutines)
    keep(staticScanner);
}

// The import library of a DLL with a code range holds the record of the range
// in a member that defines one absolute symbol, named for the DLL, by which
// the linker of an image that imports the DLL finds it without loading it,
// and imports of the range's bounds named for the DLL, <bound>$<dll>, exported
// as the bounds, so that no two DLLs' meet in an importer's symbol table.
void addKCFIRangeImports(COFFLinkerContext &ctx, StringRef dllName,
                         std::vector<COFFShortExport> &exports,
                         std::vector<NewArchiveMember> &members,
                         std::vector<uint8_t> &buffer) {
  if (!ctx.kcfi.rangeDefined || !ctx.kcfi.rangeExports[0])
    return;
  std::string dll = dllName.lower();
  for (StringRef bound : {StringRef(KCFICodeStart), StringRef(KCFICodeEnd)}) {
    // The bound's own export, which has no import, gives its hint.
    auto it = llvm::find_if(
        exports, [&](const COFFShortExport &e) { return e.Name == bound; });
    if (it == exports.end())
      return;
    COFFShortExport e;
    e.Name = (bound + "$" + dll).str();
    e.ExportAs = bound.str();
    e.Ordinal = it->Ordinal;
    e.Data = true;
    exports.push_back(std::move(e));
  }

  std::string group;
  raw_string_ostream g(group);
  for (const auto *types : {&ctx.kcfi.rangeTypes, &ctx.kcfi.rangeVfnTypes}) {
    SmallVector<std::pair<uint32_t, uint32_t>, 0> sorted(types->begin(),
                                                         types->end());
    llvm::sort(sorted);
    encodeULEB128(sorted.size(), g);
    for (auto [type, functions] : sorted) {
      support::endian::write<uint32_t>(g, type, llvm::endianness::little);
      encodeULEB128(functions, g);
    }
  }
  std::string records;
  raw_string_ostream r(records);
  r.write(LinkRecordsMagic, sizeof(LinkRecordsMagic));
  encodeULEB128(LinkRecordsVersion, r);
  encodeULEB128(0, r);
  encodeULEB128(LinkRecordImageCode, r);
  encodeULEB128(group.size(), r);
  r << group;
  members.push_back(createLinkerFacts(
      dllName, ctx.config.machine, ".llvm_link_records",
      arrayRefFromStringRef(records), "__llvm_code_range$" + dll, buffer));
}

// A DLL whose objects have KCFI prefixes exports the bounds of its KCFI code
// range, which the writer defines, as private data, so that the KCFI checks of
// an image that imports it can take a target inside the range directly. A DLL
// without a range exports an empty one, so that an importer linked against a
// release with one still loads. The names are reserved for these exports.
void addKCFIRangeExports(COFFLinkerContext &ctx) {
  if (!ctx.config.dll || !ctx.typePrefixRecords || ctx.hybridSymtab ||
      (ctx.config.machine != AMD64 && ctx.config.machine != ARM64))
    return;
  static const StringRef names[] = {KCFICodeStart, KCFICodeEnd};
  std::vector<Export> &exports = ctx.symtab.exports;
  llvm::erase_if(exports, [&](const Export &e) {
    StringRef name = !e.exportAs.empty()  ? StringRef(e.exportAs)
                     : !e.extName.empty() ? StringRef(e.extName)
                                          : StringRef(e.name);
    if (!llvm::is_contained(names, name) && !llvm::is_contained(names, e.name))
      return false;
    if (e.source != ExportSource::ExportAll)
      Err(ctx) << "cannot export " << e.name
               << (name == e.name ? "" : " as " + name.str())
               << ": the name is reserved for the KCFI code range";
    return true;
  });
  for (int i : {0, 1}) {
    ctx.kcfi.rangeExports[i] = make<DefinedSynthetic>(names[i], nullptr);
    Export e;
    e.name = names[i];
    e.sym = ctx.kcfi.rangeExports[i];
    e.data = true;
    e.isPrivate = true;
    e.source = ExportSource::Export;
    exports.push_back(e);
  }
}

// Reads the record of a DLL's KCFI code range from the member of its import
// library that holds it, returning false if the member holds none this linker
// can read.
static bool readKCFIRangeRecord(COFFLinkerContext &ctx, MemoryBufferRef mb,
                                DenseMap<uint32_t, uint32_t> (&types)[2]) {
  Expected<std::unique_ptr<COFFObjectFile>> obj = COFFObjectFile::create(mb);
  if (!obj) {
    consumeError(obj.takeError());
    return false;
  }
  if ((*obj)->getMachine() != ctx.config.machine)
    return false;
  for (const SectionRef &sec : (*obj)->sections()) {
    Expected<StringRef> name = sec.getName();
    Expected<StringRef> contents = sec.getContents();
    if (!name || !contents || *name != ".llvm_link_records") {
      consumeError(name.takeError());
      consumeError(contents.takeError());
      continue;
    }
    DataExtractor data(*contents, /*IsLittleEndian=*/true);
    DataExtractor::Cursor cur(0);
    if (data.getBytes(cur, sizeof(LinkRecordsMagic)) !=
            StringRef(LinkRecordsMagic, sizeof(LinkRecordsMagic)) ||
        data.getULEB128(cur) != LinkRecordsVersion) {
      consumeError(cur.takeError());
      return false;
    }
    data.getULEB128(cur);
    while (cur && !data.eof(cur)) {
      uint64_t kind = data.getULEB128(cur);
      uint64_t size = data.getULEB128(cur);
      uint64_t start = cur.tell();
      data.skip(cur, size);
      if (!cur || kind != LinkRecordImageCode)
        continue;
      DataExtractor group(contents->substr(start, size), true);
      DataExtractor::Cursor c(0);
      for (DenseMap<uint32_t, uint32_t> &map : types) {
        uint64_t count = group.getULEB128(c);
        for (uint64_t i = 0; c && i != count; ++i) {
          uint32_t type = group.getU32(c);
          map[type] = group.getULEB128(c);
        }
      }
      bool ok = c && group.eof(c);
      consumeError(c.takeError());
      consumeError(cur.takeError());
      return ok;
    }
    consumeError(cur.takeError());
  }
  return false;
}

// In a sealed image, with a guard function table, the KCFI checks of an image
// may take a target directly inside the code range of a DLL it imports
// statically, whose import library records the range. The image imports the
// range's bounds from each such DLL whose unsealed functions there have a type
// that one of its thunks checks, as data the loader binds into the import
// address table, which is read-only once the imports are bound. The imports
// are named for the DLL, so that no two DLLs' meet in the symbol table.
void bindKCFIImportedRanges(COFFLinkerContext &ctx) {
  bool isX64 = ctx.config.machine == AMD64;
  if (!ctx.typePrefixRecords || !(ctx.config.guardCF & GuardCFLevel::CF) ||
      ctx.hybridSymtab || (!isX64 && ctx.config.machine != ARM64))
    return;

  // The types, and second types, that the image's thunks check, as a prefix
  // stores them. The writer replaces dispatch thunks on x86-64 only.
  DenseSet<uint32_t> checked[2];
  for (auto &[sym, thunk] : ctx.kcfi.thunks) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !d->getChunk() || !d->getChunk()->live ||
        (thunk.kind == LinkKCFIThunkDispatch && !isX64))
      continue;
    checked[thunk.kind == LinkKCFIThunkVfnCheck].insert(
        isX64 ? getX86KCFIType(thunk.type) : thunk.type);
  }
  if (checked[0].empty() && checked[1].empty())
    return;

  // The records of the DLLs the image imports statically, in load order, with
  // the imports of the ranges' bounds that their import libraries hold.
  struct Record {
    LazyArchive *bounds[2] = {};
    DenseMap<uint32_t, uint32_t> types[2];
    bool bound = false;
  };
  SmallVector<Record, 0> records;
  StringSet<> seen;
  for (ImportFile *file : ctx.importFileInstances) {
    std::string dll = StringRef(file->dllName).lower();
    if (!file->live || ctx.config.delayLoads.contains(dll) ||
        !seen.insert(dll).second)
      continue;
    auto *lazy = dyn_cast_or_null<LazyArchive>(
        ctx.symtab.find("__llvm_code_range$" + dll));
    Record r;
    for (int i : {0, 1})
      r.bounds[i] = dyn_cast_or_null<LazyArchive>(ctx.symtab.find(
          (Twine("__imp_") + (i ? KCFICodeEnd : KCFICodeStart) + "$" + dll)
              .str()));
    if (lazy && r.bounds[0] && r.bounds[1] &&
        readKCFIRangeRecord(ctx, lazy->getMemberBuffer(), r.types))
      records.push_back(std::move(r));
  }

  // A thunk tests the ranges of the DLLs with the most functions of its type,
  // at most four, those with more first; only those are bound.
  for (int vfn : {0, 1}) {
    for (uint32_t type : checked[vfn]) {
      SmallVector<std::pair<uint32_t, uint32_t>, 4> holders;
      for (auto [i, r] : llvm::enumerate(records))
        if (uint32_t n = r.types[vfn].lookup(type))
          holders.push_back({n, i});
      if (holders.empty())
        continue;
      llvm::stable_sort(holders,
                        [](auto &a, auto &b) { return a.first > b.first; });
      SmallVector<uint32_t, 4> &tested = ctx.kcfi.rangeHolders[vfn][type];
      for (auto [n, i] : ArrayRef(holders).take_front(4)) {
        records[i].bound = true;
        tested.push_back(i);
      }
    }
  }

  // The ranges are bound in load order, and each thunk's list names them by
  // their index.
  SmallVector<uint32_t, 0> rangeOf(records.size());
  for (auto [i, r] : llvm::enumerate(records)) {
    if (!r.bound)
      continue;
    rangeOf[i] = ctx.kcfi.importedRanges.size();
    DefinedImportData *bounds[2];
    for (int b : {0, 1}) {
      auto *impFile = make<ImportFile>(ctx, r.bounds[b]->getMemberBuffer());
      ctx.driver.addFile(impFile);
      impFile->live = true;
      bounds[b] = impFile->impSym;
    }
    ctx.kcfi.importedRanges.push_back({bounds[0], bounds[1]});
  }
  for (DenseMap<uint32_t, SmallVector<uint32_t, 4>> &holders :
       ctx.kcfi.rangeHolders)
    for (auto &[type, tested] : holders)
      for (uint32_t &i : tested)
        i = rangeOf[i];
}

// An entry of a KCFI type's list is the address of a word that the type's
// open routine reads whole.
bool isKCFIListEntry(SectionChunk *sc, const coff_relocation &rel) {
  StringRef name = sc->getSectionName();
  return name.starts_with(KCFIListSectionPrefix) && name.ends_with("_m") &&
         sc->isAddressWord(rel);
}

void getKCFIImportUses(COFFLinkerContext &ctx,
                       SmallVectorImpl<DefinedImportData *> &read,
                       SmallVectorImpl<DefinedImportData *> &other) {
  for (auto [imp, section] : ctx.kcfi.listedImports)
    read.push_back(imp);
  for (const KCFIState::ImportedRange &r : ctx.kcfi.importedRanges) {
    other.push_back(r.start);
    other.push_back(r.end);
  }
}

// A KCFI type's list names an imported function by its import address table
// entry, which holds the function's address. Where static data holds the
// function's import thunk as its address instead, a call through such a
// pointer reaches the thunk, so the list names a cell holding the thunk too:
// without -import-slots, with it for a function an object may take the
// address of in an instruction it does not describe, and for a delay-loaded
// function. Both the linker's own lists and those of the objects whose records
// ask for it are completed.
void KCFIContents::listThunks() {
  Configuration &config = ctx.config;
  auto thunkIsAddress = [&](DefinedImportData *imp) {
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(imp->file->thunkSym);
    return thunk && thunk->getChunk()->live &&
           (!config.importSlots || imp->file->thunkIsAddress ||
            config.delayLoads.contains(imp->getDLLName().lower()));
  };
  SetVector<std::pair<ImportFile *, StringRef>> listed;
  for (auto [imp, section] : ctx.kcfi.listedImports)
    if (thunkIsAddress(imp))
      listed.insert({imp->file, section});
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab || !file->listsKCFIImports())
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      StringRef name = sc->getSectionName();
      if (!name.starts_with(KCFIListSectionPrefix) || !name.ends_with("_m"))
        continue;
      for (const coff_relocation &rel : sc->getRelocs()) {
        auto *imp = dyn_cast_or_null<DefinedImportData>(
            file->getSymbol(rel.SymbolTableIndex));
        if (imp && !imp->isRuntimePseudoReloc && thunkIsAddress(imp))
          listed.insert({imp->file, name});
      }
    }
  }
  for (auto [file, section] : listed) {
    auto *thunk = cast<DefinedImportThunk>(file->thunkSym);
    auto *cell = make<KCFIListChunk>(ctx, ".rdata", thunk);
    ctx.kcfi.chunks.push_back(cell);
    ctx.kcfi.chunks.push_back(make<KCFIListChunk>(
        ctx, section, make<DefinedSynthetic>(thunk->getName(), cell)));
  }
}

// Once the sections are merged, and before they are laid out, defines the code
// range of a sealed image, puts the linker's KCFI thunks in place of clang's,
// and points the range's bounds, and those a DLL exports, at their chunks.
void KCFIContents::layOut(OutputSection *textSec, OutputSection *rdataSec) {
  defineCodeRange();
  replaceThunks();
  placeCodeRange();
  defineRangeExports(textSec, rdataSec);
}

// Once the guard function table is known, bounds the mismatches of the image's
// closed types and narrows the misses of its member thunks.
void KCFIContents::bound(OutputSection *textSec, OutputSection *rdataSec) {
  boundMismatches(textSec, rdataSec);
  narrowMemberMisses();
}

// Defines __llvm_code_start and __llvm_code_end, which clang's KCFI thunks test
// to take a target inside the image directly, as the bounds of the code in the
// output section that holds the KCFI prefixes of a sealed image. They keep
// clang's weak default, __llvm_code_empty, a byte in a COMDAT, and so an empty
// range, unless every prefix is in one output section.
//
// The range ends before the first chunk holding an export-suppressed function.
// Such chunks of the section's plain .text group follow its other chunks, where
// /order does not place them, so that the range keeps the rest; the groups
// with a $ suffix keep their order after it.
void KCFIContents::defineCodeRange() {
  if (!prefixes.isSealed() || prefixes.get().empty())
    return;
  // Output sections are not yet indexed, so the section is found by its
  // chunks.
  DenseSet<const Chunk *> prefixed;
  for (const TypePrefix &p : prefixes.get())
    prefixed.insert(p.chunk);
  auto holdsAll = [&](OutputSection *s) {
    return llvm::count_if(s->chunks, [&](Chunk *c) {
             return prefixed.contains(c);
           }) == ptrdiff_t(prefixed.size());
  };
  auto secIt = llvm::find_if(ctx.outputSections, holdsAll);
  if (secIt == ctx.outputSections.end())
    return;
  OutputSection *sec = *secIt;
  // Each bound is replaced only while it is the weak alias resolved to that
  // default, the leader of its COMDAT. An image whose code references
  // neither still has a range, which a DLL exports.
  auto isEmptyDefault = [](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    return d && d->getValue() == 0 && d->getChunk()->sym &&
           d->getChunk()->sym->getName() == KCFICodeEmpty;
  };
  // An archive indexes a bound that a member references weakly, so a bound
  // that no loaded object references can be lazy, which defines nothing.
  auto isUserDefined = [&](Symbol *s) {
    return s && !s->isLazy() && !isEmptyDefault(s);
  };
  Symbol *start = ctx.symtab.find(KCFICodeStart);
  Symbol *end = ctx.symtab.find(KCFICodeEnd);
  if (isUserDefined(start) || isUserDefined(end))
    return;
  if (!start)
    start = make<DefinedSynthetic>(KCFICodeStart, nullptr);
  if (!end)
    end = make<DefinedSynthetic>(KCFICodeEnd, nullptr);

  std::vector<Chunk *> &chunks = sec->chunks;
  auto isPlainText = [](const Chunk *c) {
    auto *sc = dyn_cast<SectionChunk>(c);
    return sc && sc->getSectionName() == ".text";
  };
  auto isMovable = [&](const Chunk *c) {
    auto *sc = cast<SectionChunk>(c);
    return prefixes.isSuppressed(sc) &&
           !(sc->sym && ctx.config.order.count(sc->sym->getName()));
  };
  auto it = llvm::find_if(chunks, isPlainText);
  auto groupEnd = std::find_if_not(it, chunks.end(), isPlainText);
  std::stable_partition(it, groupEnd,
                        [&](const Chunk *c) { return !isMovable(c); });

  auto limit = llvm::find_if(
      chunks, [&](const Chunk *c) { return prefixes.isSuppressed(c); });
  if (limit == chunks.begin())
    return;
  DenseSet<const Chunk *> kcfiInRange(chunks.begin(), limit);
  // The types, and the second types of functions that can occupy a vtable
  // slot, that an unsealed prefix in the range has, with how many do. Where
  // the word before the marker is a membership tag, which LTO gives, it is no
  // second type.
  DenseSet<std::pair<const Chunk *, uint32_t>> tags;
  for (Symbol *s : ctx.kcfi.memberTags)
    if (auto *d = dyn_cast<DefinedRegular>(s))
      tags.insert({d->getChunk(), d->getValue()});
  for (const TypePrefix &p : prefixes.get()) {
    if (p.sealed || !kcfiInRange.contains(p.chunk))
      continue;
    const uint8_t *words = p.chunk->getContents().data() + p.offset;
    ++ctx.kcfi.rangeTypes[read32le(words + p.size - 4)];
    if (p.size == 16 && !tags.contains({p.chunk, p.offset}))
      ++ctx.kcfi.rangeVfnTypes[read32le(words)];
  }
  kcfiCodeEndChunk = limit == chunks.end() ? nullptr : *limit;
  replaceSymbol<DefinedSynthetic>(start, start->getName(), nullptr);
  replaceSymbol<DefinedSynthetic>(end, end->getName(), nullptr);
  kcfiCodeStart = cast<Defined>(start);
  kcfiCodeEnd = cast<Defined>(end);
  kcfiCodeSec = sec;
  ctx.kcfi.rangeDefined = true;
}

// Points the bounds of the code range at its first chunk and at the end of its
// last, once the KCFI thunks, which may be either, are in their final form.
void KCFIContents::placeCodeRange() {
  if (!kcfiCodeSec)
    return;
  std::vector<Chunk *> &chunks = kcfiCodeSec->chunks;
  replaceSymbol<DefinedSynthetic>(kcfiCodeStart, kcfiCodeStart->getName(),
                                  chunks.front());
  if (kcfiCodeEndChunk)
    replaceSymbol<DefinedSynthetic>(kcfiCodeEnd, kcfiCodeEnd->getName(),
                                    kcfiCodeEndChunk);
  else
    replaceSymbol<DefinedSynthetic>(kcfiCodeEnd, kcfiCodeEnd->getName(),
                                    chunks.back(), chunks.back()->getSize());
}

// Points the bounds that a DLL exports for its importers' KCFI checks at its
// code range, or, where it has none, both at one address, an empty range. That
// address is not in the export directory, where the loader would take it for
// the name of a forwarded export.
void KCFIContents::defineRangeExports(OutputSection *textSec,
                                      OutputSection *rdataSec) {
  auto [start, end] = ctx.kcfi.rangeExports;
  if (!start)
    return;
  // A DLL whose export table comes from its objects exports neither, so its
  // import library describes no range.
  if (isa<SectionChunk>(ctx.symtab.edataStart))
    ctx.kcfi.rangeDefined = false;
  if (kcfiCodeSec) {
    std::vector<Chunk *> &chunks = kcfiCodeSec->chunks;
    replaceSymbol<DefinedSynthetic>(start, start->getName(), chunks.front());
    if (kcfiCodeEndChunk)
      replaceSymbol<DefinedSynthetic>(end, end->getName(), kcfiCodeEndChunk);
    else
      replaceSymbol<DefinedSynthetic>(end, end->getName(), chunks.back(),
                                      chunks.back()->getSize());
    return;
  }
  // The first code in .text, or, in a DLL without code, a word of its own.
  auto it = llvm::find_if(textSec->chunks,
                          [](const Chunk *c) { return c->getSize(); });
  Chunk *c;
  if (it != textSec->chunks.end()) {
    c = *it;
  } else {
    c = make<KCFIListChunk>(ctx, ".rdata", nullptr);
    rdataSec->addChunk(c);
  }
  replaceSymbol<DefinedSynthetic>(start, start->getName(), c);
  replaceSymbol<DefinedSynthetic>(end, end->getName(), c);
}

// In an image whose guard function table lists a function without a KCFI
// prefix, foreign code linked into the image can hand ours one of its
// functions through a channel that no type rule sees. Where a mismatch would
// fail fast, at the routine of a closed type or at the end of a statically
// open type's list, a target in the image then goes to the dynamic scanner
// instead: the scanner fails fast on a target with our marker, and the guard
// function it continues into accepts only what this image's table lists. A
// target outside the image fails fast as before. A closed type's routine
// points the scanner at an empty list, and the static scanner is entered
// through a routine that gives it only targets outside the image, and the
// dynamic scanner the rest, with the same list. The scanners are clang's, as
// only clang knows the marker and the prefix offset it compares.
void KCFIContents::boundMismatches(OutputSection *textSec,
                                   OutputSection *rdataSec) {
  bool isX64 = ctx.config.machine == AMD64;
  if (!prefixes.hasUnprefixedTargets() ||
      (!isX64 && ctx.config.machine != ARM64))
    return;
  auto *trap = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(KCFITrap));
  Defined *empty = nullptr;
  // openKCFITypes kept the scanners for these routines, and the routines that
  // only foreign code which references no import opened.
  for (const RoutineKind &k : getRoutineKinds(ctx)) {
    auto *dynamic =
        dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(k.dynamicScanner));
    if (!dynamic || !dynamic->getChunk()->live)
      continue;
    for (auto [routine, staticScanner] : ctx.kcfi.localRoutines) {
      auto *s = dyn_cast<DefinedRegular>(staticScanner);
      if (s && s->getName() == k.staticScanner && s->getChunk()->live)
        routine->setStatic(s);
    }
    KCFIOpenChunk *closed = nullptr, *open = nullptr;
    // A closed type's routine is the trap, to which the weak default was
    // resolved.
    if (trap && trap->isLive()) {
      ctx.symtab.forEachSymbol([&](Symbol *s) {
        auto *d = dyn_cast<DefinedRegular>(s);
        if (!d || d->getChunk() != trap->getChunk() ||
            d->getValue() != trap->getValue() ||
            !d->getName().starts_with(k.mismatch))
          return;
        if (!closed) {
          // The head, then the odd word that ends the list.
          if (!empty) {
            auto *head = make<KCFIListChunk>(ctx, ".rdata", nullptr);
            rdataSec->addChunk(head);
            rdataSec->addChunk(make<KCFIListChunk>(ctx, ".rdata", nullptr, 1));
            empty = make<DefinedSynthetic>("__llvm_kcfi_list_empty", head);
          }
          closed =
              make<KCFIOpenChunk>(ctx, empty, dynamic, false, trap, k.check);
          textSec->addChunk(closed);
        }
        replaceSymbol<DefinedSynthetic>(s, s->getName(), closed);
      });
    }
    // Every routine that jumps to the static scanner does so through its
    // symbol.
    auto *scanner =
        dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(k.staticScanner));
    if (scanner && scanner->isLive()) {
      auto *outside = make<DefinedSynthetic>(
          scanner->getName(), scanner->getChunk(), scanner->getValue());
      open =
          make<KCFIOpenChunk>(ctx, nullptr, dynamic, false, outside, k.check);
      textSec->addChunk(open);
      replaceSymbol<DefinedSynthetic>(scanner, scanner->getName(), open);
    }
  }
}

// A member thunk, which takes a target that carries one of its type's
// membership tags, continues on a miss into the type's ordinary thunk through
// a weak symbol: a function of the type that LTO did not compile carries no
// tag, but may be a valid target. In a sealed image the guard function table
// names every function a pointer can reach, so where no unsealed prefix of
// the type is in an object LTO did not generate, a target that misses the tag
// is no member, and the miss goes straight to the type's mismatch routine.
void KCFIContents::narrowMemberMisses() {
  bool isX64 = ctx.config.machine == AMD64;
  if (!prefixes.isSealed() || (!isX64 && ctx.config.machine != ARM64))
    return;
  DenseSet<uint32_t> nativeTypes;
  for (const TypePrefix &p : prefixes.get())
    if (!p.sealed && !cast<ObjFile>(p.chunk->file)->ltoOutput)
      nativeTypes.insert(
          read32le(p.chunk->getContents().data() + p.offset + p.size - 4));

  struct Kind {
    StringRef miss, mismatch;
  };
  static const Kind kinds[] = {
      {KCFIMemberMissPrefix, KCFIMismatchPrefix},
      {KCFIMemberCheckMissPrefix, KCFICheckMismatchPrefix},
      {KCFIMemberLocalMissPrefix, KCFIMismatchPrefix},
      {KCFIMemberLocalCheckMissPrefix, KCFICheckMismatchPrefix}};
  ctx.symtab.forEachSymbol([&](Symbol *s) {
    auto *d = dyn_cast<Defined>(s);
    if (!d || !d->isLive())
      return;
    for (const Kind &k : kinds) {
      StringRef hex = d->getName();
      uint32_t type;
      if (!hex.consume_front(k.miss) || hex.getAsInteger(16, type))
        continue;
      // x86-64 stores a type that would spell an ENDBR instruction plus one.
      if (nativeTypes.contains(isX64 ? getX86KCFIType(type) : type))
        return;
      auto *mismatch =
          dyn_cast_or_null<Defined>(ctx.symtab.find((k.mismatch + hex).str()));
      if (!mismatch)
        return;
      Chunk *c;
      uint32_t offset = 0;
      if (auto *r = dyn_cast<DefinedRegular>(mismatch)) {
        c = r->getChunk();
        offset = r->getValue();
      } else if (auto *syn = dyn_cast<DefinedSynthetic>(mismatch)) {
        c = syn->getChunk();
      } else {
        return;
      }
      replaceSymbol<DefinedSynthetic>(d, d->getName(), c, offset);
      return;
    }
  });
}

// Builds the type check of one of clang's KCFI thunks without its branch, and
// its page test, from the facts the thunk's object records, as clang encodes
// them. Returns false where the thunk's form cannot be built.
static bool buildKCFIThunkCheck(MachineTypes machine, const KCFIState::Thunk &t,
                                SmallVectorImpl<uint8_t> &compare,
                                SmallVectorImpl<uint8_t> &pageTest) {
  bool vfn = t.kind == LinkKCFIThunkVfnCheck;
  // A vfn thunk compares the second type and the start of the marker, 16
  // bytes before the entry; any other the end of the marker and the type, 8
  // bytes before it. A target outside the code range is compared only where
  // the prefix read, 16 or 12 bytes and any patchable prefix, cannot start on
  // the page before.
  uint32_t compareOffset = t.offset + (vfn ? 16 : 8);
  uint32_t readLog2 = Log2_64_Ceil(t.offset + (vfn ? 16 : 12));
  uint32_t pageMask = readLog2 < 12 ? 0xFFF & ~((1u << readLog2) - 1) : 0;
  uint64_t pattern = getTypePrefixPattern(t.marker);
  // x86-64 stores a type that would spell an ENDBR instruction plus one.
  uint32_t type = machine == AMD64 ? getX86KCFIType(t.type) : t.type;
  uint64_t expected =
      vfn ? type | pattern << 32 : pattern >> 32 | uint64_t(type) << 32;

  if (machine == ARM64) {
    // ldur x16, [x15, #-offset]; mov x17, #expected; cmp x16, x17
    // tst x15, #mask
    if (compareOffset > 256 || !pageMask)
      return false;
    auto insn = [&](SmallVectorImpl<uint8_t> &v, uint32_t i) {
      v.resize(v.size() + 4);
      write32le(v.end() - 4, i);
    };
    insn(compare, 0xF84001F0 | (-compareOffset & 0x1FF) << 12);
    insn(compare, 0xD2800011 | (expected & 0xFFFF) << 5);
    insn(compare, 0xF2A00011 | (expected >> 16 & 0xFFFF) << 5);
    insn(compare, 0xF2C00011 | (expected >> 32 & 0xFFFF) << 5);
    insn(compare, 0xF2E00011 | (expected >> 48 & 0xFFFF) << 5);
    insn(compare, 0xEB11021F);
    insn(pageTest,
         0xF24001FF | ((64 - readLog2) & 63) << 16 | (11 - readLog2) << 10);
    return true;
  }

  // movabs r11, expected; cmp [reg - offset], r11; test reg32, mask
  bool rax = t.kind == LinkKCFIThunkDispatch;
  compare.append({0x49, 0xBB});
  compare.resize(compare.size() + 8);
  write64le(compare.end() - 8, expected);
  uint8_t rm = rax ? 0 : 1;
  if (compareOffset <= 128) {
    compare.append({0x4C, 0x39, uint8_t(0x58 | rm), uint8_t(-compareOffset)});
  } else {
    compare.append({0x4C, 0x39, uint8_t(0x98 | rm)});
    compare.resize(compare.size() + 4);
    write32le(compare.end() - 4, -compareOffset);
  }
  if (rax)
    pageTest.push_back(0xA9);
  else
    pageTest.append({0xF7, 0xC1});
  pageTest.resize(pageTest.size() + 4);
  write32le(pageTest.end() - 4, pageMask);
  return true;
}

// Replaces each KCFI thunk that the image keeps and its object describes,
// __llvm_kcfi_dispatch_<type> with the target in RAX, and
// __llvm_kcfi_check_<type> and __llvm_kcfi_vfn_check_<type> with it in RCX or
// X15, with a KCFIThunkChunk in its place, in an image the linker sealed. The
// chunk tests the image's own range as one comparison of the target's offset
// from its start with its size, which the linker knows, and only for a type
// that an unsealed prefix in the range has, since no other target in the image
// can match; then the ranges of the DLLs the image imports whose records list
// an unsealed function of the type, for the same reason. It builds clang's
// type check and page test from the marker, the patchable prefix and the type
// that the thunk's record gives.
void KCFIContents::replaceThunks() {
  bool isX64 = ctx.config.machine == AMD64;
  if (!prefixes.isSealed() || (!isX64 && ctx.config.machine != ARM64))
    return;

  DenseMap<const Chunk *, Chunk *> replacements;
  SetVector<ObjFile *> files;
  for (auto &[sym, thunk] : ctx.kcfi.thunks) {
    // The record describes the thunk only if the chunk is the thunk's own.
    auto *d = dyn_cast<DefinedRegular>(sym);
    SectionChunk *sc = d ? d->getChunk() : nullptr;
    if (!sc || !sc->live || sc->sym != d || d->getValue() != 0 ||
        (thunk.kind == LinkKCFIThunkDispatch && !isX64))
      continue;
    auto *mismatchSym = dyn_cast<Defined>(thunk.mismatch);
    auto *guardSym = dyn_cast_or_null<Defined>(ctx.symtab.find(
        thunk.kind == LinkKCFIThunkDispatch ? "__guard_dispatch_icall_fptr"
                                            : "__guard_check_icall_fptr"));
    SmallVector<uint8_t, 24> compare, pageTest;
    if (!mismatchSym || !guardSym ||
        !buildKCFIThunkCheck(ctx.config.machine, thunk, compare, pageTest))
      continue;

    // x86-64 stores a type that would spell an ENDBR instruction plus one.
    bool vfn = thunk.kind == LinkKCFIThunkVfnCheck;
    uint32_t stored = isX64 ? getX86KCFIType(thunk.type) : thunk.type;
    bool own =
        kcfiCodeSec &&
        (vfn ? ctx.kcfi.rangeVfnTypes : ctx.kcfi.rangeTypes).count(stored);
    // The ranges of the DLLs with an unsealed function of the type that the
    // thunk tests.
    SmallVector<KCFIThunkChunk::Range, 4> imported;
    auto tested = ctx.kcfi.rangeHolders[vfn].find(stored);
    if (tested != ctx.kcfi.rangeHolders[vfn].end())
      for (uint32_t i : tested->second)
        imported.push_back(
            {ctx.kcfi.importedRanges[i].start, ctx.kcfi.importedRanges[i].end});
    kcfiThunksTestRange |= own;
    replacements[sc] = make<KCFIThunkChunk>(
        ctx, thunk.kind != LinkKCFIThunkDispatch, compare, pageTest,
        mismatchSym, guardSym, own ? kcfiCodeStart : nullptr,
        own ? kcfiCodeEnd : nullptr, imported);
    files.insert(cast<ObjFile>(sc->file));
  }
  if (replacements.empty())
    return;

  // Each chunk takes clang's place in its section, and every symbol defined
  // at clang's thunk, its own and those of other objects, such as a weak
  // alias resolved to it, its address.
  for (OutputSection *sec : ctx.outputSections)
    for (Chunk *&c : sec->chunks)
      if (Chunk *r = replacements.lookup(c))
        c = r;
  if (Chunk *r = replacements.lookup(kcfiCodeEndChunk))
    kcfiCodeEndChunk = r;
  auto repoint = [&](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (!d || d->getValue() != 0)
      return;
    if (Chunk *r = replacements.lookup(d->getChunk()))
      replaceSymbol<DefinedSynthetic>(s, s->getName(), r);
  };
  ctx.symtab.forEachSymbol(repoint);
  // The symbols local to an object are not in the symbol table.
  for (ObjFile *file : files)
    for (Symbol *s : file->getSymbols())
      if (auto *d = dyn_cast_or_null<DefinedRegular>(s);
          d && !d->getCOFFSymbol().isExternal())
        repoint(d);
  for (auto &kv : replacements)
    cast<SectionChunk>(const_cast<Chunk *>(kv.first))->live = false;
}

// The KCFI thunks the linker made compare a target's offset from the start of
// the code range with the range's size, as a sign-extended 32-bit immediate.
void KCFIContents::checkCodeRange() {
  if (kcfiThunksTestRange &&
      kcfiCodeEnd->getRVA() - kcfiCodeStart->getRVA() > INT32_MAX)
    Err(ctx) << "the code range of " << ctx.config.outputFile
             << " is too large for its KCFI thunks";
}
} // namespace lld::coff
