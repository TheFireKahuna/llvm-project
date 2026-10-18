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
#include "LabelledCode.h"
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
#include <optional>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {
// The routine a KCFI type check reaches on a mismatch when the linker opens the
// type: points R10/X16 at the type's list and jumps to the compiled scanner,
// which fails fast for a static opening and continues into the guard function
// for a dynamic one. An outside routine first diverts an out-of-image target.
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

// The linker's form of one of clang's KCFI thunks, replacing it in a sealed
// image. A target inside a code range the image vouches for -- its own, and
// each imported DLL's whose record lists a function of the type -- takes
// clang's type check then the jump; any other takes clang's page-test path.
// A local thunk serves a type with no out-of-image target. A member thunk's
// extra entry (for a checked outside target) jumps inside an imported range.
class KCFIThunkChunk : public NonSectionCodeChunk {
public:
  struct Range {
    Defined *start;
    Defined *end;
  };
  KCFIThunkChunk(COFFLinkerContext &ctx, bool check, bool local,
                 ArrayRef<uint8_t> compare, ArrayRef<uint8_t> pageTest,
                 Symbol *mismatch, Symbol *guard, Defined *codeStart,
                 Defined *codeEnd, bool typeInRange, ArrayRef<Range> imported,
                 bool checked);
  size_t getSize() const override { return measure().size; }
  void writeTo(uint8_t *buf) const override { write(buf); }
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override;
  // The offset of the entry for a target whose type is checked.
  uint32_t getCheckedOffset() const { return measure().checked; }
  // Makes the thunk's branches to the mismatch routine reach it as the code
  // right after the thunk, where the writer placed it.
  void setMismatchFollows() {
    mismatchFollows = true;
    measured.reset();
  }

private:
  // Size and checked-entry offset, fixed by the thunk's fields and whether a
  // mismatch routine follows (not by any address), so measured once.
  struct Measured {
    uint32_t size;
    uint32_t checked;
  };
  const Measured &measure() const {
    if (!measured) {
      uint32_t checked = 0;
      uint32_t size = write(nullptr, &checked);
      measured = Measured{size, checked};
    }
    return *measured;
  }
  mutable std::optional<Measured> measured;
  uint32_t write(uint8_t *buf, uint32_t *checkedOffset = nullptr) const;
  void writeAMD64(LabelledCode &a, uint32_t &checkedOffset) const;
  void writeARM64(LabelledCode &a, uint32_t &checkedOffset) const;

