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
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Transforms/Utils/KCFIHash.h"
#include <map>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;

namespace lld::coff {
// Branch conditions as ARM64 encodes them, and as x86-64's jcc second byte.
enum Cond { Eq, Ne, Hs, Lo };
static const uint8_t x86Cond[] = {0x84, 0x85, 0x83, 0x82};

static bool isCheckThunk(LinkKCFIThunkKind k) {
  return k != LinkKCFIThunkDispatch && k != LinkKCFIThunkLocalDispatch;
}
static bool isLocalThunk(LinkKCFIThunkKind k) {
  return k == LinkKCFIThunkLocalDispatch || k == LinkKCFIThunkLocalCheck;
}
static bool isMemberMiss(LinkKCFIThunkKind k) {
  return k == LinkKCFIThunkMemberMiss || k == LinkKCFIThunkMemberOutsideMiss;
}

// The compiled scanner an open type's routine passes its list to: a static one
// fails fast at the list's end, a dynamic one continues into the guard function.
static StringRef getScanner(bool check, bool dynamic) {
  static const StringRef names[2][2] = {
      {KCFIOpenScanner, KCFIOpenDynamicScanner},
      {KCFICheckOpenScanner, KCFICheckOpenDynamicScanner}};
  return names[check][dynamic];
}

static bool isListSection(StringRef name) {
  return name.starts_with(KCFIListSectionPrefix) && name.ends_with("_m");
}

// A word of a KCFI type's list, or a cell an entry points to: a value, or sym's
// address (zero if sym is not in the image).
class KCFIWordChunk : public NonSectionChunk {
public:
  KCFIWordChunk(COFFLinkerContext &ctx, StringRef section, Defined *sym,
                uint64_t value = 0)
      : section(section), sym(sym), value(value), ctx(ctx) {
    setAlignment(8);
  }
  size_t getSize() const override { return 8; }
  uint32_t getOutputCharacteristics() const override {
    return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  }
  StringRef getSectionName() const override { return section; }
  void getBaserels(std::vector<Baserel> *res) override {
    if (sym && sym->isLive())
      res->emplace_back(getRVA(), ctx.config.machine);
  }
  void writeTo(uint8_t *buf) const override {
    if (!sym)
      write64le(buf, value);
    else
      write64le(buf, sym->isLive() ? sym->getRVA() + ctx.config.imageBase : 0);
  }

private:
  StringRef section;
  Defined *sym;
  uint64_t value;
  COFFLinkerContext &ctx;
};

// Code the linker writes for KCFI checks, measured and written by one emitter.
// The target is in RAX (RCX for a check) with R10 and R11 free, or on ARM64 in
// X15 with X16 and X17 free.
class KCFICodeChunk : public NonSectionCodeChunk {
public:
  KCFICodeChunk(COFFLinkerContext &ctx)
      : ctx(ctx), arm(ctx.config.machine == ARM64) {}
  size_t getSize() const override {
    if (!size)
      size = LabelledCode::layOut(nullptr, rva,
                                  [&](LabelledCode &a) { emit(a); });
    return *size;
  }
  void writeTo(uint8_t *buf) const override {
    LabelledCode::layOut(buf, rva, [&](LabelledCode &a) { emit(a); });
  }
  uint32_t getOutputCharacteristics() const override {
    return IMAGE_SCN_CNT_CODE | NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override { return ctx.config.machine; }

protected:
  virtual void emit(LabelledCode &a) const = 0;
  // A jump at any distance: the linker adds no range thunk for its own chunks.
  void jumpTo(LabelledCode &a, uint64_t target) const {
    if (arm) {
      a.address(17, target);
      a.put32(0xD61F0220); // br x17
    } else {
      a.put({0xE9, 0, 0, 0, 0});
      a.disp(target);
    }
  }

  COFFLinkerContext &ctx;
  bool arm;
  mutable std::optional<uint32_t> size;
};

// The mismatch routine of a type the linker opens: R10 (X16) is pointed at the
// list and the scanner jumped to; with outside, an out-of-image target goes
// there instead.
class KCFIOpenChunk : public KCFICodeChunk {
public:
  KCFIOpenChunk(COFFLinkerContext &ctx, Defined *list, Defined *scanner,
                bool dynamic, Defined *outside = nullptr, bool check = false)
      : KCFICodeChunk(ctx), list(list), scanner(scanner), dynamic(dynamic),
        outside(outside), check(check) {
    setAlignment(arm ? 4 : 1);
  }
  void setStatic(Defined *s) {
    scanner = s;
    dynamic = false;
    size.reset();
  }

private:
  void emit(LabelledCode &a) const override;

  Defined *list, *scanner;
  bool dynamic;
  Defined *outside;
  bool check;
};

void KCFIOpenChunk::emit(LabelledCode &a) const {
  uint32_t out = a.newLabel();
  if (outside) {
    // The image ends at its last section, rounded up to its mapping page.
    uint64_t end = 0;
    for (OutputSection *sec : ctx.outputSections)
      end = std::max<uint64_t>(end, sec->getRVA() + sec->getVirtualSize());
    for (auto [bound, cond] :
         {std::pair(uint64_t(0), Lo), std::pair(alignTo(end, 4096), Hs)}) {
      if (arm) {
        a.page(17, bound);
        a.put32(0xEB1101FF); // cmp x15, x17
        a.branch19(0x54000000 | cond, out);
      } else {
        a.put({0x4C, 0x8D, 0x1D, 0, 0, 0, 0}); // lea r11, [rip + bound]
        a.disp(bound);
        a.put({0x4C, 0x39, uint8_t(check ? 0xD9 : 0xD8)}); // cmp rax, r11
        a.jccTo(x86Cond[cond], outside->getRVA());
      }
    }
  }
  // The scanner reads the list from its first word after the head.
  if (list && arm) {
    a.address(16, list->getRVA() + 8);
  } else if (list) {
    a.put({0x4C, 0x8D, 0x15, 0, 0, 0, 0}); // lea r10, [rip + list + 8]
    a.disp(list->getRVA() + 8);
  }
  jumpTo(a, scanner->getRVA());
  if (dynamic && !arm)
    a.put({0xCC});
  if (outside && arm) {
    a.bind(out);
    jumpTo(a, outside->getRVA());
  }
}

// The linker's form of a clang KCFI thunk in a sealed image: a target in a code
// range the image vouches for (its own, or an imported DLL's listing the type)
// takes the type check and the jump, any other clang's page-test path. The
// checked entry, for a member thunk's outside miss, tests the imported ranges.
class KCFIThunkChunk : public KCFICodeChunk {
public:
  KCFIThunkChunk(COFFLinkerContext &ctx) : KCFICodeChunk(ctx) {
    setAlignment(16);
  }
  bool build(const KCFIState::Thunk &t);
  void writeTo(uint8_t *buf) const override;
  uint32_t getCheckedOffset() const {
    getSize();
    return checkedOffset;
  }

