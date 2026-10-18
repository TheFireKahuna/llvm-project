//===- KCFI.cpp -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The linker's part of KCFI in an image whose objects give their functions
// marked type prefixes: opening the types that code without a prefix reaches,
// the image's code range and its use by importers, and the linker's form of
// the compiler's thunks. The compiler describes its thunks, member misses and
// mismatch routines in link records, and the facts that open types as symbols.
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
// The conditions of the branches KCFI code takes, as ARM64 encodes them; x86Cond
// gives each as the second opcode byte of x86-64's jcc.
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

// The scanners an open type's routine passes the type's list to, by whether
// the routine serves check thunks (all ARM64 has) and whether the opening is
// dynamic: a static scanner fails fast where a dynamic one continues into the
// guard function.
static StringRef getScanner(bool check, bool dynamic) {
  static const StringRef names[2][2] = {
      {KCFIOpenScanner, KCFIOpenDynamicScanner},
      {KCFICheckOpenScanner, KCFICheckOpenDynamicScanner}};
  return names[check][dynamic];
}

static bool isListSection(StringRef name) {
  return name.starts_with(KCFIListSectionPrefix) && name.ends_with("_m");
}

// A word of a KCFI type's list, in the section that sorts it among the
// compiler's pieces, or a cell an entry points to: a value, or the address of
// sym (zero if sym is not in the image).
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

// Code the linker writes for KCFI checks, which one emitter both measures and
// writes. The target is in RAX on x86-64, or RCX for a check, with R10 and R11
// free; on ARM64 it is in X15, with X16 and X17 free.
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
  // A jump to target at any distance; on ARM64 through X17, since the linker
  // adds no range extension thunk for a chunk of its own.
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

// The routine a KCFI type check reaches on a mismatch where the linker opens
// the type: it points R10 (X16) at the type's list and jumps to the compiled
// scanner. With outside, it first sends a target outside the image there.
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

// The linker's form of one of clang's KCFI thunks, replacing it in a sealed
// image. A target inside a code range the image vouches for (its own, and each
// imported DLL's whose record lists a function of the type) takes clang's type
// check and the jump; any other takes clang's page-test path. Every target of
// a local thunk is in the image. The checked entry, for a member thunk's miss
// outside the code range, tests only the imported ranges.
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

  // Clang's type check (on x86-64 split after its load of the constant,
  // which a thunk testing ranges hoists) and page test.
  SmallVector<uint8_t, 24> load, compare, pageTest;
  mutable uint32_t checkedOffset = 0;
};

// Builds clang's type check and page test from the thunk's record: the
// compare of the 8 bytes before the entry (16 for a second type) with the
// marker and the type, and the test that a target outside the code range lies
// far enough into its page for the prefix read. False if they cannot be built.
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
  // The mismatch routine is reached through a label where it follows the
  // thunk, and on ARM64 otherwise through a jump at the thunk's end.
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
  // On x86-64 the target is in the range where end - 1 - target is below its
  // size, and on ARM64 where target - start is.
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
  // Branches to below for a target below r's start, then on c to label.
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