  bool check;
  bool local;
  bool checked;
  bool typeInRange;
  bool mismatchFollows = false;
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
// compiler's pieces, or a cell an entry points to. It holds a value, or a
// symbol's address (zero if the symbol is not in the image).
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
  // The image ends at its last section, rounded up to its mapping page.
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

KCFIThunkChunk::KCFIThunkChunk(COFFLinkerContext &ctx, bool check, bool local,
                               ArrayRef<uint8_t> compare,
                               ArrayRef<uint8_t> pageTest, Symbol *mismatch,
                               Symbol *guard, Defined *codeStart,
                               Defined *codeEnd, bool typeInRange,
                               ArrayRef<Range> imported, bool checked)
    : check(check), local(local), checked(checked), typeInRange(typeInRange),
      compare(compare), pageTest(pageTest), mismatch(mismatch), guard(guard),
      codeStart(codeStart), codeEnd(codeEnd), imported(imported), ctx(ctx) {
  setAlignment(16);
}

MachineTypes KCFIThunkChunk::getMachine() const { return ctx.config.machine; }

uint32_t KCFIThunkChunk::write(uint8_t *buf, uint32_t *checkedOffset) const {
  uint32_t checkedAt = 0;
  uint32_t size = LabelledCode::layOut(buf, rva, [&](LabelledCode &a) {
    if (ctx.config.machine == ARM64)
      writeARM64(a, checkedAt);
    else
      writeAMD64(a, checkedAt);
  });
  if (checkedOffset)
    *checkedOffset = checkedAt;
  if (buf && mismatchFollows && cast<Defined>(mismatch)->getRVA() != rva + size)
    Err(ctx) << "the KCFI mismatch routine " << mismatch->getName()
             << " does not follow its thunk";
  return size;
}

// With the own range tested, the thunk is: the range test, the inside-range
// type check and jump, the imported-range tests, then the outside path. The
// type-check constant is loaded once before the range tests, which leave it.
void KCFIThunkChunk::writeAMD64(LabelledCode &a,
                                uint32_t &checkedOffset) const {
  // The target is in RAX for a dispatch thunk and in RCX for a check thunk;
  // R10 and R11 are free, as for the guard function.
  uint8_t reg = check ? 1 : 0;
  uint32_t toMismatch = a.newLabel();
  auto branchToMismatch = [&](uint8_t cc) {
    if (mismatchFollows)
      a.jcc(cc, toMismatch);
    else
      a.jccTo(cc, cast<Defined>(mismatch)->getRVA());
  };
  // Clang's type check loads the expected prefix into R11 with movabs, ten
  // bytes, and compares the target's prefix with R11.
  ArrayRef<uint8_t> load = ArrayRef(compare).take_front(10);
  ArrayRef<uint8_t> compareR11 = ArrayRef(compare).drop_front(10);
  auto typeCheck = [&] {
    a.put(compareR11);
    branchToMismatch(0x85); // jne mismatch
  };
  // jmp rax, or ret
  auto takeTarget = [&] {
    if (check)
      a.put({0xC3});
    else
      a.put({0xFF, 0xE0});
  };
  // lea r10, [rip + end - 1]; sub r10, reg; cmp r10, size; jcc label. The
  // target is in the range exactly where end - 1 - target is below the size.
  auto testRange = [&](uint8_t cc, uint32_t label, bool toRoutine) {
    a.put({0x4C, 0x8D, 0x15, 0, 0, 0, 0});
    a.disp(codeEnd->getRVA() - 1);
    a.put({0x49, 0x29, uint8_t(0xC2 | reg << 3)});
    a.put({0x49, 0x81, 0xFA, 0, 0, 0, 0});
    a.imm32(codeEnd->getRVA() - codeStart->getRVA());
    if (toRoutine)
      branchToMismatch(cc);
    else
      a.jcc(cc, label);
  };
  // cmp reg, [rip + bound]; jb label, or jae label
  auto testBound = [&](Defined *bound, uint8_t cc, uint32_t label) {
    a.put({0x48, 0x3B, uint8_t(0x05 | reg << 3), 0, 0, 0, 0});
    a.disp(bound->getRVA());
    a.jcc(cc, label);
  };
  // jmp [rip + guard]
  auto guardJump = [&] {
    a.put({0xFF, 0x25, 0, 0, 0, 0});
    a.disp(cast<Defined>(guard)->getRVA());
  };
  // The page test; je mismatch; the type check; jne mismatch;
  // jmp [rip + guard]
  auto outsidePath = [&](bool loaded) {
    a.put(pageTest);
    branchToMismatch(0x84);
    if (!loaded)
      a.put(load);
    typeCheck();
    guardJump();
  };

  if (local) {
    if (!codeStart) {
      outsidePath(/*loaded=*/false);
    } else {
      uint32_t fail = a.newLabel();
      if (typeInRange) {
        testRange(0x83, fail, /*toRoutine=*/false);
        a.put(load);
        typeCheck();
        takeTarget();
      } else {
        testRange(0x82, 0, /*toRoutine=*/true);
      }
      // mov ecx, FAST_FAIL_GUARD_ICALL_CHECK_FAILURE_XFG; int 0x29
      a.bind(fail);
      a.put({0xB9, 0x40, 0, 0, 0, 0xCD, 0x29});
    }
    if (mismatchFollows)
      a.bind(toMismatch);
    return;
  }

  bool own = codeStart;
  bool hits = own || !imported.empty();
  if (hits)
    a.put(load);
  uint32_t hit = a.newLabel(), outside = a.newLabel();
  if (own) {
    uint32_t imports = a.newLabel();
    testRange(0x83, imports, /*toRoutine=*/false);
    a.bind(hit);
    typeCheck();
    takeTarget();
    a.bind(imports);
  }
  for (size_t i = 0; i != imported.size(); ++i) {
    if (!own && i + 1 == imported.size()) {
      testBound(imported[i].start, 0x82, outside);
      testBound(imported[i].end, 0x83, outside);
      a.bind(hit);
      typeCheck();
      takeTarget();
      break;
    }
    uint32_t next = a.newLabel();
    testBound(imported[i].start, 0x82, next);
    testBound(imported[i].end, 0x82, hit);
    a.bind(next);
  }
  a.bind(outside);
  outsidePath(/*loaded=*/hits);

  if (checked) {
    // The tests of the imported ranges, each taking the target; then
    // jmp [rip + guard]
    checkedOffset = a.off;
    uint32_t take = a.newLabel();
    for (const Range &r : imported) {
      uint32_t next = a.newLabel();
      testBound(r.start, 0x82, next);
      testBound(r.end, 0x82, take);
      a.bind(next);
    }
    guardJump();
    a.bind(take);
    takeTarget();
  }
  if (mismatchFollows)
    a.bind(toMismatch);
}

// As on x86-64, but the target is in X15 and X16/X17 are free; clang's type
// check uses both, so its constant is not hoisted. A branch to a mismatch
// routine not following the thunk uses a jump at the thunk's end.
void KCFIThunkChunk::writeARM64(LabelledCode &a,
                                uint32_t &checkedOffset) const {
  uint32_t toMismatch = a.newLabel();
  // b.cond with a condition: eq 0, ne 1, hs 2, lo 3.
  auto branchIf = [&](uint32_t cond, uint32_t label) {
    a.branch19(0x54000000 | cond, label);
  };
  auto typeCheck = [&] {
    a.put(compare);
    branchIf(1, toMismatch);
  };
  // adrp x16, start; add x16, x16, :lo12:start; sub x16, x15, x16;
  // mov x17, #size; movk x17, #size, lsl #16; cmp x16, x17; b.cond label
  auto testRange = [&](uint32_t cond, uint32_t label) {
    uint64_t codeSize = codeEnd->getRVA() - codeStart->getRVA();
    a.address(16, codeStart->getRVA());
    a.put32(0xCB1001F0);
    a.put32(0xD2800011 | (codeSize & 0xFFFF) << 5);
    a.put32(0xF2A00011 | (codeSize >> 16) << 5);
    a.put32(0xEB11021F);
    branchIf(cond, label);
  };
  // adrp x16, start; add x16, x16, :lo12:start; ldp x16, x17, [x16];
  // cmp x15, x16; b.lo below; cmp x15, x17; b.cond label
  auto testImported = [&](const Range &r, uint32_t below, uint32_t cond,
                          uint32_t label) {
    a.address(16, r.start->getRVA());
    a.put32(0xA9404610);
    a.put32(0xEB1001FF);
    branchIf(3, below);
    a.put32(0xEB1101FF);
    branchIf(cond, label);
  };
  // adrp x16, guard; ldr x16, [x16, :lo12:guard]; br x16
  auto guardJump = [&] {
    a.loadImport(16, cast<Defined>(guard)->getRVA());
    a.put32(0xD61F0200);
  };
  // The page test; b.eq mismatch; the type check; b.ne mismatch; the guard
  // function
  auto outsidePath = [&] {
    a.put(pageTest);
    branchIf(0, toMismatch);
    typeCheck();
    guardJump();
  };
  auto ret = [&] { a.put32(0xD65F03C0); };

  if (local) {
    if (!codeStart) {
      outsidePath();
    } else {
      uint32_t fail = a.newLabel();
      if (typeInRange) {
        testRange(2, fail);
        typeCheck();
        ret();
      } else {
        testRange(3, toMismatch);
      }
      // mov w0, #FAST_FAIL_GUARD_ICALL_CHECK_FAILURE_XFG; brk #0xf003
      a.bind(fail);
      a.put32(0x52800800);
      a.put32(0xD43E0060);
    }
  } else {
    bool own = codeStart;
    uint32_t hit = a.newLabel(), outside = a.newLabel();
    if (own) {
      uint32_t imports = a.newLabel();
      testRange(2, imports);
      a.bind(hit);
      typeCheck();
      ret();
      a.bind(imports);
    }
    for (size_t i = 0; i != imported.size(); ++i) {
      if (!own && i + 1 == imported.size()) {
        testImported(imported[i], outside, 2, outside);
        a.bind(hit);
        typeCheck();
        ret();
        break;
      }
      uint32_t next = a.newLabel();
      testImported(imported[i], next, 3, hit);
      a.bind(next);
    }
    a.bind(outside);
    outsidePath();
    if (checked) {
      // The tests of the imported ranges, each taking the return; then the
      // guard function; ret
      checkedOffset = a.off;
      uint32_t take = a.newLabel();
      for (const Range &r : imported) {
        uint32_t next = a.newLabel();
        testImported(r, next, 3, take);
        a.bind(next);
      }
      guardJump();
      a.bind(take);
      ret();
    }
  }
  // adrp x17, mismatch; add x17, x17, :lo12:mismatch; br x17
  a.bind(toMismatch);
  if (!mismatchFollows) {
    a.address(17, cast<Defined>(mismatch)->getRVA());
    a.put32(0xD61F0220);
  }
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

// The kinds of mismatch routine, one per thunk kind, with the scanners they
// pass a type's list to: a static scanner fails fast, a dynamic one continues
// into the guard function. ARM64 has only check thunks.
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

// Whether s, which an object references, reaches code without a prefix of ours
// (which the guard table lists for an object lacking guard metadata). A
// reference through __imp_X, which may become a linker-made pointer, reaches X.
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
// prefix of ours: an entry its guard metadata lists, a function its
// address-taken import table reaches, or -- lacking guard metadata, or
// describing address-taking sites -- a function any relocation reaches.
// Liveness, which the writer also checks, is not yet known.
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

// Opens a KCFI type where the compiler could not see that unprefixed code
// reaches it, because the link brings that code in. Clang gives the facts as
// weak externals naming the type (8 hex digits) and a function; each opens the
// type statically or dynamically by how its symbol resolves:
//
// - __kcfi_typeid_<f> = <type>: f's address is taken (static, f listed).
// - __kcfi_inflow_<type>_<g>: a pointer of the type can return through g.
// - __kcfi_param_<type>_<g>: g can receive such a pointer.
// - __kcfi_tinflow_<type>_<called>: a call through the called type returns one
//   of the type, opening it transitively.
//
// A fact may name a node (n<node>) for a record's whole type set instead of a
// type; __kcfi_node_<node>_<type> gives its types. A type opens only where a
// KCFI thunk names its mismatch routine; the opened routines are kept past GC.
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

  // Whether each object is foreign, and the marked-prefix functions: each the
  // first to follow its static __cfi_ label in its chunk.
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

  // Per type: whether it opens dynamically, whether only through in-image
  // foreign code, and the symbols to add to its list, each flagged if its entry
  // points at a cell holding it rather than at the symbol.
  struct Opening {
    bool dynamic = false;
    bool local = false;
    SmallVector<std::pair<Defined *, bool>, 0> entries;
  };
  std::map<uint32_t, Opening> openings;
  std::vector<Symbol *> typeids;
  // A fact's value is the check identifier, held in a COFF absolute symbol.
  auto factCheck = [&](Symbol *s) -> uint32_t {
    if (auto *d = dyn_cast_or_null<DefinedAbsolute>(s->getDefined()))
      return d->getVA();
    return 0;
  };
  // Inflow and param facts by the symbol named, and the nodes they name; each
  // node's types as (check, precise key); the precise types each object opens;
  // and the types a call through a precise key can hand back.
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
    // A lazy fact belongs to an archive member not in the link; its value reads
    // as zero but its precise key could still reach types via loaded objects.
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

  // A type opens dynamically where a precise key reaches it; its routine is
  // keyed by the check identifier. Each key is remembered with whether only
  // local facts reached it, so a call through it follows its tinflow facts
  // (again if a non-local fact arrives later); a type is local only where every
  // key that reached it is.
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

  // An import is listed by its import-table entry (the writer adds a cell for
  // its thunk where static data holds one); an unprefixed definition by a cell
  // holding its address.
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
  // The scanner m jumps to, if m is a compiled mismatch routine of the kind.
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
  // through one can hand back a foreign function, transitively. Such an opening
  // is never local: a cast or import may carry a run-time address from any
  // image.
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
  // The types that only foreign code in the image opened dynamically.
  SmallVector<uint32_t, 0> localTypes;
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

    // The mismatch routine is the weak default (the trap) or a compiled routine
    // that jumps to a scanner; anything else is left as it is.
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
        ctx.kcfi.outsideTypes.insert(type);
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
    if (!opened)
      continue;
    // A function in another image may validly target a type listed for an
    // import or open dynamically, unless only in-image foreign code that
    // references no import opened it.
    bool imported = llvm::any_of(opening.entries, [](auto entry) {
      return isa<DefinedImportData>(entry.first);
    });
    if ((imported || opening.dynamic) &&
        ctx.kcfi.outsideTypes.insert(type).second && !imported && opening.local)
      localTypes.push_back(type);
    if (opening.entries.empty())
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

  // Where the guard table may list an unprefixed function that foreign code
  // lists, a fail-fast mismatch is sent to the dynamic scanner, and routines
  // opened only by import-free foreign code to the static scanner. The table is
  // known only after GC, so the scanners are kept wherever a foreign object may
  // list such a function.
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
  // Import-free foreign code can hand ours only in-image functions, which the
  // bound accepts, so a type opened only by such code need not be dynamic.
  // Foreign code that references an import can pass on pointers from any DLL.
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
  // Import-free foreign code hands ours only in-image functions.
  for (uint32_t type : localTypes)
    ctx.kcfi.outsideTypes.erase(type);
}

// The import library of a DLL with a code range holds the range record in a
// member defining one absolute symbol named for the DLL, which an importer's
// linker finds without loading the member, and imports of the range's bounds
// named <bound>$<dll> so that no two DLLs' bounds meet in an importer's symbol
// table.
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
  members.push_back(
      createLinkRecordMember(ctx, dllName, "__llvm_code_range$" + dll,
                             {{LinkRecordImageCode, group}}, buffer));
}