  bool check = false, local = false, checked = false, typeInRange = false;
  // Whether the mismatch routine is the code right after the thunk.
  bool mismatchFollows = false;
  Defined *mismatch = nullptr, *guard = nullptr;
  // The bounds of the image's own range, if the thunk tests it.
  Symbol *codeStart = nullptr, *codeEnd = nullptr;
  SmallVector<KCFIState::ImportedRange, 0> imported;

private:
  void emit(LabelledCode &a) const override;

  // Clang's type check (on x86-64 split after the constant's load, which range
  // tests hoist) and page test.
  SmallVector<uint8_t, 24> load, compare, pageTest;
  mutable uint32_t checkedOffset = 0;
};

// Builds clang's type check (the 8 bytes before the entry, 16 for a second
// type, against the marker and type) and page test from the thunk's record.
bool KCFIThunkChunk::build(const KCFIState::Thunk &t) {
  bool vfn = t.kind == LinkKCFIThunkVfnCheck;
  uint32_t off = t.offset + (vfn ? 16 : 8);
  uint32_t readLog2 = Log2_64_Ceil(t.offset + (vfn ? 16 : 12));
  uint32_t pageMask = readLog2 < 12 ? 0xFFF & ~((1u << readLog2) - 1) : 0;
  uint64_t pattern = getTypePrefixPattern(t.marker);
  // x86-64 stores a type that would spell an ENDBR instruction plus one.
  uint32_t type = arm ? t.type : getX86KCFIType(t.type);
  uint64_t expected =
      vfn ? type | pattern << 32 : pattern >> 32 | uint64_t(type) << 32;
  auto put = [](SmallVectorImpl<uint8_t> &v, uint64_t x, unsigned n) {
    for (unsigned i = 0; i != n; ++i)
      v.push_back(x >> (8 * i));
  };
  if (arm) {
    // ldur x16, [x15, #-off]; mov x17, #expected; cmp x16, x17;
    // tst x15, #mask
    if (off > 256 || !pageMask)
      return false;
    put(compare, 0xF84001F0 | (-off & 0x1FF) << 12, 4);
    for (uint32_t hw = 0; hw != 4; ++hw)
      put(compare,
          (hw ? 0xF2800011 : 0xD2800011) | hw << 21 |
              (expected >> (16 * hw) & 0xFFFF) << 5,
          4);
    put(compare, 0xEB11021F, 4);
    put(pageTest,
        0xF24001FF | ((64 - readLog2) & 63) << 16 | (11 - readLog2) << 10, 4);
    return true;
  }
  // movabs r11, expected; cmp [reg - off], r11; test reg32, mask
  load = {0x49, 0xBB};
  put(load, expected, 8);
  if (off <= 128) {
    compare = {0x4C, 0x39, uint8_t(0x58 | check), uint8_t(-off)};
  } else {
    compare = {0x4C, 0x39, uint8_t(0x98 | check)};
    put(compare, -off, 4);
  }
  if (check)
    pageTest = {0xF7, 0xC1};
  else
    pageTest = {0xA9};
  put(pageTest, pageMask, 4);
  return true;
}

void KCFIThunkChunk::writeTo(uint8_t *buf) const {
  KCFICodeChunk::writeTo(buf);
  if (mismatchFollows && mismatch->getRVA() != rva + getSize())
    Err(ctx) << "the KCFI mismatch routine " << mismatch->getName()
             << " does not follow its thunk";
}

void KCFIThunkChunk::emit(LabelledCode &a) const {
  uint32_t toMismatch = a.newLabel();
  auto jcc = [&](Cond c, uint32_t label) {
    if (arm)
      a.branch19(0x54000000 | c, label);
    else
      a.jcc(x86Cond[c], label);
  };
  auto toRoutine = [&](Cond c) {
    if (arm || mismatchFollows)
      jcc(c, toMismatch);
    else
      a.jccTo(x86Cond[c], mismatch->getRVA());
  };
  auto typeCheck = [&] {
    a.put(compare);
    toRoutine(Ne);
  };
  auto take = [&] {
    if (arm)
      a.put32(0xD65F03C0); // ret
    else if (check)
      a.put({0xC3}); // ret
    else
      a.put({0xFF, 0xE0}); // jmp rax
  };
  // In range: end - 1 - target (x86-64) or target - start below the size.
  auto testRange = [&](Cond c, uint32_t label, bool toMismatchRoutine) {
    uint64_t start = cast<Defined>(codeStart)->getRVA();
    uint64_t end = cast<Defined>(codeEnd)->getRVA();
    uint64_t extent = end - start;
    if (arm) {
      a.address(16, start);
      a.put32(0xCB1001F0);                          // sub x16, x15, x16
      a.put32(0xD2800011 | (extent & 0xFFFF) << 5); // mov x17, #extent
      a.put32(0xF2A00011 | (extent >> 16) << 5);    // movk x17, lsl #16
      a.put32(0xEB11021F);                          // cmp x16, x17
    } else {
      a.put({0x4C, 0x8D, 0x15, 0, 0, 0, 0}); // lea r10, [rip + end - 1]
      a.disp(end - 1);
      a.put({0x49, 0x29, uint8_t(0xC2 | check << 3)}); // sub r10, rax
      a.put({0x49, 0x81, 0xFA, 0, 0, 0, 0});           // cmp r10, extent
      a.imm32(extent);
    }
    if (toMismatchRoutine)
      toRoutine(c);
    else
      jcc(c, label);
  };
  auto testImported = [&](const KCFIState::ImportedRange &r, uint32_t below,
                          Cond c, uint32_t label) {
    if (arm) {
      a.address(16, r.start->getRVA());
      a.put32(0xA9404610); // ldp x16, x17, [x16]
      a.put32(0xEB1001FF); // cmp x15, x16
      jcc(Lo, below);
      a.put32(0xEB1101FF); // cmp x15, x17
      jcc(c, label);
      return;
    }
    a.put({0x48, 0x3B, uint8_t(0x05 | check << 3), 0, 0, 0, 0}); // cmp rax, []
    a.disp(r.start->getRVA());
    jcc(Lo, below);
    a.put({0x48, 0x3B, uint8_t(0x05 | check << 3), 0, 0, 0, 0});
    a.disp(r.end->getRVA());
    jcc(c, label);
  };
  auto guardJump = [&] {
    if (arm) {
      a.loadImport(16, guard->getRVA());
      a.put32(0xD61F0200); // br x16
    } else {
      a.put({0xFF, 0x25, 0, 0, 0, 0}); // jmp [rip + guard]
      a.disp(guard->getRVA());
    }
  };
  auto outsidePath = [&](bool loaded) {
    a.put(pageTest);
    toRoutine(Eq);
    if (!loaded)
      a.put(load);
    typeCheck();
    guardJump();
  };

  if (local && !codeStart) {
    outsidePath(false);
  } else if (local) {
    uint32_t fail = a.newLabel();
    if (typeInRange) {
      testRange(Hs, fail, false);
      a.put(load);
      typeCheck();
      take();
    } else {
      testRange(Lo, 0, true);
    }
    // Fail fast with FAST_FAIL_GUARD_ICALL_CHECK_FAILURE_XFG.
    a.bind(fail);
    if (arm) {
      a.put32(0x52800800); // mov w0, #64
      a.put32(0xD43E0060); // brk #0xf003
    } else {
      a.put({0xB9, 0x40, 0, 0, 0, 0xCD, 0x29}); // mov ecx, 64; int 0x29
    }
  } else {
    bool own = codeStart, hits = own || !imported.empty();
    if (hits)
      a.put(load);
    uint32_t hit = a.newLabel(), outside = a.newLabel();
    if (own) {
      uint32_t imports = a.newLabel();
      testRange(Hs, imports, false);
      a.bind(hit);
      typeCheck();
      take();
      a.bind(imports);
    }
    for (size_t i = 0; i != imported.size(); ++i) {
      if (!own && i + 1 == imported.size()) {
        testImported(imported[i], outside, Hs, outside);
        a.bind(hit);
        typeCheck();
        take();
        break;
      }
      uint32_t next = a.newLabel();
      testImported(imported[i], next, Lo, hit);
      a.bind(next);
    }
    a.bind(outside);
    outsidePath(hits);
    if (checked) {
      checkedOffset = a.off;
      uint32_t taken = a.newLabel();
      for (const KCFIState::ImportedRange &r : imported) {
        uint32_t next = a.newLabel();
        testImported(r, next, Lo, taken);
        a.bind(next);
      }
      guardJump();
      a.bind(taken);
      take();
    }
  }
  a.bind(toMismatch);
  if (arm && !mismatchFollows)
    jumpTo(a, mismatch->getRVA());
}

// Opens the KCFI types that code without a prefix, which the link brings in,
// reaches. Clang's facts are weak absolutes valued at a check identifier:
// __kcfi_typeid_<f> (f's address is taken: static, f listed), and
// __kcfi_{inflow,param,tinflow}_<key>_<x> (a pointer of the type returns
// through g, reaches g, or returns from a call through type x: dynamic) and
// __kcfi_popen_<key>, where key is a precise type in 16 hex digits or n and a
// node, whose types __kcfi_node_<node>_<key> give.
void openKCFITypes(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  bool isX64 = ctx.config.machine == AMD64;
  if (!isX64 && ctx.config.machine != ARM64)
    return;
  llvm::TimeTraceScope timeScope("Open KCFI types");
  // Read before a destructor forwarder's replacement can move a destructor's
  // symbol out of its object.
  DenseSet<std::pair<SectionChunk *, uint32_t>> prefixed;
  for (ObjFile *file : ctx.objFileInstances)
    for (const TypePrefixLabel &l : getTypePrefixes(file))
      if (l.entry)
        prefixed.insert({l.label->getChunk(), l.entry});
  if (!symtab.find(KCFIOpenDynamicScanner) &&
      !symtab.find(KCFICheckOpenDynamicScanner))
    return;
  auto isForeignFile = [](ObjFile *file) { return !hasTypePrefixes(file); };
  // A DLL's variable stays undefined until automatic import resolves it.
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
  auto isImport = [](Defined *d) {
    return isa_and_nonnull<DefinedImportThunk, DefinedImportData>(d);
  };
  // A function is foreign where it has no prefix; a variable, which never has
  // one, where the object defining it is foreign.
  auto isForeign = [&](Symbol *s) {
    Defined *d = resolve(s);
    if (isImport(d))
      return true;
    auto *c = dyn_cast_or_null<DefinedCOFF>(d);
    auto *file = c ? dyn_cast_or_null<ObjFile>(c->getFile()) : nullptr;
    if (!file)
      return false;
    bool foreign = isForeignFile(file);
    if (auto *r = dyn_cast<DefinedRegular>(c); r && r->getChunk() &&
        (r->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
      return !prefixed.contains({r->getChunk(), r->getValue()});
    return foreign;
  };

  // Nodes are read first, so a fact naming one takes its types at once.
  using Type = std::pair<uint32_t, uint64_t>;
  DenseMap<Symbol *, SmallVector<Type, 1>> inflows, params;
  DenseMap<uint64_t, SmallVector<Type, 1>> tinflows, nodes;
  SmallVector<Type, 0> popens;
  std::vector<Symbol *> facts, typeids;
  symtab.forEachSymbol([&](Symbol *s) {
    if (!s->isLazy() && s->getName().starts_with("__kcfi_"))
      facts.push_back(s);
  });
  auto addParam = [&](Symbol *g, Type t) {
    params[g].push_back(t);
    // Code declaring g dllimport reaches it through __imp_g, a local import.
    auto *imp = dyn_cast_or_null<Undefined>(
        symtab.find(("__imp_" + g->getName()).str()));
    if (imp && !imp->getWeakAlias())
      params[imp].push_back(t);
  };
  for (bool nodePass : {true, false}) {
    for (Symbol *s : facts) {
      auto [kind, rest] = s->getName().drop_front(strlen("__kcfi_")).split('_');
      if ((kind == "node") != nodePass)
        continue;
      if (kind == "typeid") {
        typeids.push_back(s);
        continue;
      }
      bool node = rest.consume_front("n");
      uint64_t key, other;
      StringRef tail = rest.drop_front(std::min<size_t>(rest.size(), 16));
      if (rest.size() < 16 || rest.take_front(16).getAsInteger(16, key) ||
          (!tail.empty() && !tail.consume_front("_")))
        continue;
      auto *abs = dyn_cast_or_null<DefinedAbsolute>(s->getDefined());
      Type type(abs ? abs->getVA() : 0, key);
      auto forEachType = [&](auto f) {
        if (!node)
          return f(type);
        if (auto it = nodes.find(key); it != nodes.end())
          for (Type t : it->second)
            f(t);
      };
      if (kind == "node" && !node && !tail.getAsInteger(16, other)) {
        nodes[key].push_back({type.first, other});
      } else if (kind == "popen" && !node && tail.empty()) {
        popens.push_back(type);
      } else if (kind == "tinflow" && !tail.getAsInteger(16, other)) {
        forEachType([&](Type t) { tinflows[other].push_back(t); });
      } else if ((kind == "inflow" || kind == "param") && !tail.empty()) {
        bool inflow = kind == "inflow";
        if (Symbol *g = symtab.find(tail))
          forEachType([&](Type t) {
            inflow ? inflows[g].push_back(t) : addParam(g, t);
          });
      }
    }
  }

  // Per type: dynamic, opened only by in-image foreign code (local), and list
  // entries, each flagged if a cell holds it. A precise key is followed through
  // its tinflow facts, again when a non-local fact reaches it.
  struct Opening {
    bool dynamic = false, local = false;
    SmallVector<std::pair<Defined *, bool>, 0> entries;
  };
  std::map<uint32_t, Opening> openings;
  DenseMap<uint64_t, bool> reached;
  SmallVector<uint64_t, 0> worklist;
  auto reach = [&](Type t, bool local) {
    Opening &o = openings[t.first];
    o.local = (!o.dynamic || o.local) && local;
    o.dynamic = true;
    auto [it, inserted] = reached.try_emplace(t.second, local);
    if (inserted || (it->second && !local)) {
      it->second = local;
      worklist.push_back(t.second);
    }
  };
  for (auto &[g, types] : inflows)
    if (isForeign(g))
      for (Type t : types)
        reach(t, !isImport(resolve(g)));
  // An object with code and no prefix is foreign.
  if (!params.empty())
    for (ObjFile *file : ctx.objFileInstances)
      for (Symbol *s : file->getSymbols()) {
        auto it = params.find(s);
        if (it == params.end())
          continue;
        auto *r = dyn_cast_or_null<DefinedRegular>(s->getDefined());
        if (r && r->file == file)
          continue;
        if (!isForeignFile(file))
          break;
        for (Type t : it->second)
          reach(t, true);
      }
  for (Type t : popens)
    reach({t.first, t.second}, false);
  while (!worklist.empty()) {
    uint64_t key = worklist.pop_back_val();
    bool local = reached[key];
    if (auto it = tinflows.find(key); it != tinflows.end())
      for (Type t : it->second)
        reach(t, local);
  }
  // An import is listed by its IAT entry, an unprefixed definition by a cell.
  llvm::sort(typeids,
             [](Symbol *a, Symbol *b) { return a->getName() < b->getName(); });
  for (Symbol *s : typeids) {
    auto *id = dyn_cast_or_null<DefinedAbsolute>(s->getDefined());
    Symbol *f = symtab.find(s->getName().drop_front(strlen("__kcfi_typeid_")));
    if (!id || !isForeign(f))
      continue;
    auto &entries = openings[uint32_t(id->getVA())].entries;
    Defined *d = resolve(f);
    if (auto *thunk = dyn_cast<DefinedImportThunk>(d))
      entries.push_back({thunk->wrappedSym, false});
    else
      entries.push_back({d, !isa<DefinedImportData>(d)});
  }

  // A type opens where its thunks name the trap default or a compiled routine,
  // whose type may then have targets in another image.
  DenseMap<uint64_t, Symbol *> mismatches;
  for (auto &[sym, t] : ctx.kcfi.thunks)
    if (!isMemberMiss(t.kind))
      mismatches.try_emplace(uint64_t(t.type) << 1 | isCheckThunk(t.kind),
                             t.mismatch);
  auto getCompiledScanner = [&](Symbol *m, bool check) -> Symbol * {
    if (auto *r = dyn_cast_or_null<DefinedRegular>(m))
      for (bool dynamic : {false, true})
        if (Symbol *s = symtab.find(getScanner(check, dynamic));
            s && is_contained(r->getChunk()->symbols(), s))
          return s;
    return nullptr;
  };
  auto keep = [&](Defined *d) {
    if (!d->isGCRoot) {
      d->isGCRoot = true;
      ctx.config.gcroot.push_back(d);
    }
  };
  SmallVector<uint32_t, 0> localTypes;
  for (auto &[type, o] : openings) {
    std::string hex = utohexstr(type, /*LowerCase=*/true, /*Width=*/8);
    auto *head = dyn_cast_or_null<Defined>(symtab.find(KCFIListPrefix + hex));
    auto getHead = [&, type = type] {
      if (!head) {
        auto *c = make<KCFIWordChunk>(
            ctx, saver().save(KCFIListSectionPrefix + hex + "_a"), nullptr,
            type);
        ctx.kcfi.chunks.push_back(c);
        ctx.kcfi.chunks.push_back(make<KCFIWordChunk>(
            ctx, saver().save(KCFIListSectionPrefix + hex + "_z"), nullptr,
            uint64_t(type) << 1 | 1));
        head = cast<Defined>(
            symtab.addSynthetic(saver().save(KCFIListPrefix + hex), c));
      }
      return head;
    };
    bool opened = false;
    for (bool check : {false, true}) {
      Symbol *m = check || isX64 ? mismatches.lookup(uint64_t(type) << 1 | check) : nullptr;
      bool replace;
      if (auto *u = dyn_cast_or_null<Undefined>(m)) {
        Defined *d = u->getDefinedWeakAlias();
        if (!d || d->getName() != KCFITrap)
          continue;
        replace = true;
      } else if (Symbol *scanner = getCompiledScanner(m, check)) {
        ctx.kcfi.outsideTypes.insert(type);
        replace = o.dynamic && scanner->getName() == getScanner(check, false);
      } else {
        continue;
      }
      opened = true;
      if (!replace)
        continue;
      StringRef name = getScanner(check, o.dynamic);
      auto *scanner = dyn_cast_or_null<Defined>(symtab.find(name));
      if (!scanner) {
        Err(ctx) << "cannot open KCFI type " << hex << ": " << name
                 << " is not defined";
        continue;
      }
      getHead();
      keep(scanner);
      keep(head);
      auto *routine = make<KCFIOpenChunk>(ctx, head, scanner, o.dynamic);
      ctx.kcfi.chunks.push_back(routine);
      replaceSymbol<DefinedSynthetic>(m, m->getName(), routine);
      if (o.local)
        if (auto *s =
                dyn_cast_or_null<Defined>(symtab.find(getScanner(check, false))))
          ctx.kcfi.localRoutines.push_back({routine, s});
    }
    if (!opened)
      continue;
    bool imported = llvm::any_of(
        o.entries, [](auto &e) { return isa<DefinedImportData>(e.first); });
    if ((imported || o.dynamic) && ctx.kcfi.outsideTypes.insert(type).second &&
        !imported && o.local)
      localTypes.push_back(type);
    if (o.entries.empty())
      continue;
    getHead();
    StringRef section = saver().save(KCFIListSectionPrefix + hex + "_m");
    for (auto [target, viaCell] : o.entries) {
      Defined *entry = target;
      if (viaCell) {
        auto *cell = make<KCFIWordChunk>(ctx, ".rdata", target);
        ctx.kcfi.chunks.push_back(cell);
        entry = make<DefinedSynthetic>(target->getName(), cell);
      } else if (auto *imp = dyn_cast<DefinedImportData>(target)) {
        ctx.kcfi.listedImports.push_back({imp, section});
      }
      ctx.kcfi.chunks.push_back(make<KCFIWordChunk>(ctx, section, entry));
    }
  }
  // The scanners bound() may route to are kept before GC, with their unwind
  // data, wherever a foreign object may get an unprefixed function into the
  // guard table: through its guard metadata, its address-taken import table,
  // or (without guard metadata or with address-taking sites) any relocation.
  // A reference through __imp_X reaches X.
  if (!(ctx.config.guardCF & GuardCFLevel::CF))
    return;
  auto isCode = [](DefinedRegular *r) {
    return r && r->getChunk() &&
           (r->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE);
  };
  auto isPrefixedEntry = [&](Symbol *s) {
    auto *r = dyn_cast_or_null<DefinedRegular>(s);
    return isCode(r) && !isForeign(r);
  };
  auto isUnprefixedCode = [&](Symbol *s) {
    if (auto *u = dyn_cast_or_null<Undefined>(s);
        u && u->getName().starts_with("__imp_"))
      s = symtab.find(u->getName().drop_front(strlen("__imp_")));
    if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s))
      s = li->getTarget();
    auto *r = dyn_cast_or_null<DefinedRegular>(s);
    return isa_and_nonnull<DefinedImportThunk>(s) ||
           (isCode(r) && !isPrefixedEntry(r));
  };
  auto mayListUnprefixed = [&](ObjFile *file) {
    auto anyIndexed = [&](ArrayRef<SectionChunk *> chunks, auto pred) {
      for (SectionChunk *c : chunks) {
        ArrayRef<uint8_t> data = c->getContents();
        for (size_t i = 0; i + 4 <= data.size(); i += 4)
          if (uint32_t index = read32le(data.data() + i);
              index < file->getSymbols().size() &&
              pred(file->getSymbols()[index]))
            return true;
      }
      return false;
    };
    if (file->hasGuardCF()) {
      if (anyIndexed(file->getGuardFidChunks(),
                     [&](Symbol *s) {
                       return isa_and_nonnull<Defined>(s) &&
                              !isPrefixedEntry(s);
                     }) ||
          anyIndexed(file->getGuardIATChunks(), [&](Symbol *s) {
            return !isa_and_nonnull<DefinedImportData>(s) &&
                   isUnprefixedCode(s);
          }))
        return true;
      if (!file->describesSites())
        return false;
    }
    for (Chunk *c : file->getChunks())
      if (auto *sc = dyn_cast_or_null<SectionChunk>(c))
        for (const coff_relocation &rel : sc->getRelocs())
          if (isUnprefixedCode(file->getSymbol(rel.SymbolTableIndex)))
            return true;
    return false;
  };
  SmallVector<ObjFile *, 0> foreign;
  bool mayList = false;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!isForeignFile(file))
      continue;
    foreign.push_back(file);
    mayList = mayList || mayListUnprefixed(file);
  }
  if (!mayList)
    return;
  for (bool check : {false, true})
    if (check || isX64)
      if (auto *s = dyn_cast_or_null<Defined>(symtab.find(getScanner(check, true))))
        keep(s);
  // Foreign code that references an import can pass on pointers from any DLL;
  // import-free foreign code only in-image functions, which the bound accepts,
  // so a type it alone opened need not be dynamic.
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
  for (uint32_t type : localTypes)
    ctx.kcfi.outsideTypes.erase(type);
}