// Opens a KCFI type where the compiler could not see that unprefixed code
// reaches it, because the link brings that code in. Clang gives the facts as
// weak absolute symbols whose value is a check identifier, each opening types
// statically or dynamically by how the symbol it names resolves:
//
// - __kcfi_typeid_<f>: f's address is taken (static, f listed).
// - __kcfi_inflow_<key>_<g>: a pointer of the type can return through g.
// - __kcfi_param_<key>_<g>: g can receive such a pointer.
// - __kcfi_tinflow_<key>_<called>: a call through the called type returns one
//   of the type, opening it transitively.
// - __kcfi_popen_<key>: the object opens the type dynamically.
//
// key is a precise type in 16 hex digits, or n and a node of a record's types,
// which each __kcfi_node_<node>_<key> gives. A type opens only where a KCFI
// thunk names its mismatch routine; the routines opened are kept past GC.
void openKCFITypes(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  bool isX64 = ctx.config.machine == AMD64;
  if ((!isX64 && ctx.config.machine != ARM64) ||
      (!symtab.find(KCFIOpenDynamicScanner) &&
       !symtab.find(KCFICheckOpenDynamicScanner)))
    return;
  llvm::TimeTraceScope timeScope("Open KCFI types");

  // Whether each object is foreign (no marked prefix), and the entries of the
  // marked prefixes, each the first to follow its static __cfi_ label.
  DenseSet<std::pair<SectionChunk *, uint32_t>> prefixed;
  DenseMap<ObjFile *, bool> foreignFiles;
  auto isForeignFile = [&](ObjFile *file) {
    auto [it, inserted] = foreignFiles.try_emplace(file);
    if (inserted) {
      for (const TypePrefixLabel &l : getTypePrefixes(file))
        if (l.entry)
          prefixed.insert({l.label->getChunk(), l.entry});
      it->second = !hasTypePrefixes(file);
    }
    return it->second;
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
  auto isImport = [](Defined *d) {
    return isa_and_nonnull<DefinedImportThunk, DefinedImportData>(d);
  };
  auto isCode = [](DefinedRegular *r) {
    return r && r->getChunk() &&
           (r->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE);
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
    if (auto *r = dyn_cast<DefinedRegular>(c); isCode(r))
      return !prefixed.contains({r->getChunk(), r->getValue()});
    return foreign;
  };

  // The facts, read with the nodes first, so that a fact naming a node takes
  // its types at once. A lazy fact belongs to an archive member not linked.
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
    // Code that declares g dllimport reaches our g through __imp_g, which
    // becomes a local import unless it is bound to something else.
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

  // Per type: whether it opens dynamically, whether only through in-image
  // foreign code, and the symbols to add to its list, each flagged if its entry
  // is a cell holding it. A precise key that reaches a type is followed through
  // its tinflow facts, again if a non-local fact arrives later; a type is local
  // only where every key that reached it is.
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
  // An object's own dynamic openings are never local: a cast or an import may
  // carry an address from any image.
  for (Type t : popens)
    reach({t.first, t.second}, false);
  while (!worklist.empty()) {
    uint64_t key = worklist.pop_back_val();
    bool local = reached[key];
    if (auto it = tinflows.find(key); it != tinflows.end())
      for (Type t : it->second)
        reach(t, local);
  }
  // An import is listed by its import-table entry (the writer adds a cell for
  // its thunk where static data holds one); an unprefixed definition by a cell
  // holding its address.
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

  // A type's mismatch routines, for dispatch and for check thunks, are those
  // its thunks name: the weak default (the trap), or a compiled routine that
  // jumps to a scanner, whose type then may have targets in another image.
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
    // A function in another image may validly target a type listed for an
    // import or open dynamically, unless only in-image foreign code that
    // references no import opened it.
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

  // Where the guard table may list an unprefixed function that foreign code
  // lists, a fail-fast mismatch is sent to the dynamic scanner, and a routine
  // opened only by import-free foreign code to the static one (see bound()).
  // The table is known only after GC, so the scanners are kept wherever a
  // foreign object may list such a function: an entry its guard metadata
  // lists, a function its address-taken import table reaches, or, lacking
  // guard metadata or describing address-taking sites, one any relocation
  // reaches. A reference through __imp_X, which may become a linker-made
  // pointer, reaches X.
  if (!(ctx.config.guardCF & GuardCFLevel::CF))
    return;
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

// The import library of a DLL with a code range holds the range record in a
// member defining one absolute symbol named for the DLL, which an importer's
// linker finds without loading the member.
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

// Reads a DLL's KCFI code-range record from the import-library member holding
// it, returning false if it holds none this linker can read.
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

// In a sealed image with a guard table, the KCFI checks may take a target
// directly inside the code range of a statically imported DLL whose import
// library records the range. The image imports the range's bounds from each
// such DLL whose unsealed functions there have a type one of its thunks
// checks, as data the loader binds into the (read-only) import address table.
void bindKCFIImportedRanges(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Bind KCFI imported ranges");
  bool isX64 = ctx.config.machine == AMD64;
  if (!ctx.typePrefixRecords || !(ctx.config.guardCF & GuardCFLevel::CF) ||
      ctx.hybridSymtab || (!isX64 && ctx.config.machine != ARM64))
    return;
  // The types, and second types, that the image's thunks check, as prefixes
  // store them. The writer replaces dispatch thunks on x86-64 only.
  DenseSet<uint32_t> checked[2];
  for (auto &[sym, t] : ctx.kcfi.thunks)
    if (auto *d = dyn_cast<DefinedRegular>(sym);
        d && d->getChunk() && d->getChunk()->live && !isMemberMiss(t.kind) &&
        !isLocalThunk(t.kind) && (isX64 || t.kind != LinkKCFIThunkDispatch))
      checked[t.kind == LinkKCFIThunkVfnCheck].insert(
          isX64 ? getX86KCFIType(t.type) : t.type);
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
        readRangeRecord(ctx, lazy->getMemberBuffer(), r.types))
      records.push_back(std::move(r));
  }

  // A thunk tests the ranges of the DLLs with the most functions of its type,
  // at most four, those with more first; only those are bound, in load order.
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

// A KCFI type's list names an imported function by its import-table entry,
// which holds the address. Where static data holds the import thunk as the
// address instead, a call through such a pointer reaches the thunk, so the list
// also names a cell holding it. Completes the linker's lists and objects' own.
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

// After the sections are merged and before they are laid out: defines the code
// range of a sealed image, puts the linker's thunks in place of clang's, then
// points the range's bounds, and those a DLL exports, at the range's first
// chunk and at the first after it or its section's end. A DLL without a range
// exports both bounds at one address outside the export directory, where the
// loader would read a forwarded export's name: the first code in .text, or in
// a DLL without code a word of its own.
void KCFIContents::layOut(OutputSection *textSec, OutputSection *rdataSec) {
  llvm::TimeTraceScope timeScope("Lay out KCFI");
  defineCodeRange();
  replaceThunks();
  auto [start, end] = ctx.kcfi.rangeExports;
  // A DLL whose export table comes from its objects exports neither, so its
  // import library describes no range, and an importer imports no bound.
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

// Defines __llvm_code_start/__llvm_code_end, which clang's KCFI thunks test to
// take an in-image target directly, as the bounds of the code in the output
// section holding a sealed image's prefixes; they keep clang's empty weak
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
  auto secIt = llvm::find_if(ctx.outputSections, [&](OutputSection *s) {
    return llvm::count_if(s->chunks, [&](Chunk *c) {
             return prefixed.contains(c);
           }) == ptrdiff_t(prefixed.size());
  });
  if (secIt == ctx.outputSections.end())
    return;
  // A bound is replaced only while it is still the weak alias to that default,
  // which folding may have folded onto another byte; a bound that no loaded
  // object references can be lazy, which defines nothing.
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
  // The types, and the vtable-slot second types, of unsealed prefixes in the
  // range, with how many carry each. A word before the marker that is a
  // membership tag (which LTO gives) is no second type.
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

// Places each KCFI thunk's mismatch routine right after the thunk, so that the
// thunk's branches to it take an 8-bit displacement and the jump that ends
// clang's thunk (which its object lists) is left out; a routine several thunks
// name follows the first, and code an order file places stays put. In a sealed
// image, then replaces each thunk the image keeps with a KCFIThunkChunk at its
// place. It tests the image's own range, as one compare of the target's offset
// against its size, for a type an unsealed prefix in the range has; then the
// ranges of the imported DLLs whose records list one.
void KCFIContents::replaceThunks() {
  bool isX64 = ctx.config.machine == AMD64;
  bool sealed = prefixes.isSealed() && (isX64 || ctx.config.machine == ARM64);
  // A mismatch routine that is the type's own and starts its chunk: a compiled
  // routine, or one the linker opened.
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
  // A thunk that a member thunk's outside miss continues into gets an entry
  // for a target whose type is checked.
  DenseMap<const Chunk *, SmallVector<Symbol *, 1>> outsideMisses;
  for (auto &[sym, t] : ctx.kcfi.thunks)
    if (auto *d = dyn_cast<DefinedRegular>(sym);
        d && t.kind == LinkKCFIThunkMemberOutsideMiss && d->getValue() == 0)
      outsideMisses[d->getChunk()].push_back(sym);

  DenseMap<const Chunk *, const Chunk *> routineOf;
  DenseMap<const Chunk *, KCFIThunkChunk *> replacements;
  SetVector<ObjFile *> files;
  for (auto &[sym, t] : ctx.kcfi.thunks) {
    // The record describes the thunk only if the chunk is the thunk's own.
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
    // x86-64 stores a type that would spell an ENDBR instruction plus one.
    bool vfn = t.kind == LinkKCFIThunkVfnCheck;
    uint32_t stored = isX64 ? getX86KCFIType(t.type) : t.type;
    c->local = isLocalThunk(t.kind);
    c->typeInRange = codeSec && ctx.kcfi.rangeTypes[vfn].count(stored);
    // A local thunk tests the range wherever the image has one, and all its
    // targets are in the image.
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

  // Output sections are not yet indexed, so a routine moves only within the
  // section that holds both it and its thunk. A replacement takes clang's
  // place, and a routine that follows it needs only instruction alignment.
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
  // Every symbol at clang's thunk takes the replacement's address: the thunk's
  // own, others' such as a weak alias, and those local to its object, which are
  // not in the symbol table.
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

// Once the guard function table is known. Where it lists an unprefixed
// function, foreign code hands ours one of its functions through a channel no
// type rule sees, so a fail-fast mismatch (a closed type's routine, or the end
// of a static type's list) sends an in-image target to the dynamic scanner,
// which fails fast on our marker and continues into the guard function; an
// out-of-image target still fails fast. The scanners are clang's, as only
// clang knows the marker and the prefix offset.
//
// Then, in a sealed image: a member thunk takes a target carrying one of its
// type's tags, and on a miss continues into the type's thunk, since a function
// of the type that LTO did not compile has no tag but may be valid. Where no
// unsealed non-LTO prefix of the type exists, an in-image miss is no member
// and goes straight to the mismatch routine, and so does a miss outside the
// code range unless a function in another image may validly target the type:
// one an object opens (its routine is then not the trap) or the linker opened.
void KCFIContents::bound(OutputSection *textSec, OutputSection *rdataSec) {
  bool isX64 = ctx.config.machine == AMD64;
  if (!isX64 && ctx.config.machine != ARM64)
    return;
  bool unprefixed = prefixes.hasUnprefixedTargets();
  if (!unprefixed && !prefixes.isSealed())
    return;
  auto *trap = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(KCFITrap));
  auto isTrap = [&](Symbol *s) {
    auto *r = dyn_cast<DefinedRegular>(s);
    return r && trap && r->getChunk() == trap->getChunk() &&
           r->getValue() == trap->getValue();
  };
  Defined *empty = nullptr;
  for (bool check : {false, true}) {
    if (!unprefixed || (!check && !isX64))
      continue;
    auto *dynamic = dyn_cast_or_null<DefinedRegular>(
        ctx.symtab.find(getScanner(check, true)));
    if (!dynamic || !dynamic->getChunk()->live)
      continue;
    // openKCFITypes kept the scanners for these routines.
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
      // The list is empty: its head, then the odd word that ends it.
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
    // Every routine that jumps to the static scanner does so through its
    // symbol.
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

// The KCFI thunks the linker made compare a target's offset from the start of
// the code range with the range's size, as a sign-extended 32-bit immediate.
void KCFIContents::checkCodeRange() {
  llvm::TimeTraceScope timeScope("Check KCFI code range");
  if (thunksTestRange && cast<Defined>(codeEnd)->getRVA() -
                             cast<Defined>(codeStart)->getRVA() >
                         INT32_MAX)
    Err(ctx) << "the code range of " << ctx.config.outputFile
             << " is too large for its KCFI thunks";
}
} // namespace lld::coff