// A DLL whose objects have KCFI prefixes exports the bounds of its code range
// as private data, so an importer's KCFI checks can take a target inside the
// range directly. A DLL without a range exports an empty one, so an importer
// linked against a release that had one still loads. The names are reserved.
void addKCFIRangeExports(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Add KCFI range exports");
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

// Reads a DLL's KCFI code-range record from the import-library member holding
// it, returning false if it holds none this linker can read.
static bool readKCFIRangeRecord(COFFLinkerContext &ctx, MemoryBufferRef mb,
                                DenseMap<uint32_t, uint32_t> (&types)[2]) {
  bool ok = false;
  forEachLinkRecordGroup(
      mb, ctx.config.machine, [&](uint64_t kind, ArrayRef<uint8_t> payload) {
        if (kind != LinkRecordImageCode)
          return;
        DataExtractor group(payload, /*IsLittleEndian=*/true);
        DataExtractor::Cursor c(0);
        for (DenseMap<uint32_t, uint32_t> &map : types) {
          uint64_t count = group.getULEB128(c);
          for (uint64_t i = 0; c && i != count; ++i) {
            uint32_t type = group.getU32(c);
            map[type] = group.getULEB128(c);
          }
        }
        ok = c && group.eof(c);
        consumeError(c.takeError());
      });
  return ok;
}

// Whether a thunk of kind takes its target in RAX, and whether its type's
// every target is in the image.
static bool isDispatchThunk(LinkKCFIThunkKind kind) {
  return kind == LinkKCFIThunkDispatch || kind == LinkKCFIThunkLocalDispatch;
}
static bool isLocalThunk(LinkKCFIThunkKind kind) {
  return kind == LinkKCFIThunkLocalDispatch || kind == LinkKCFIThunkLocalCheck;
}

// In a sealed image with a guard table, the KCFI checks may take a target
// directly inside the code range of a statically imported DLL whose import
// library records the range. The image imports the range's bounds from each
// such DLL whose unsealed functions there have a type one of its thunks checks,
// as data the loader binds into the (read-only) import address table. The
// imports are named for the DLL so no two DLLs' meet in the symbol table.
void bindKCFIImportedRanges(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Bind KCFI imported ranges");
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
        isLocalThunk(thunk.kind) ||
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
    if (!file->live || file->isDelayLoaded() || !seen.insert(dll).second)
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

// A KCFI type's list names an imported function by its import-table entry,
// which holds the address. Where static data holds the import thunk as the
// address instead, a call through such a pointer reaches the thunk, so the list
// also names a cell holding it. Completes the linker's lists and objects' own.
void KCFIContents::listThunks() {
  llvm::TimeTraceScope timeScope("List KCFI thunks");
  Configuration &config = ctx.config;
  auto thunkIsAddress = [&](DefinedImportData *imp) {
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(imp->file->thunkSym);
    return thunk && thunk->getChunk()->live &&
           (!config.importSlots || imp->file->thunkIsAddress ||
            imp->file->isDelayLoaded());
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

// After the sections are merged and before they are laid out: defines the code
// range of a sealed image, places each thunk's mismatch routine after it, puts
// the linker's KCFI thunks in place of clang's, and points the range's bounds,
// and those a DLL exports, at their chunks.
void KCFIContents::layOut(OutputSection *textSec, OutputSection *rdataSec) {
  llvm::TimeTraceScope timeScope("Lay out KCFI");
  openChunks.insert(ctx.kcfi.chunks.begin(), ctx.kcfi.chunks.end());
  defineCodeRange();
  placeMismatchRoutines();
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

// Defines __llvm_code_start/__llvm_code_end, which clang's KCFI thunks test to
// take an in-image target directly, as the bounds of the code in the output
// section holding a sealed image's prefixes; they keep clang's empty-range weak
// default unless every prefix is in one section. The range ends before the
// first export-suppressed function, whose chunks are moved after the rest.
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
  // A bound is replaced only while still the weak alias to that default; since
  // folding may fold the default's byte onto another, it is compared with the
  // default's own symbol. Code referencing neither still has a range.
  auto *empty =
      dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(KCFICodeEmpty));
  auto isEmptyDefault = [&](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    return d && empty && d->getChunk() == empty->getChunk() &&
           d->getValue() == empty->getValue();
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
  // The types, and the vtable-slot second types, of unsealed prefixes in the
  // range, with how many carry each. A word before the marker that is a
  // membership tag (which LTO gives) is no second type.
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

// Points the bounds a DLL exports for its importers' KCFI checks at its code
// range, or, lacking one, both at one address, an empty range. That address is
// kept out of the export directory, where the loader would read it as a
// forwarded export's name.
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

// Where the guard table lists an unprefixed function, foreign code hands ours
// one of its functions through a channel no type rule sees. So a fail-fast
// mismatch (a closed type's routine, or the end of a static type's list) sends
// an in-image target to the dynamic scanner, which fails fast on our marker and
// continues into the guard function; an out-of-image target still fails fast.
// The scanners are clang's, as only clang knows the marker and prefix offset.
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

// A member thunk takes a target carrying one of its type's membership tags and
// on a miss continues into the ordinary thunk through a weak symbol, since a
// function of the type that LTO did not compile has no tag but may be valid.
// In a sealed image the guard table names every reachable function, so where no
// unsealed non-LTO prefix of the type exists, an in-image miss is no member and
// goes straight to the mismatch routine; a target in another image is caught by
// its outside miss, narrowed likewise unless it may validly target the type.
void KCFIContents::narrowMemberMisses() {
  bool isX64 = ctx.config.machine == AMD64;
  if (!prefixes.isSealed() || (!isX64 && ctx.config.machine != ARM64))
    return;
  DenseSet<uint32_t> nativeTypes;
  for (const TypePrefix &p : prefixes.get())
    if (!p.sealed && !cast<ObjFile>(p.chunk->file)->ltoOutput)
      nativeTypes.insert(
          read32le(p.chunk->getContents().data() + p.offset + p.size - 4));

  // A function in another image may validly target a type an object opens
  // (which may give it the object's mismatch routine where the linker found no
  // fact of it) or that the linker opens for one.
  auto *trap = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(KCFITrap));
  auto isOutsideType = [&](uint32_t type, Defined *mismatch) {
    if (ctx.kcfi.outsideTypes.contains(type))
      return true;
    auto *r = dyn_cast<DefinedRegular>(mismatch);
    return r && (!trap || r->getChunk() != trap->getChunk() ||
                 r->getValue() != trap->getValue());
  };

  struct Kind {
    StringRef miss, mismatch;
    bool outside;
  };
  static const Kind kinds[] = {
      {KCFIMemberMissPrefix, KCFIMismatchPrefix, false},
      {KCFIMemberCheckMissPrefix, KCFICheckMismatchPrefix, false},
      {KCFIMemberLocalMissPrefix, KCFIMismatchPrefix, false},
      {KCFIMemberLocalCheckMissPrefix, KCFICheckMismatchPrefix, false},
      {KCFIMemberOutsideMissPrefix, KCFIMismatchPrefix, true},
      {KCFIMemberCheckOutsideMissPrefix, KCFICheckMismatchPrefix, true}};
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
      auto *mismatch = dyn_cast_or_null<Defined>(
          ctx.symtab.findNonInserting((k.mismatch + hex).str()));
      if (!mismatch || (k.outside && isOutsideType(type, mismatch)))
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

// Builds clang's KCFI thunk type check (without its branch) and page test from
// the facts the thunk's object records. Returns false where the form cannot be
// built.
static bool buildKCFIThunkCheck(MachineTypes machine, const KCFIState::Thunk &t,
                                SmallVectorImpl<uint8_t> &compare,
                                SmallVectorImpl<uint8_t> &pageTest) {
  bool vfn = t.kind == LinkKCFIThunkVfnCheck;
  // A vfn thunk compares the second type and the marker's start, 16 bytes
  // before the entry; any other the marker's end and the type, 8 bytes before.
  // A target outside the code range is compared only where the prefix read (16
  // or 12 bytes plus any patchable prefix) cannot start on the page before.
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
  bool rax = isDispatchThunk(t.kind);
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

// The chunk of a KCFI thunk's mismatch routine where the routine is the type's
// own and starts its chunk: a compiled routine (its COMDAT leader) or one the
// linker opened in its place. The trap, which a closed type's routine defaults
// to, serves every such type.
const Chunk *KCFIContents::getMismatchRoutine(Symbol *mismatch) const {
  if (auto *d = dyn_cast<DefinedRegular>(mismatch)) {
    SectionChunk *sc = d->getChunk();
    return sc && sc->live && sc->sym == d && d->getValue() == 0 ? sc : nullptr;
  }
  if (auto *s = dyn_cast<DefinedSynthetic>(mismatch))
    if (openChunks.contains(s->getChunk()))
      return s->getChunk();
  return nullptr;
}

// Places each KCFI thunk's mismatch routine right after the thunk, so the
// thunk's branches to it take an 8-bit displacement and the jump that ends
// clang's thunk (which its object lists) is left out. A routine several thunks
// name follows the first; code an order file places stays put.
void KCFIContents::placeMismatchRoutines() {
  auto isOrdered = [&](const Chunk *c) {
    auto *sc = dyn_cast<SectionChunk>(c);
    return sc && sc->sym && ctx.config.order.count(sc->sym->getName());
  };
  DenseMap<const Chunk *, const Chunk *> routineOf;
  for (auto &[sym, thunk] : ctx.kcfi.thunks) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    SectionChunk *sc = d ? d->getChunk() : nullptr;
    if (!sc || !sc->live || sc->sym != d || d->getValue() != 0 || isOrdered(sc))
      continue;
    const Chunk *routine = getMismatchRoutine(thunk.mismatch);
    if (routine && !isOrdered(routine))
      routineOf.try_emplace(sc, routine);
  }
  if (routineOf.empty())
    return;
  // Output sections are not yet indexed, so a routine moves only within the
  // section that holds both it and its thunk.
  for (OutputSection *sec : ctx.outputSections) {
    std::vector<Chunk *> &chunks = sec->chunks;
    DenseSet<const Chunk *> held(chunks.begin(), chunks.end());
    DenseSet<const Chunk *> moved;
    for (Chunk *c : chunks)
      if (const Chunk *routine = routineOf.lookup(c))
        if (routine != c && held.contains(routine))
          moved.insert(routine);
    if (moved.empty())
      continue;
    std::vector<Chunk *> placed;
    placed.reserve(chunks.size());
    DenseSet<const Chunk *> done;
    for (Chunk *c : chunks) {
      if (moved.contains(c))
        continue;
      placed.push_back(c);
      if (const Chunk *routine = routineOf.lookup(c))
        if (moved.contains(routine) && done.insert(routine).second)
          placed.push_back(const_cast<Chunk *>(routine));
    }
    chunks = std::move(placed);
  }
}

// In a sealed image, replaces each KCFI thunk the image keeps and its object
// describes with a KCFIThunkChunk. The chunk tests the image's own range as one
// compare of the target's offset against its size, only for a type an unsealed
// prefix in the range has; then the ranges of imported DLLs whose records list
// one. It builds clang's type check and page test from the thunk's record.
void KCFIContents::replaceThunks() {
  bool isX64 = ctx.config.machine == AMD64;
  if (!prefixes.isSealed() || (!isX64 && ctx.config.machine != ARM64))
    return;

  DenseMap<const Chunk *, Chunk *> replacements;
  // The chunk of each replacement's mismatch routine, which may follow it.
  DenseMap<const Chunk *, const Chunk *> routines;
  SmallVector<std::pair<Symbol *, KCFIThunkChunk *>, 0> checkedMisses;
  SetVector<ObjFile *> files;
  for (auto &[sym, thunk] : ctx.kcfi.thunks) {
    // The record describes the thunk only if the chunk is the thunk's own.
    auto *d = dyn_cast<DefinedRegular>(sym);
    SectionChunk *sc = d ? d->getChunk() : nullptr;
    bool dispatch = isDispatchThunk(thunk.kind);
    bool local = isLocalThunk(thunk.kind);
    if (!sc || !sc->live || sc->sym != d || d->getValue() != 0 ||
        (dispatch && !isX64))
      continue;
    auto *mismatchSym = dyn_cast<Defined>(thunk.mismatch);
    auto *guardSym = dyn_cast_or_null<Defined>(ctx.symtab.find(
        dispatch ? "__guard_dispatch_icall_fptr" : "__guard_check_icall_fptr"));
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
    // thunk tests; every target of a local thunk is in the image.
    SmallVector<KCFIThunkChunk::Range, 4> imported;
    auto tested = ctx.kcfi.rangeHolders[vfn].find(stored);
    if (!local && tested != ctx.kcfi.rangeHolders[vfn].end())
      for (uint32_t i : tested->second)
        imported.push_back(
            {ctx.kcfi.importedRanges[i].start, ctx.kcfi.importedRanges[i].end});
    // A local thunk tests the range wherever the image has one.
    bool testsRange = local ? kcfiCodeSec != nullptr : own;
    kcfiThunksTestRange |= testsRange;
    // A member thunk's miss of an outside target that still continues into this
    // thunk (its type may be valid in another image) takes its checked entry.
    StringRef outsidePrefix = dispatch ? KCFIMemberOutsideMissPrefix
                                       : KCFIMemberCheckOutsideMissPrefix;
    auto *outsideMiss = dyn_cast_or_null<DefinedRegular>(
        vfn || local
            ? nullptr
            : ctx.symtab.find((outsidePrefix + utohexstr(thunk.type,
                                                         /*LowerCase=*/true,
                                                         /*Width=*/8))
                                  .str()));
    bool checked = outsideMiss && outsideMiss->getChunk() == sc &&
                   outsideMiss->getValue() == 0;
    auto *chunk = make<KCFIThunkChunk>(
        ctx, !dispatch, local, compare, pageTest, mismatchSym, guardSym,
        testsRange ? kcfiCodeStart : nullptr,
        testsRange ? kcfiCodeEnd : nullptr, own, imported, checked);
    replacements[sc] = chunk;
    routines[chunk] = getMismatchRoutine(thunk.mismatch);
    if (checked)
      checkedMisses.push_back({outsideMiss, chunk});
    files.insert(cast<ObjFile>(sc->file));
  }
  if (replacements.empty())
    return;

  // Each chunk takes clang's place, and every symbol at clang's thunk (its own
  // and others', e.g. a weak alias) takes its address. A following mismatch
  // routine starts where the thunk ends, needing only instruction alignment.
  unsigned insnAlign = isX64 ? 1 : 4;
  for (OutputSection *sec : ctx.outputSections) {
    std::vector<Chunk *> &chunks = sec->chunks;
    for (size_t i = 0; i != chunks.size(); ++i) {
      Chunk *r = replacements.lookup(chunks[i]);
      if (!r)
        continue;
      chunks[i] = r;
      const Chunk *routine = routines.lookup(r);
      if (routine && i + 1 != chunks.size() && chunks[i + 1] == routine &&
          routine->getAlignment() <= insnAlign)
        static_cast<KCFIThunkChunk *>(r)->setMismatchFollows();
    }
  }
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
  for (auto [miss, chunk] : checkedMisses)
    replaceSymbol<DefinedSynthetic>(miss, miss->getName(), chunk,
                                    chunk->getCheckedOffset());
  for (auto &kv : replacements)
    cast<SectionChunk>(const_cast<Chunk *>(kv.first))->live = false;
}

// The KCFI thunks the linker made compare a target's offset from the start of
// the code range with the range's size, as a sign-extended 32-bit immediate.
void KCFIContents::checkCodeRange() {
  llvm::TimeTraceScope timeScope("Check KCFI code range");
  if (kcfiThunksTestRange &&
      kcfiCodeEnd->getRVA() - kcfiCodeStart->getRVA() > INT32_MAX)
    Err(ctx) << "the code range of " << ctx.config.outputFile
             << " is too large for its KCFI thunks";
}
} // namespace lld::coff