// A DLL exports its code range's bounds as private data, empty without a range
// so that an importer linked against a release with one still loads.
void addKCFIRangeExports(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Add KCFI range exports");
  if (!ctx.config.dll || !ctx.typePrefixRecords || ctx.hybridSymtab ||
      (ctx.config.machine != AMD64 && ctx.config.machine != ARM64))
    return;
  static const StringRef names[] = {KCFICodeStart, KCFICodeEnd};
  ctx.symtab.reserveExportNames(
      [](StringRef name) { return is_contained(names, name); },
      "the KCFI code range");
  for (int i : {0, 1}) {
    ctx.kcfi.rangeExports[i] = make<DefinedSynthetic>(names[i], nullptr);
    Export e;
    e.name = names[i];
    e.sym = ctx.kcfi.rangeExports[i];
    e.data = true;
    e.isPrivate = true;
    e.source = ExportSource::Export;
    e.qualifiedName = names[i];
    ctx.symtab.exports.push_back(e);
  }
}

// The range record is a member defining one absolute named for the DLL.
void addKCFIRangeRecord(COFFLinkerContext &ctx, StringRef dllName,
                        std::vector<NewArchiveMember> &members,
                        std::vector<uint8_t> &buffer) {
  if (!ctx.kcfi.rangeDefined || !ctx.kcfi.rangeExports[0])
    return;
  std::string group;
  raw_string_ostream g(group);
  for (const MapVector<uint32_t, uint32_t> &types : ctx.kcfi.rangeTypes) {
    SmallVector<std::pair<uint32_t, uint32_t>, 0> sorted(types.begin(),
                                                         types.end());
    llvm::sort(sorted);
    encodeULEB128(sorted.size(), g);
    for (auto [type, functions] : sorted) {
      support::endian::write<uint32_t>(g, type, llvm::endianness::little);
      encodeULEB128(functions, g);
    }
  }
  members.push_back(
      createLinkRecordMember(ctx, dllName,
                             "__llvm_code_range$" + dllName.lower(),
                             {{LinkRecordImageCode, group}}, buffer));
}

static bool readRangeRecord(COFFLinkerContext &ctx, MemoryBufferRef mb,
                            DenseMap<uint32_t, uint32_t> (&types)[2]) {
  bool ok = false;
  forEachLinkRecordGroup(
      mb, ctx.config.machine, [&](uint64_t kind, ArrayRef<uint8_t> payload) {
        if (kind != LinkRecordImageCode)
          return;
        DataExtractor group(payload, /*IsLittleEndian=*/true);
        DataExtractor::Cursor c(0);
        for (DenseMap<uint32_t, uint32_t> &map : types)
          for (uint64_t n = group.getULEB128(c), i = 0; c && i != n; ++i) {
            uint32_t type = group.getU32(c);
            map[type] = group.getULEB128(c);
          }
        ok = c && group.eof(c);
        consumeError(c.takeError());
      });
  return ok;
}

// A sealed image imports the range bounds of each statically imported DLL
// whose record lists a type its thunks check, into the read-only IAT.
void bindKCFIImportedRanges(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Bind KCFI imported ranges");
  bool isX64 = ctx.config.machine == AMD64;
  if (!ctx.typePrefixRecords || !(ctx.config.guardCF & GuardCFLevel::CF) ||
      ctx.hybridSymtab || (!isX64 && ctx.config.machine != ARM64))
    return;
  DenseSet<uint32_t> checked[2];
  for (auto &[sym, t] : ctx.kcfi.thunks)
    if (auto *d = dyn_cast<DefinedRegular>(sym);
        d && d->getChunk() && d->getChunk()->live && !isMemberMiss(t.kind) &&
        !isLocalThunk(t.kind) && (isX64 || t.kind != LinkKCFIThunkDispatch))
      checked[t.kind == LinkKCFIThunkVfnCheck].insert(
          isX64 ? getX86KCFIType(t.type) : t.type);
  if (checked[0].empty() && checked[1].empty())
    return;

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
        readRangeRecord(ctx, lazy->getMemberBuffer(), r.types))
      records.push_back(std::move(r));
  }

  // A thunk tests at most four ranges, those with more functions of its type
  // first; only those are bound, in load order.
  for (int vfn : {0, 1})
    for (uint32_t type : checked[vfn]) {
      SmallVector<std::pair<uint32_t, uint32_t>, 4> holders;
      for (auto [i, r] : llvm::enumerate(records))
        if (uint32_t n = r.types[vfn].lookup(type))
          holders.push_back({n, i});
      llvm::stable_sort(holders,
                        [](auto &a, auto &b) { return a.first > b.first; });
      for (auto [n, i] : ArrayRef(holders).take_front(4)) {
        records[i].bound = true;
        ctx.kcfi.rangeHolders[vfn][type].push_back(i);
      }
    }
  SmallVector<uint32_t, 0> rangeOf(records.size());
  for (auto [i, r] : llvm::enumerate(records)) {
    if (!r.bound)
      continue;
    rangeOf[i] = ctx.kcfi.importedRanges.size();
    DefinedImportData *start = ctx.symtab.loadImportMember(r.bounds[0]);
    ctx.kcfi.importedRanges.push_back(
        {start, ctx.symtab.loadImportMember(r.bounds[1])});
  }
  for (auto &holders : ctx.kcfi.rangeHolders)
    for (auto &[type, tested] : holders)
      for (uint32_t &i : tested)
        i = rangeOf[i];
}

bool isKCFIListEntry(SectionChunk *sc, const coff_relocation &rel) {
  return isListSection(sc->getSectionName()) && sc->isAddressWord(rel);
}

// Where static data holds an import's thunk as its address, the type's list
// also names a cell holding the thunk.
void KCFIContents::listThunks() {
  if (!ctx.kcfi.listsImports && ctx.kcfi.listedImports.empty())
    return;
  llvm::TimeTraceScope timeScope("List KCFI thunks");
  auto thunkIsAddress = [&](DefinedImportData *imp) {
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(imp->file->thunkSym);
    return thunk && thunk->getChunk()->live &&
           (!ctx.config.importSlots || imp->file->thunkIsAddress ||
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
      if (!sc || !sc->live || !isListSection(sc->getSectionName()))
        continue;
      for (const coff_relocation &rel : sc->getRelocs())
        if (auto *imp = dyn_cast_or_null<DefinedImportData>(
                file->getSymbol(rel.SymbolTableIndex));
            imp && !imp->isRuntimePseudoReloc && thunkIsAddress(imp))
          listed.insert({imp->file, sc->getSectionName()});
    }
  }
  for (auto [file, section] : listed) {
    auto *thunk = cast<DefinedImportThunk>(file->thunkSym);
    auto *cell = make<KCFIWordChunk>(ctx, ".rdata", thunk);
    ctx.kcfi.chunks.push_back(cell);
    ctx.kcfi.chunks.push_back(make<KCFIWordChunk>(
        ctx, section, make<DefinedSynthetic>(thunk->getName(), cell)));
  }
}

// Before layout: defines the code range, replaces clang's thunks, and points
// the bounds, and those a DLL exports, at the range. Without a range a DLL
// exports one address outside the export directory (where the loader would
// read a forwarder): the first code, or a word of its own.
void KCFIContents::layOut(OutputSection *textSec, OutputSection *rdataSec) {
  llvm::TimeTraceScope timeScope("Lay out KCFI");
  defineCodeRange();
  replaceThunks();
  auto [start, end] = ctx.kcfi.rangeExports;
  // A DLL whose export table comes from its objects describes no range.
  if (start && isa<SectionChunk>(ctx.symtab.edataStart))
    ctx.kcfi.rangeDefined = false;
  if (!ctx.kcfi.rangeDefined)
    for (Export &e : ctx.symtab.exports)
      if (e.sym == start || e.sym == end)
        e.qualifiedName = "";
  Chunk *first = nullptr, *after = nullptr;
  uint64_t afterOffset = 0;
  if (codeSec) {
    first = codeSec->chunks.front();
    after = codeEndChunk ? codeEndChunk : codeSec->chunks.back();
    afterOffset = codeEndChunk ? 0 : after->getSize();
  } else if (start) {
    auto it = llvm::find_if(textSec->chunks,
                            [](const Chunk *c) { return c->getSize(); });
    if (it == textSec->chunks.end()) {
      first = make<KCFIWordChunk>(ctx, ".rdata", nullptr);
      rdataSec->addChunk(first);
    } else {
      first = *it;
    }
    after = first;
  }
  for (auto [s, e] : {std::pair<Symbol *, Symbol *>(codeStart, codeEnd),
                      std::pair<Symbol *, Symbol *>(start, end)})
    if (s && first) {
      replaceSymbol<DefinedSynthetic>(s, s->getName(), first);
      replaceSymbol<DefinedSynthetic>(e, e->getName(), after, afterOffset);
    }
}

// The code range of a sealed image: the output section holding every prefix,
// up to the export-suppressed functions, which are moved after the rest.
void KCFIContents::defineCodeRange() {
  if (!prefixes.isSealed() || prefixes.get().empty())
    return;
  DenseSet<const Chunk *> prefixed;
  for (const TypePrefix &p : prefixes.get())
    prefixed.insert(p.chunk);
  auto secIt = llvm::find_if(ctx.outputSections, [&](OutputSection *s) {
    return llvm::count_if(s->chunks, [&](Chunk *c) {
             return prefixed.contains(c);
           }) == ptrdiff_t(prefixed.size());
  });
  if (secIt == ctx.outputSections.end())
    return;
  // A bound is defined only while it is clang's empty default (or lazy).
  auto *empty = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(KCFICodeEmpty));
  auto isUserDefined = [&](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    return s && !s->isLazy() &&
           !(d && empty && d->getChunk() == empty->getChunk() &&
             d->getValue() == empty->getValue());
  };
  Symbol *start = ctx.symtab.find(KCFICodeStart);
  Symbol *end = ctx.symtab.find(KCFICodeEnd);
  if (isUserDefined(start) || isUserDefined(end))
    return;
  if (!start)
    start = make<DefinedSynthetic>(KCFICodeStart, nullptr);
  if (!end)
    end = make<DefinedSynthetic>(KCFICodeEnd, nullptr);

  std::vector<Chunk *> &chunks = (*secIt)->chunks;
  auto isPlainText = [](const Chunk *c) {
    auto *sc = dyn_cast<SectionChunk>(c);
    return sc && sc->getSectionName() == ".text";
  };
  auto group = llvm::find_if(chunks, isPlainText);
  std::stable_partition(
      group, std::find_if_not(group, chunks.end(), isPlainText),
      [&](const Chunk *c) {
        auto *sc = cast<SectionChunk>(c);
        return !prefixes.isSuppressed(sc) ||
               (sc->sym && ctx.config.order.count(sc->sym->getName()));
      });
  auto limit = llvm::find_if(
      chunks, [&](const Chunk *c) { return prefixes.isSuppressed(c); });
  if (limit == chunks.begin())
    return;
  // The census of unsealed types and second types (not membership tags).
  DenseSet<const Chunk *> inRange(chunks.begin(), limit);
  DenseSet<std::pair<const Chunk *, uint32_t>> tags;
  for (Symbol *s : ctx.kcfi.memberTags)
    if (auto *d = dyn_cast<DefinedRegular>(s))
      tags.insert({d->getChunk(), d->getValue()});
  for (const TypePrefix &p : prefixes.get()) {
    if (p.sealed || !inRange.contains(p.chunk))
      continue;
    const uint8_t *words = p.chunk->getContents().data() + p.offset;
    ++ctx.kcfi.rangeTypes[0][read32le(words + p.size - 4)];
    if (p.size == 16 && !tags.contains({p.chunk, p.offset}))
      ++ctx.kcfi.rangeTypes[1][read32le(words)];
  }
  codeEndChunk = limit == chunks.end() ? nullptr : *limit;
  codeStart = start;
  codeEnd = end;
  codeSec = *secIt;
  ctx.kcfi.rangeDefined = true;
}

// Places each thunk's mismatch routine after it (8-bit branches, and clang's
// ending jump left out), except for ordered code, and in a sealed image puts a
// KCFIThunkChunk in each thunk's place.
void KCFIContents::replaceThunks() {
  bool isX64 = ctx.config.machine == AMD64;
  bool sealed = prefixes.isSealed() && (isX64 || ctx.config.machine == ARM64);
  DenseSet<const Chunk *> opened(ctx.kcfi.chunks.begin(),
                                 ctx.kcfi.chunks.end());
  auto getRoutine = [&](Symbol *m) -> Chunk * {
    if (auto *d = dyn_cast<DefinedRegular>(m)) {
      SectionChunk *sc = d->getChunk();
      return sc && sc->live && sc->sym == d && d->getValue() == 0 ? sc
                                                                  : nullptr;
    }
    auto *s = dyn_cast<DefinedSynthetic>(m);
    return s && opened.contains(s->getChunk()) ? s->getChunk() : nullptr;
  };
  auto isOrdered = [&](const Chunk *c) {
    auto *sc = dyn_cast<SectionChunk>(c);
    return sc && sc->sym && ctx.config.order.count(sc->sym->getName());
  };
  DenseMap<const Chunk *, SmallVector<Symbol *, 1>> outsideMisses;
  for (auto &[sym, t] : ctx.kcfi.thunks)
    if (auto *d = dyn_cast<DefinedRegular>(sym);
        d && t.kind == LinkKCFIThunkMemberOutsideMiss && d->getValue() == 0)
      outsideMisses[d->getChunk()].push_back(sym);

  DenseMap<const Chunk *, const Chunk *> routineOf;
  DenseMap<const Chunk *, KCFIThunkChunk *> replacements;
  SetVector<ObjFile *> files;
  for (auto &[sym, t] : ctx.kcfi.thunks) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    SectionChunk *sc = d ? d->getChunk() : nullptr;
    if (isMemberMiss(t.kind) || !sc || !sc->live || sc->sym != d ||
        d->getValue() != 0)
      continue;
    if (Chunk *routine = getRoutine(t.mismatch);
        routine && !isOrdered(sc) && !isOrdered(routine))
      routineOf.try_emplace(sc, routine);
    bool dispatch = !isCheckThunk(t.kind);
    if (!sealed || (dispatch && !isX64))
      continue;
    auto *c = make<KCFIThunkChunk>(ctx);
    c->mismatch = dyn_cast<Defined>(t.mismatch);
    c->guard = dyn_cast_or_null<Defined>(ctx.symtab.find(
        dispatch ? "__guard_dispatch_icall_fptr" : "__guard_check_icall_fptr"));
    c->check = !dispatch;
    if (!c->mismatch || !c->guard || !c->build(t))
      continue;
    bool vfn = t.kind == LinkKCFIThunkVfnCheck;
    uint32_t stored = isX64 ? getX86KCFIType(t.type) : t.type;
    c->local = isLocalThunk(t.kind);
    c->typeInRange = codeSec && ctx.kcfi.rangeTypes[vfn].count(stored);
    if (c->local ? codeSec != nullptr : c->typeInRange) {
      c->codeStart = codeStart;
      c->codeEnd = codeEnd;
      thunksTestRange = true;
    }
    if (auto it = ctx.kcfi.rangeHolders[vfn].find(stored);
        !c->local && it != ctx.kcfi.rangeHolders[vfn].end())
      for (uint32_t i : it->second)
        c->imported.push_back(ctx.kcfi.importedRanges[i]);
    c->checked = !vfn && !c->local && outsideMisses.count(sc);
    replacements[sc] = c;
    files.insert(cast<ObjFile>(sc->file));
  }

  unsigned insnAlign = isX64 ? 1 : 4;
  for (OutputSection *sec : ctx.outputSections) {
    std::vector<Chunk *> &chunks = sec->chunks;
    DenseSet<const Chunk *> moved, done;
    if (!routineOf.empty()) {
      DenseSet<const Chunk *> held(chunks.begin(), chunks.end());
      for (Chunk *c : chunks)
        if (const Chunk *r = routineOf.lookup(c);
            r && r != c && held.contains(r))
          moved.insert(r);
    }
    if (!moved.empty()) {
      std::vector<Chunk *> placed;
      placed.reserve(chunks.size());
      for (Chunk *c : chunks) {
        if (moved.contains(c))
          continue;
        placed.push_back(c);
        if (const Chunk *r = routineOf.lookup(c);
            moved.contains(r) && done.insert(r).second)
          placed.push_back(const_cast<Chunk *>(r));
      }
      chunks = std::move(placed);
    }
    for (size_t i = 0; i != chunks.size(); ++i)
      if (KCFIThunkChunk *r = replacements.lookup(chunks[i])) {
        chunks[i] = r;
        r->mismatchFollows = i + 1 != chunks.size() &&
                             chunks[i + 1] == getRoutine(r->mismatch) &&
                             chunks[i + 1]->getAlignment() <= insnAlign;
      }
  }
  if (replacements.empty())
    return;
  if (KCFIThunkChunk *r = replacements.lookup(codeEndChunk))
    codeEndChunk = r;
  // Every symbol at clang's thunk, local ones included, moves to the chunk.
  auto repoint = [&](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (d && d->getValue() == 0)
      if (KCFIThunkChunk *r = replacements.lookup(d->getChunk()))
        replaceSymbol<DefinedSynthetic>(s, s->getName(), r);
  };
  ctx.symtab.forEachSymbol(repoint);
  for (ObjFile *file : files)
    for (Symbol *s : file->getSymbols())
      if (auto *d = dyn_cast_or_null<DefinedRegular>(s);
          d && !d->getCOFFSymbol().isExternal())
        repoint(d);
  for (auto &[sc, misses] : outsideMisses)
    if (KCFIThunkChunk *r = replacements.lookup(sc); r && r->checked)
      for (Symbol *s : misses)
        replaceSymbol<DefinedSynthetic>(s, s->getName(), r,
                                        r->getCheckedOffset());
  for (auto &kv : replacements)
    cast<SectionChunk>(const_cast<Chunk *>(kv.first))->live = false;
}

// Once the guard table is known. Where it lists an unprefixed function foreign
// code lists, a fail-fast mismatch sends an in-image target to the dynamic
// scanner (marker, then guard function), and a type only import-free foreign
// code opened becomes static. Then, in a sealed image, a member miss goes to
// the mismatch routine where no native prefix of the type exists, an outside
// one only where no other image may hold a valid target.
void KCFIContents::bound(OutputSection *textSec, OutputSection *rdataSec) {
  bool isX64 = ctx.config.machine == AMD64;
  if (!isX64 && ctx.config.machine != ARM64)
    return;
  auto *trap = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(KCFITrap));
  auto isTrap = [&](Symbol *s) {
    auto *r = dyn_cast<DefinedRegular>(s);
    return r && trap && r->getChunk() == trap->getChunk() &&
           r->getValue() == trap->getValue();
  };
  Defined *empty = nullptr;
  for (bool check : {false, true}) {
    if (!prefixes.hasUnprefixedTargets() || (!check && !isX64))
      continue;
    auto *dynamic = dyn_cast_or_null<DefinedRegular>(
        ctx.symtab.find(getScanner(check, true)));
    if (!dynamic || !dynamic->getChunk()->live)
      continue;
    for (auto [routine, staticScanner] : ctx.kcfi.localRoutines)
      if (auto *s = dyn_cast<DefinedRegular>(staticScanner);
          s && s->getName() == getScanner(check, false) &&
          s->getChunk()->live)
        routine->setStatic(s);
    KCFIOpenChunk *closed = nullptr;
    for (auto &[sym, t] : ctx.kcfi.thunks) {
      if (isMemberMiss(t.kind) || isCheckThunk(t.kind) != check ||
          !isTrap(t.mismatch) || !trap->isLive())
        continue;
      // A closed type's list is empty: its head, then the odd word ending it.
      if (!empty) {
        auto *head = make<KCFIWordChunk>(ctx, ".rdata", nullptr);
        rdataSec->addChunk(head);
        rdataSec->addChunk(make<KCFIWordChunk>(ctx, ".rdata", nullptr, 1));
        empty = make<DefinedSynthetic>("__llvm_kcfi_list_empty", head);
      }
      if (!closed) {
        closed = make<KCFIOpenChunk>(ctx, empty, dynamic, false, trap, check);
        textSec->addChunk(closed);
      }
      replaceSymbol<DefinedSynthetic>(t.mismatch, t.mismatch->getName(),
                                      closed);
    }
    // Routines reach the static scanner through its symbol.
    auto *scanner = dyn_cast_or_null<DefinedRegular>(
        ctx.symtab.find(getScanner(check, false)));
    if (scanner && scanner->isLive()) {
      auto *outside = make<DefinedSynthetic>(
          scanner->getName(), scanner->getChunk(), scanner->getValue());
      auto *open =
          make<KCFIOpenChunk>(ctx, nullptr, dynamic, false, outside, check);
      textSec->addChunk(open);
      replaceSymbol<DefinedSynthetic>(scanner, scanner->getName(), open);
    }
  }

  if (!prefixes.isSealed())
    return;
  DenseSet<uint32_t> nativeTypes;
  for (const TypePrefix &p : prefixes.get())
    if (!p.sealed && !cast<ObjFile>(p.chunk->file)->ltoOutput)
      nativeTypes.insert(
          read32le(p.chunk->getContents().data() + p.offset + p.size - 4));
  for (auto &[sym, t] : ctx.kcfi.thunks) {
    auto *d = dyn_cast<Defined>(sym);
    auto *m = dyn_cast<Defined>(t.mismatch);
    if (!isMemberMiss(t.kind) || !d || !d->isLive() || !m ||
        nativeTypes.contains(isX64 ? getX86KCFIType(t.type) : t.type))
      continue;
    if (t.kind == LinkKCFIThunkMemberOutsideMiss &&
        (ctx.kcfi.outsideTypes.contains(t.type) ||
         (isa<DefinedRegular>(m) && !isTrap(m))))
      continue;
    if (auto *r = dyn_cast<DefinedRegular>(m))
      replaceSymbol<DefinedSynthetic>(d, d->getName(), r->getChunk(),
                                      r->getValue());
    else if (auto *s = dyn_cast<DefinedSynthetic>(m))
      replaceSymbol<DefinedSynthetic>(d, d->getName(), s->getChunk());
  }
}

// The linker's thunks compare against the range size as a 32-bit immediate.
void KCFIContents::checkCodeRange() {
  llvm::TimeTraceScope timeScope("Check KCFI code range");
  if (thunksTestRange && cast<Defined>(codeEnd)->getRVA() -
                             cast<Defined>(codeStart)->getRVA() >
                         INT32_MAX)
    Err(ctx) << "the code range of " << ctx.config.outputFile
             << " is too large for its KCFI thunks";
}
} // namespace lld::coff
