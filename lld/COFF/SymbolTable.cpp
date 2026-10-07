//===- SymbolTable.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "COFFLinkerContext.h"
#include "Config.h"
#include "Driver.h"
#include "LTO.h"
#include "PDB.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Strings.h"
#include "lld/Common/Timer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Mangler.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/COFFModuleDefinition.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <utility>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support;

namespace lld::coff {

StringRef ltrim1(StringRef s, const char *chars) {
  if (!s.empty() && strchr(chars, s[0]))
    return s.substr(1);
  return s;
}

static COFFSyncStream errorOrWarn(COFFLinkerContext &ctx) {
  return {ctx, ctx.config.forceUnresolved ? DiagLevel::Warn : DiagLevel::Err};
}

// Causes the file associated with a lazy symbol to be linked in.
static void forceLazy(Symbol *s) {
  s->pendingArchiveLoad = true;
  switch (s->kind()) {
  case Symbol::Kind::LazyArchiveKind: {
    auto *l = cast<LazyArchive>(s);
    l->file->addMember(l->sym);
    break;
  }
  case Symbol::Kind::LazyObjectKind: {
    InputFile *file = cast<LazyObject>(s)->file;
    // FIXME: Remove this once we resolve all defineds before all undefineds in
    //        ObjFile::initializeSymbols().
    if (!file->lazy)
      return;
    file->lazy = false;
    file->symtab.ctx.driver.addFile(file);
    break;
  }
  case Symbol::Kind::LazyDLLSymbolKind: {
    auto *l = cast<LazyDLLSymbol>(s);
    l->file->makeImport(l->sym);
    break;
  }
  default:
    llvm_unreachable(
        "symbol passed to forceLazy is not a LazyArchive or LazyObject");
  }
}

// Returns the symbol in SC whose value is <= Addr that is closest to Addr.
// This is generally the global variable or function whose definition contains
// Addr.
static Symbol *getSymbol(SectionChunk *sc, uint32_t addr) {
  DefinedRegular *candidate = nullptr;

  for (Symbol *s : sc->file->getSymbols()) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (!d || !d->data || d->file != sc->file || d->getChunk() != sc ||
        d->getValue() > addr ||
        (candidate && d->getValue() < candidate->getValue()))
      continue;

    candidate = d;
  }

  return candidate;
}

static std::vector<std::string> getSymbolLocations(BitcodeFile *file) {
  std::string res("\n>>> referenced by ");
  StringRef source = file->obj->getSourceFileName();
  if (!source.empty())
    res += source.str() + "\n>>>               ";
  res += toString(file);
  return {res};
}

static std::optional<std::pair<StringRef, uint32_t>>
getFileLineDwarf(const SectionChunk *c, uint32_t addr) {
  std::optional<DILineInfo> optionalLineInfo =
      c->file->getDILineInfo(addr, c->getSectionNumber() - 1);
  if (!optionalLineInfo)
    return std::nullopt;
  const DILineInfo &lineInfo = *optionalLineInfo;
  if (lineInfo.FileName == DILineInfo::BadString)
    return std::nullopt;
  return std::make_pair(saver().save(lineInfo.FileName), lineInfo.Line);
}

static std::optional<std::pair<StringRef, uint32_t>>
getFileLine(const SectionChunk *c, uint32_t addr) {
  // MinGW can optionally use codeview, even if the default is dwarf.
  std::optional<std::pair<StringRef, uint32_t>> fileLine =
      getFileLineCodeView(c, addr);
  // If codeview didn't yield any result, check dwarf in MinGW mode.
  if (!fileLine && c->file->symtab.ctx.config.mingw)
    fileLine = getFileLineDwarf(c, addr);
  return fileLine;
}

// Given a file and the index of a symbol in that file, returns a description
// of all references to that symbol from that file. If no debug information is
// available, returns just the name of the file, else one string per actual
// reference as described in the debug info.
// Returns up to maxStrings string descriptions, along with the total number of
// locations found.
static std::pair<std::vector<std::string>, size_t>
getSymbolLocations(ObjFile *file, uint32_t symIndex, size_t maxStrings) {
  struct Location {
    Symbol *sym;
    std::pair<StringRef, uint32_t> fileLine;
  };
  std::vector<Location> locations;
  size_t numLocations = 0;

  for (Chunk *c : file->getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(c);
    if (!sc)
      continue;
    for (const coff_relocation &r : sc->getRelocs()) {
      if (r.SymbolTableIndex != symIndex)
        continue;
      numLocations++;
      if (locations.size() >= maxStrings)
        continue;

      std::optional<std::pair<StringRef, uint32_t>> fileLine =
          getFileLine(sc, r.VirtualAddress);
      Symbol *sym = getSymbol(sc, r.VirtualAddress);
      if (fileLine)
        locations.push_back({sym, *fileLine});
      else if (sym)
        locations.push_back({sym, {"", 0}});
    }
  }

  if (maxStrings == 0)
    return std::make_pair(std::vector<std::string>(), numLocations);

  if (numLocations == 0)
    return std::make_pair(
        std::vector<std::string>{"\n>>> referenced by " + toString(file)}, 1);

  std::vector<std::string> symbolLocations(locations.size());
  size_t i = 0;
  for (Location loc : locations) {
    llvm::raw_string_ostream os(symbolLocations[i++]);
    os << "\n>>> referenced by ";
    if (!loc.fileLine.first.empty())
      os << loc.fileLine.first << ":" << loc.fileLine.second
         << "\n>>>               ";
    os << toString(file);
    if (loc.sym)
      os << ":(" << toString(file->symtab.ctx, *loc.sym) << ')';
  }
  return std::make_pair(symbolLocations, numLocations);
}

std::vector<std::string> getSymbolLocations(ObjFile *file, uint32_t symIndex) {
  return getSymbolLocations(file, symIndex, SIZE_MAX).first;
}

static std::pair<std::vector<std::string>, size_t>
getSymbolLocations(InputFile *file, uint32_t symIndex, size_t maxStrings) {
  if (auto *o = dyn_cast<ObjFile>(file))
    return getSymbolLocations(o, symIndex, maxStrings);
  if (auto *b = dyn_cast<BitcodeFile>(file)) {
    std::vector<std::string> symbolLocations = getSymbolLocations(b);
    size_t numLocations = symbolLocations.size();
    if (symbolLocations.size() > maxStrings)
      symbolLocations.resize(maxStrings);
    return std::make_pair(symbolLocations, numLocations);
  }
  llvm_unreachable("unsupported file type passed to getSymbolLocations");
  return std::make_pair(std::vector<std::string>(), (size_t)0);
}

// For an undefined symbol, stores all files referencing it and the index of
// the undefined symbol in each file.
struct UndefinedDiag {
  Symbol *sym;
  struct File {
    InputFile *file;
    uint32_t symIndex;
  };
  std::vector<File> files;
};

void SymbolTable::reportUndefinedSymbol(const UndefinedDiag &undefDiag) {
  auto diag = errorOrWarn(ctx);
  diag << "undefined symbol: " << printSymbol(undefDiag.sym);

  const size_t maxUndefReferences = 3;
  size_t numDisplayedRefs = 0, numRefs = 0;
  for (const UndefinedDiag::File &ref : undefDiag.files) {
    auto [symbolLocations, totalLocations] = getSymbolLocations(
        ref.file, ref.symIndex, maxUndefReferences - numDisplayedRefs);

    numRefs += totalLocations;
    numDisplayedRefs += symbolLocations.size();
    for (const std::string &s : symbolLocations)
      diag << s;
  }
  if (numDisplayedRefs < numRefs)
    diag << "\n>>> referenced " << numRefs - numDisplayedRefs << " more times";

  // Hints
  StringRef name = undefDiag.sym->getName();
  if (name.consume_front("__imp_")) {
    Symbol *imp = find(name);
    if (imp && imp->isLazy()) {
      diag << "\nNOTE: a relevant symbol '" << imp->getName()
           << "' is available in " << toString(imp->getFile())
           << " but cannot be used because it is not an import library.";
    }
  }
}

void SymbolTable::loadMinGWSymbols() {
  std::vector<Symbol *> undefs;
  for (auto &i : symMap) {
    Symbol *sym = i.second;
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef)
      continue;
    if (undef->getWeakAlias())
      continue;
    undefs.push_back(sym);
  }

  for (auto sym : undefs) {
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef)
      continue;
    if (undef->getWeakAlias())
      continue;
    StringRef name = undef->getName();

    if (machine == I386 && ctx.config.stdcallFixup) {
      // Check if we can resolve an undefined decorated symbol by finding
      // the intended target as an undecorated symbol (only with a leading
      // underscore).
      StringRef origName = name;
      StringRef baseName = name;
      // Trim down stdcall/fastcall/vectorcall symbols to the base name.
      baseName = ltrim1(baseName, "_@");
      baseName = baseName.substr(0, baseName.find('@'));
      // Add a leading underscore, as it would be in cdecl form.
      std::string newName = ("_" + baseName).str();
      Symbol *l;
      if (newName != origName && (l = find(newName)) != nullptr) {
        // If we found a symbol and it is lazy; load it.
        if (l->isLazy() && !l->pendingArchiveLoad) {
          Log(ctx) << "Loading lazy " << l->getName() << " from "
                   << l->getFile()->getName() << " for stdcall fixup";
          forceLazy(l);
        }
        // If it's lazy or already defined, hook it up as weak alias.
        if (l->isLazy() || isa<Defined>(l)) {
          if (ctx.config.warnStdcallFixup)
            Warn(ctx) << "Resolving " << origName << " by linking to "
                      << newName;
          else
            Log(ctx) << "Resolving " << origName << " by linking to "
                     << newName;
          undef->setWeakAlias(l);
          continue;
        }
      }
    }

    if (ctx.config.autoImport || ctx.config.importSlots) {
      if (name.starts_with("__imp_"))
        continue;
      // If we have an undefined symbol, but we have a lazy symbol we could
      // load, load it.
      Symbol *l = find(("__imp_" + name).str());
      if (!l || l->pendingArchiveLoad || !l->isLazy())
        continue;

      Log(ctx) << "Loading lazy " << l->getName() << " from "
               << l->getFile()->getName() << " for automatic import";
      forceLazy(l);
    }
  }
}

// The kinds of name that an image built by clang for Windows Itanium or
// NT-POSIX defines for the addresses inside an object that static data in
// another image can hold: X$apK for the address point K bytes into vtable X,
// and X$soK for the subobject K bytes into variable X.
static constexpr StringLiteral interiorKinds[] = {"$so", "$ap"};

// The name of the object into which the interior name Name points, or an empty
// string if Name is not one.
static StringRef getInteriorBase(StringRef name) {
  size_t i = name.rfind('$');
  if (i == StringRef::npos)
    return {};
  StringRef offset = name.substr(i + 1);
  if ((!offset.consume_front("so") && !offset.consume_front("ap")) ||
      offset.empty() || !llvm::all_of(offset, isDigit))
    return {};
  return name.take_front(i);
}

DefinedImportData *SymbolTable::findImport(StringRef name, bool &loaded) {
  loaded = false;
  Symbol *s = find(name);
  if (!s)
    return nullptr;
  if (s->isLazy() && !s->pendingArchiveLoad) {
    forceLazy(s);
    ctx.driver.run();
    loaded = true;
  }
  return dyn_cast<DefinedImportData>(s);
}

DefinedImportData *SymbolTable::findInteriorImport(DefinedImportData *imp,
                                                   int64_t offset,
                                                   bool &loaded) {
  loaded = false;
  if (offset <= 0)
    return nullptr;
  for (StringRef kind : interiorKinds) {
    DefinedImportData *interior =
        findImport((imp->getName() + kind + Twine(offset)).str(), loaded);
    // Only the image that exports the object names addresses inside it.
    if (interior &&
        interior->getDLLName().equals_insensitive(imp->getDLLName()))
      return interior;
  }
  return nullptr;
}

void SymbolTable::exportInteriorNames() {
  // The compiler exports the names along with a definition that it exports;
  // only an export from elsewhere needs them added.
  if (llvm::all_of(exports, [](const Export &e) {
        return e.source == ExportSource::Directives;
      }))
    return;
  DenseMap<StringRef, size_t> byName;
  for (size_t i = 0, e = exports.size(); i != e; ++i)
    if (exports[i].forwardTo.empty())
      byName.try_emplace(exports[i].name, i);
  std::vector<Export> added;
  for (auto &entry : symMap) {
    Symbol *sym = entry.second;
    StringRef name = sym->getName();
    StringRef base = getInteriorBase(name);
    if (base.empty() || !isa<DefinedRegular>(sym) || byName.contains(name))
      continue;
    auto it = byName.find(base);
    if (it == byName.end())
      continue;
    const Export &b = exports[it->second];
    StringRef suffix = name.drop_front(base.size());
    Export e;
    e.name = name;
    if (!b.extName.empty())
      e.extName = saver().save(b.extName + suffix);
    if (!b.exportAs.empty())
      e.exportAs = saver().save(b.exportAs + suffix);
    e.noname = b.noname;
    e.isPrivate = b.isPrivate;
    e.data = true;
    e.source = b.source;
    e.symbolName = name;
    e.sym = addGCRoot(name);
    added.push_back(e);
  }
  llvm::append_range(exports, added);
}

bool SymbolTable::loadLocalImportMembers() {
  std::vector<Symbol *> lazies;
  bool referenced = false;
  llvm::erase_if(impUndefs, [&](Symbol *sym) {
    auto *u = dyn_cast<Undefined>(sym);
    if (!u)
      return true;
    // An archive offering __imp_X itself, such as an import library, takes
    // precedence; its member was requested when the reference was added.
    if (u->pendingArchiveLoad || u->getWeakAlias())
      return false;
    StringRef name = sym->getName().substr(strlen("__imp_"));
    Symbol *l = find(name);
    if (l && l->isLazy() && !l->pendingArchiveLoad)
      lazies.push_back(l);
    // /alternatename defines X only when something references X, so
    // __imp_X references it, as a direct reference would; when the alternate
    // is an import, __imp_X is that import's pointer.
    if (!l) {
      auto it = alternateNames.find(name);
      if (it == alternateNames.end())
        return false;
      Symbol *impTo = find(("__imp_" + it->second).str());
      if (impTo && !isa<Undefined>(impTo)) {
        impTo->isUsedInRegularObj = true;
        if (impTo->isLazy())
          forceLazy(impTo);
        u->setWeakAlias(impTo);
        referenced = true;
      } else if (Symbol *to = find(it->second); to && !isa<Undefined>(to)) {
        addUndefined(name);
        referenced = true;
      }
    }
    return false;
  });
  // A weak reference loads the import an import library offers for it, which
  // defines X for a function and __imp_X in every case. A weak reference
  // loads no other member, so an archive's definition does not satisfy it.
  for (Symbol *sym : weakRefs) {
    if (!isa<Undefined>(sym) || sym->pendingArchiveLoad)
      continue;
    Symbol *l = find(("__imp_" + sym->getName()).str());
    if (!l || !l->isLazy() || l->pendingArchiveLoad)
      continue;
    if (auto *a = dyn_cast<LazyArchive>(l))
      if (identify_magic(a->getMemberBuffer().getBuffer()) !=
          file_magic::coff_import_library)
        continue;
    if (isa<LazyObject>(l))
      continue;
    lazies.push_back(l);
  }

  // Loading a lazy object parses it at once, which may add to impUndefs or
  // define a symbol that is still to be loaded.
  bool loaded = referenced;
  for (Symbol *l : lazies) {
    if (!l->isLazy() || l->pendingArchiveLoad)
      continue;
    Log(ctx) << "Loading lazy " << l->getName() << " from "
             << l->getFile()->getName() << " for __imp_" << l->getName();
    forceLazy(l);
    loaded = true;
  }
  return loaded;
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
void SymbolTable::openKCFITypes() {
  bool isX64 = ctx.config.machine == AMD64;
  if (!isX64 && ctx.config.machine != ARM64)
    return;
  // Every object with a KCFI thunk defines the scanners of the thunk's kind.
  if (!find("__llvm_kcfi_open_dynamic") &&
      !find("__llvm_kcfi_check_open_dynamic"))
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
    SmallVector<DefinedRegular *, 0> labels, entries;
    for (Symbol *s : file->getSymbols()) {
      auto *d = dyn_cast_or_null<DefinedRegular>(s);
      if (!d || d->file != file || !d->getChunk() ||
          !(d->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
        continue;
      if (d->getCOFFSymbol().getComplexType() == IMAGE_SYM_DTYPE_FUNCTION)
        entries.push_back(d);
      else if (!d->getCOFFSymbol().isExternal() &&
               d->getName().starts_with("__cfi_"))
        labels.push_back(d);
    }
    auto byLocation = [](DefinedRegular *a, DefinedRegular *b) {
      return std::make_pair(a->getChunk(), a->getValue()) <
             std::make_pair(b->getChunk(), b->getValue());
    };
    llvm::sort(entries, byLocation);
    for (DefinedRegular *label : labels) {
      auto e = llvm::upper_bound(entries, label, byLocation);
      if (getKCFIPrefixSize(label->getChunk(), label->getValue()) &&
          e != entries.end() && (*e)->getChunk() == label->getChunk())
        prefixed.insert({(*e)->getChunk(), (*e)->getValue()});
    }
    return it->second = isKCFIForeignFile(file);
  };
  // A reference to a variable that a DLL provides stays undefined until
  // automatic import resolves it to the variable's __imp_ pointer.
  auto resolve = [&](Symbol *s) -> Defined * {
    if (!s)
      return nullptr;
    if (Defined *d = s->getDefined())
      return d;
    return dyn_cast_or_null<DefinedImportData>(impSymbol(s->getName()));
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
  // identifier, which a COFF absolute symbol holds. A precise key of
  // 1 << 32 | check, printed with 1 in the high word, is an unprototyped type,
  // which stands for every precise type of the check identifier.
  auto factCheck = [&](Symbol *s) -> uint32_t {
    if (auto *d = dyn_cast_or_null<DefinedAbsolute>(s->getDefined()))
      return d->getVA();
    return 0;
  };
  // The types that the inflow and parameter facts name, by the symbol each
  // names, the nodes that they name, and the types of each node, each as
  // (check, precise key). The precise types each object opens, keyed, with the
  // check identifier. The types that a call through a precise key can hand
  // back, and the precise keys of the called types of each check identifier.
  DenseMap<Symbol *, SmallVector<std::pair<uint32_t, uint64_t>, 1>> inflows,
      params;
  SmallVector<std::pair<Symbol *, uint64_t>, 0> inflowNodes, paramNodes;
  DenseMap<uint64_t, SmallVector<std::pair<uint32_t, uint64_t>, 1>> tinflows;
  DenseMap<uint32_t, SetVector<uint64_t>> calledKeys;
  SmallVector<std::pair<uint64_t, uint64_t>, 0> tinflowNodes;
  DenseMap<uint64_t, SmallVector<std::pair<uint32_t, uint64_t>, 2>> nodes;
  SmallVector<std::pair<uint64_t, uint32_t>, 0> popens;
  // A fact names its precise key, 16 hex digits, then the symbol it concerns.
  auto parseFact = [&](StringRef rest, uint64_t &key) -> Symbol * {
    if (rest.size() < 18 || rest[16] != '_' ||
        rest.take_front(16).getAsInteger(16, key))
      return nullptr;
    return find(rest.drop_front(17));
  };
  auto parseNodeFact = [&](StringRef rest, uint64_t &node) -> Symbol * {
    if (rest.size() < 19 || rest[17] != '_' ||
        rest.substr(1, 16).getAsInteger(16, node))
      return nullptr;
    return find(rest.drop_front(18));
  };
  auto addParam = [&](Symbol *g, uint32_t check, uint64_t key) {
    params[g].push_back({check, key});
    // Code that declares g dllimport reaches our g through __imp_g, which
    // becomes a local import unless it is bound to something else.
    auto *imp =
        dyn_cast_or_null<Undefined>(find(("__imp_" + g->getName()).str()));
    if (imp && !imp->getWeakAlias())
      params[imp].push_back({check, key});
  };
  forEachSymbol([&](Symbol *s) {
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
      // The fact ends in the called type's precise key, 16 hex digits, and its
      // check identifier, 8, rather than in a symbol.
      uint64_t called = 0;
      uint32_t calledCheck = 0;
      if (name.size() < 26 || name[name.size() - 25] != '_' ||
          name.take_back(24).take_front(16).getAsInteger(16, called) ||
          name.take_back(8).getAsInteger(16, calledCheck))
        return;
      calledKeys[calledCheck].insert(called);
      name = name.drop_back(25);
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
  SmallVector<std::pair<uint64_t, uint32_t>, 0> worklist;
  auto reach = [&](uint32_t check, uint64_t key, bool local) {
    Opening &o = openings[check];
    o.local = (!o.dynamic || o.local) && local;
    o.dynamic = true;
    auto [it, inserted] = reached.try_emplace(key, local);
    if (inserted || (it->second && !local)) {
      it->second = local;
      worklist.push_back({key, check});
    }
  };
  auto openDynamically =
      [&](ArrayRef<std::pair<uint32_t, uint64_t>> types, bool local) {
        for (auto [check, key] : types)
          reach(check, key, local);
      };
  for (auto &[g, types] : inflows)
    if (isForeign(g))
      openDynamically(
          types, !isa_and_nonnull<DefinedImportThunk, DefinedImportData>(
                     resolve(g)));

  // An import is listed by its import address table entry, and the writer adds
  // a cell holding its thunk where static data holds the thunk; a definition
  // without a prefix is listed by a cell holding its address.
  llvm::sort(typeids,
             [](Symbol *a, Symbol *b) { return a->getName() < b->getName(); });
  for (Symbol *s : typeids) {
    auto *id = dyn_cast_or_null<DefinedAbsolute>(s->getDefined());
    Symbol *f = find(s->getName().substr(strlen("__kcfi_typeid_")));
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

  struct Kind {
    StringRef mismatch, staticScanner, dynamicScanner;
  };
  static const Kind kinds[] = {
      {"__llvm_kcfi_mismatch_", "__llvm_kcfi_open", "__llvm_kcfi_open_dynamic"},
      {"__llvm_kcfi_check_mismatch_", "__llvm_kcfi_check_open",
       "__llvm_kcfi_check_open_dynamic"}};
  ArrayRef<Kind> machineKinds = ArrayRef(kinds).drop_front(isX64 ? 0 : 1);
  // The scanner that m jumps to, if m is a compiled mismatch routine of the
  // kind.
  auto compiledScanner = [&](Symbol *m, const Kind &k) -> Symbol * {
    auto *r = dyn_cast_or_null<DefinedRegular>(m);
    if (!r)
      return nullptr;
    auto refs = r->getChunk()->symbols();
    for (StringRef name : {k.staticScanner, k.dynamicScanner})
      if (Symbol *scanner = find(name); scanner && is_contained(refs, scanner))
        return scanner;
    return nullptr;
  };

  // Each object publishes the precise types it opens dynamically; a call
  // through one of them can hand back a foreign function too, and so on until
  // nothing more opens. An object opening is never local: it is a cast or an
  // import, which may carry a run-time address from any image.
  for (auto [key, check] : popens)
    reach(check, key, /*local=*/false);
  // The precise key of an unprototyped type stands for every precise type of
  // its check identifier: its opening follows the facts of every called type
  // of that check identifier, and the opening of a precise type follows the
  // facts of an unprototyped called type of its check identifier as well as
  // its own.
  while (!worklist.empty()) {
    auto [key, check] = worklist.pop_back_val();
    bool local = reached[key];
    auto follow = [&](uint64_t from) {
      if (auto it = tinflows.find(from); it != tinflows.end())
        for (auto [c, k] : it->second)
          reach(c, k, local);
    };
    if (key >> 32 == 1) {
      if (auto it = calledKeys.find(check); it != calledKeys.end())
        for (uint64_t called : it->second)
          follow(called);
    } else {
      follow(key);
      follow(uint64_t(1) << 32 | check);
    }
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
    Defined *head = dyn_cast_or_null<Defined>(find("__llvm_kcfi_list_" + hex));
    auto addHead = [&] {
      auto *c = make<KCFIListChunk>(
          ctx, saver().save(".rdata$llvm_kcfi_" + hex + "_a"), nullptr, type);
      kcfiChunks.push_back(c);
      kcfiChunks.push_back(make<KCFIListChunk>(
          ctx, saver().save(".rdata$llvm_kcfi_" + hex + "_z"), nullptr,
          uint64_t(type) << 1 | 1));
      head = cast<Defined>(
          addSynthetic(saver().save("__llvm_kcfi_list_" + hex), c));
    };

    // The mismatch routine is the weak default, the trap, or a compiled
    // routine, which jumps to one of the scanners. Anything else, such as a
    // routine of another form, is left as it is.
    bool opened = false;
    for (const Kind &k : machineKinds) {
      Symbol *m = find((Twine(k.mismatch) + hex).str());
      bool replace;
      if (auto *u = dyn_cast_or_null<Undefined>(m)) {
        Defined *d = u->getDefinedWeakAlias();
        if (!d || d->getName() != "__llvm_kcfi_trap")
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
      auto *scanner = dyn_cast_or_null<Defined>(find(name));
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
      kcfiChunks.push_back(routine);
      replaceSymbol<DefinedSynthetic>(m, m->getName(), routine);
      if (opening.local)
        if (auto *s = dyn_cast_or_null<Defined>(find(k.staticScanner)))
          kcfiLocalRoutines.push_back({routine, s});
    }
    if (!opened || opening.entries.empty())
      continue;

    if (!head)
      addHead();
    StringRef section = saver().save(".rdata$llvm_kcfi_" + hex + "_m");
    for (auto [target, viaCell] : opening.entries) {
      Defined *entry = target;
      if (viaCell) {
        auto *cell = make<KCFIListChunk>(ctx, ".rdata", target);
        kcfiChunks.push_back(cell);
        entry = make<DefinedSynthetic>(target->getName(), cell);
      } else if (auto *imp = dyn_cast<DefinedImportData>(target)) {
        kcfiListedImports.push_back({imp, section});
      }
      kcfiChunks.push_back(make<KCFIListChunk>(ctx, section, entry));
    }
  }
}

uint32_t getKCFIPrefixSize(SectionChunk *sc, uint32_t off) {
  ArrayRef<uint8_t> data = sc->getContents();
  auto hasMarker = [&](uint32_t at) {
    return at + 12 <= data.size() && data[at] == 0x0F && data[at + 1] == 0x1F &&
           data[at + 2] == 0x80 && data[at + 7] == 0xB8;
  };
  return hasMarker(off) ? 12 : hasMarker(off + 4) ? 16 : 0;
}

bool isKCFIForeignFile(ObjFile *file) {
  bool definesCode = false;
  for (Symbol *s : file->getSymbols()) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (!d || d->file != file || !d->getChunk() ||
        !(d->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
      continue;
    if (!d->getCOFFSymbol().isExternal() &&
        d->getName().starts_with("__cfi_") &&
        getKCFIPrefixSize(d->getChunk(), d->getValue()))
      return false;
    // An empty section, such as the .text an assembler always emits, holds no
    // code.
    definesCode |= d->getChunk()->getSize() != 0;
  }
  return definesCode;
}

Defined *SymbolTable::impSymbol(StringRef name) {
  if (name.starts_with("__imp_"))
    return nullptr;
  return dyn_cast_or_null<Defined>(find(("__imp_" + name).str()));
}

bool SymbolTable::handleMinGWAutomaticImport(Symbol *sym, StringRef name) {
  Defined *imp = impSymbol(name);
  if (!imp)
    return false;

  // Replace the reference directly to a variable with a reference
  // to the import address table instead. This obviously isn't right,
  // but we mark the symbol as isRuntimePseudoReloc, and a later pass
  // will add runtime pseudo relocations for every relocation against
  // this Symbol. The runtime pseudo relocation framework expects the
  // reference itself to point at the IAT entry. Under -import-slots, the
  // later pass binds each word of static data holding the variable's address
  // in place instead, and reports every other reference.
  size_t impSize = 0;
  if (isa<DefinedImportData>(imp)) {
    Log(ctx) << "Automatically importing " << name << " from "
             << cast<DefinedImportData>(imp)->getDLLName();
    impSize = sizeof(DefinedImportData);
  } else if (isa<DefinedRegular>(imp)) {
    Log(ctx) << "Automatically importing " << name << " from "
             << toString(cast<DefinedRegular>(imp)->file);
    impSize = sizeof(DefinedRegular);
  } else {
    Warn(ctx) << "unable to automatically import " << name << " from "
              << imp->getName() << " from " << cast<DefinedRegular>(imp)->file
              << "; unexpected symbol type";
    return false;
  }
  sym->replaceKeepingName(imp, impSize);
  sym->isRuntimePseudoReloc = true;

  // There may exist symbols named .refptr.<name> which only consist
  // of a single pointer to <name>. If it turns out <name> is
  // automatically imported, we don't need to keep the .refptr.<name>
  // pointer at all, but redirect all accesses to it to the IAT entry
  // for __imp_<name> instead, and drop the whole .refptr.<name> chunk.
  DefinedRegular *refptr =
      dyn_cast_or_null<DefinedRegular>(find((".refptr." + name).str()));
  if (refptr && refptr->getChunk()->getSize() == ctx.config.wordsize) {
    SectionChunk *sc = dyn_cast_or_null<SectionChunk>(refptr->getChunk());
    if (sc && sc->getRelocs().size() == 1 && *sc->symbols().begin() == sym) {
      Log(ctx) << "Replacing .refptr." << name << " with " << imp->getName();
      refptr->getChunk()->live = false;
      refptr->replaceKeepingName(imp, impSize);
    }
  }
  return true;
}

/// Helper function for reportUnresolvable and resolveRemainingUndefines.
/// This function emits an "undefined symbol" diagnostic for each symbol in
/// undefs. If localImports is not nullptr, it also emits a "locally
/// defined symbol imported" diagnostic for symbols in localImports.
/// objFiles and bitcodeFiles (if not nullptr) are used to report where
/// undefined symbols are referenced.
void SymbolTable::reportProblemSymbols(
    const SmallPtrSetImpl<Symbol *> &undefs,
    const DenseMap<Symbol *, Symbol *> *localImports, bool needBitcodeFiles) {
  // Return early if there is nothing to report (which should be
  // the common case).
  if (undefs.empty() && (!localImports || localImports->empty()))
    return;

  for (Symbol *b : ctx.config.gcroot) {
    if (undefs.contains(b))
      errorOrWarn(ctx) << "<root>: undefined symbol: " << printSymbol(b);
    if (localImports)
      if (Symbol *imp = localImports->lookup(b))
        Warn(ctx) << "<root>: locally defined symbol imported: "
                  << printSymbol(imp) << " (defined in "
                  << toString(imp->getFile()) << ") [LNK4217]";
  }

  std::vector<UndefinedDiag> undefDiags;
  DenseMap<Symbol *, int> firstDiag;

  auto processFile = [&](InputFile *file, ArrayRef<Symbol *> symbols) {
    uint32_t symIndex = (uint32_t)-1;
    for (Symbol *sym : symbols) {
      ++symIndex;
      if (!sym)
        continue;
      if (undefs.contains(sym)) {
        auto [it, inserted] = firstDiag.try_emplace(sym, undefDiags.size());
        if (inserted)
          undefDiags.push_back({sym, {{file, symIndex}}});
        else
          undefDiags[it->second].files.push_back({file, symIndex});
      }
      // References whose instructions may be rewritten are reported only if
      // they still read the pointer, by bindLocalImports.
      auto *obj = dyn_cast<ObjFile>(file);
      bool deferred = obj && (obj->describesSites ||
                              (machine == ARM64 && !ctx.hybridSymtab));
      if (localImports && !deferred)
        if (Symbol *imp = localImports->lookup(sym))
          Warn(ctx) << file
                    << ": locally defined symbol imported: " << printSymbol(imp)
                    << " (defined in " << imp->getFile() << ") [LNK4217]";
    }
  };

  for (ObjFile *file : ctx.objFileInstances)
    processFile(file, file->getSymbols());

  if (needBitcodeFiles)
    for (BitcodeFile *file : bitcodeFileInstances)
      processFile(file, file->getSymbols());

  for (const UndefinedDiag &undefDiag : undefDiags)
    reportUndefinedSymbol(undefDiag);
}

void SymbolTable::reportUnresolvable() {
  SmallPtrSet<Symbol *, 8> undefs;
  for (auto &i : symMap) {
    Symbol *sym = i.second;
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef || sym->deferUndefined)
      continue;
    if (undef->getWeakAlias())
      continue;
    StringRef name = undef->getName();
    if (name.starts_with("__imp_")) {
      Symbol *imp = find(name.substr(strlen("__imp_")));
      if (Defined *def = imp ? imp->getDefined() : nullptr) {
        def->isUsedInRegularObj = true;
        continue;
      }
    }
    if (name.contains("_PchSym_"))
      continue;
    if ((ctx.config.autoImport || ctx.config.importSlots) && impSymbol(name))
      continue;
    undefs.insert(sym);
  }

  reportProblemSymbols(undefs, /*localImports=*/nullptr, true);
}

void SymbolTable::addStartStopSymbols() {
  // An ordered map keeps the diagnostics in a stable order.
  std::map<StringRef, SectionRun> runs;
  for (auto &i : symMap) {
    auto *undef = dyn_cast<Undefined>(i.second);
    if (!undef || !undef->isUsedInRegularObj || undef->getWeakAlias())
      continue;
    StringRef name = undef->getName();
    if (machine == I386 && !name.consume_front("_"))
      continue;
    bool isStop = name.consume_front("__stop_");
    if ((!isStop && !name.consume_front("__start_")) ||
        !isValidCIdentifier(name))
      continue;
    SectionRun &run = runs[name];
    run.name = name;
    (isStop ? run.stop : run.start) = undef;
  }
  if (runs.empty())
    return;

  for (Chunk *c : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(c);
    if (!sc)
      continue;
    auto it = runs.find(sc->getSectionName().split('$').first);
    if (it != runs.end())
      it->second.chunks.push_back(sc);
  }

  for (auto &[name, run] : runs) {
    // With no section to bound, the reference stays undefined.
    if (run.chunks.empty())
      continue;
    // A bound an input defines cannot be paired with one the linker places.
    StringRef startName = mangle(saver().save("__start_" + name));
    StringRef stopName = mangle(saver().save("__stop_" + name));
    Symbol *other = run.start ? find(stopName) : find(startName);
    if ((!run.start || !run.stop) && other && isa<Defined>(other)) {
      Err(ctx) << (run.start ? startName : stopName)
               << " cannot be defined by the linker: "
               << (run.start ? stopName : startName)
               << " is defined by an input";
      continue;
    }
    if (run.start)
      addSynthetic(startName, nullptr);
    if (run.stop)
      addSynthetic(stopName, nullptr);
    sectionRuns.push_back(std::move(run));
  }
}

void SymbolTable::addBoundarySymbols() {
  for (const char *n : {"_etext", "etext", "_edata", "edata", "_end", "end"}) {
    auto *undef = dyn_cast_or_null<Undefined>(find(mangle(n)));
    if (!undef || !undef->isUsedInRegularObj || undef->getWeakAlias())
      continue;
    boundarySymbols.push_back(addSynthetic(undef->getName(), nullptr));
  }
}

void SymbolTable::resolveRemainingUndefines(std::vector<Undefined *> &aliases) {
  llvm::TimeTraceScope timeScope("Resolve remaining undefined symbols");
  SmallPtrSet<Symbol *, 8> undefs;
  DenseMap<Symbol *, Symbol *> localImports;

  for (auto &i : symMap) {
    Symbol *sym = i.second;
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef)
      continue;
    if (!sym->isUsedInRegularObj)
      continue;

    StringRef name = undef->getName();

    // A weak alias may have been resolved, so check for that.
    if (undef->getWeakAlias()) {
      aliases.push_back(undef);
      continue;
    }

    // If we can resolve a symbol by removing __imp_ prefix, do that.
    // This odd rule is for compatibility with MSVC linker.
    if (name.starts_with("__imp_")) {
      auto findLocalSym = [&](StringRef n) {
        Symbol *sym = find(n);
        return sym ? sym->getDefined() : nullptr;
      };

      StringRef impName = name.substr(strlen("__imp_"));
      Defined *imp = findLocalSym(impName);
      if (!imp && isEC()) {
        // Try to use the mangled symbol on ARM64EC.
        std::optional<std::string> mangledName =
            getArm64ECMangledFunctionName(impName);
        if (mangledName)
          imp = findLocalSym(*mangledName);
        if (!imp && impName.consume_front("aux_")) {
          // If it's a __imp_aux_ symbol, try skipping the aux_ prefix.
          imp = findLocalSym(impName);
          if (!imp && (mangledName = getArm64ECMangledFunctionName(impName)))
            imp = findLocalSym(*mangledName);
        }
      }
      if (imp) {
        replaceSymbol<DefinedLocalImport>(sym, ctx, name, imp);
        localImportChunks.push_back(cast<DefinedLocalImport>(sym)->getChunk());
        localImports[sym] = imp;
        continue;
      }
    }

    // We don't want to report missing Microsoft precompiled headers symbols.
    // A proper message will be emitted instead in PDBLinker::aquirePrecompObj
    if (name.contains("_PchSym_"))
      continue;

    if ((ctx.config.autoImport || ctx.config.importSlots) &&
        handleMinGWAutomaticImport(sym, name))
      continue;

    // Remaining undefined symbols are not fatal if /force is specified.
    // They are replaced with dummy defined symbols.
    if (ctx.config.forceUnresolved)
      replaceSymbol<DefinedAbsolute>(sym, ctx, name, 0);
    undefs.insert(sym);
  }

  reportProblemSymbols(
      undefs, ctx.config.warnLocallyDefinedImported ? &localImports : nullptr,
      false);
}

// A .refptr.X section holds only a pointer to X, which the compiler reads
// where X may be outside the image or absent. Once X is resolved, the pointer
// is one the link has or can make. When X is defined in the image, or is an
// absent weak reference, it binds a local import pointer to X or to zero,
// whose rewritten references reach X or zero directly. That costs nothing
// and changes no value, but pays only where references can be rewritten: on
// ARM64, and on x86-64 in objects that describe their sites. The section
// stays the pointer whenever the image needs one, as bindLocalImports
// decides. Under -import-slots, it becomes X's import pointer when X is
// imported, which every reader can read in its place, and the section is
// left out.
void SymbolTable::bindPointerCells() {
  if (isEC() || ctx.hybridSymtab)
    return;
  bool bindLocal =
      machine == ARM64 ||
      (machine == AMD64 && llvm::any_of(ctx.objFileInstances, [](ObjFile *f) {
         return f->describesSites;
       }));
  if (!bindLocal && !ctx.config.importSlots)
    return;
  llvm::TimeTraceScope timeScope("Bind pointer cells");
  SmallPtrSet<Symbol *, 4> importedWeakData;
  for (auto &i : symMap) {
    auto *cell = dyn_cast<DefinedRegular>(i.second);
    if (!cell || !cell->getName().starts_with(".refptr."))
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
    auto *imp = dyn_cast_or_null<DefinedImportData>(
        find(("__imp_" + name).str()));
    auto *abs = dyn_cast_or_null<DefinedAbsolute>(target);
    bool absentWeak = isa<Undefined>(x) && abs && abs->getVA() == 0;
    if (ctx.config.importSlots && imp &&
        (isa<DefinedImportThunk>(x) || absentWeak)) {
      cell->replaceKeepingName(imp, sizeof(DefinedImportData));
      if (absentWeak)
        importedWeakData.insert(x);
      sc->live = false;
    } else if (bindLocal && target && !isa<DefinedImportThunk>(target) &&
               !isa<DefinedImportData>(target) && (!abs || absentWeak)) {
      replaceSymbol<DefinedLocalImport>(cell, ctx, cell->getName(), target);
      LocalImportChunk *c = cast<DefinedLocalImport>(cell)->getChunk();
      c->cell = sc;
      // Mark-live keeps the pointer when something live refers to it.
      c->live = !ctx.config.doGC;
      localImportChunks.push_back(c);
    }
  }

  // Imported data has no address in the image, so a reference to it other
  // than through its pointer would read zero.
  if (importedWeakData.empty())
    return;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    SmallPtrSet<Symbol *, 4> reported;
    for (Chunk *c : file->getChunks())
      if (auto *sc = dyn_cast_or_null<SectionChunk>(c);
          sc && !sc->getSectionName().starts_with(".rdata$.refptr."))
        for (const coff_relocation &rel : sc->getRelocs()) {
          Symbol *s = file->getSymbol(rel.SymbolTableIndex);
          if (importedWeakData.contains(s) && reported.insert(s).second)
            Err(ctx) << file << ": weak reference to " << printSymbol(s)
                     << ", which is imported, needs its address in "
                     << sc->getSectionName();
        }
  }
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
// scanned, and only when the link has one.
void SymbolTable::bindLocalImports() {
  if (localImportChunks.empty())
    return;
  llvm::TimeTraceScope timeScope("Bind local imports");
  bool arm64 = machine == ARM64 && !ctx.hybridSymtab;
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
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    auto isLocalImport = [](Symbol *s) {
      return isa_and_nonnull<DefinedLocalImport>(s);
    };
    if (!file->describesSites && !arm64) {
      for (Symbol *s : file->getSymbols())
        if (isLocalImport(s))
          read.insert(cast<DefinedLocalImport>(s));
      continue;
    }
    if (llvm::none_of(file->getSymbols(), isLocalImport))
      continue;

    MapVector<DefinedLocalImport *, bool> seen;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      bool fromData = ctx.config.importSlots &&
                      !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE);
      for (const coff_relocation &rel : sc->getRelocs()) {
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        if (!isLocalImport(s))
          continue;
        auto *li = cast<DefinedLocalImport>(s);
        if (fromData) {
          readByData.insert(li);
          continue;
        }
        bool mismatch = false;
        bool rewritable = arm64 ? sc->isArm64LocalImportPageRef(rel)
                                : sc->getLocalImportRewrite(rel, &mismatch)
                                      .has_value();
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
    // A .refptr pointer is the compiler's, not a dllimport declaration.
    for (auto [li, rewritable] : seen)
      if (!li->getName().starts_with(".refptr."))
        refs.push_back({file, li, rewritable});
  }

  // A pointer that a compiler made stays where it is, as the pointer, if
  // something live reads it other than through a rewritten instruction, or if
  // its section is otherwise kept.
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

  if (!ctx.config.warnLocallyDefinedImported)
    return;
  for (const Ref &r : refs)
    if (!r.rewritable || (arm64 && !r.li->getChunk()->bypassed))
      Warn(ctx) << r.file << ": locally defined symbol imported: "
                << printSymbol(r.li->getTarget()) << " (defined in "
                << r.li->getTarget()->getFile() << ") [LNK4217]";
}

std::pair<Symbol *, bool> SymbolTable::insert(StringRef name) {
  bool inserted = false;
  Symbol *&sym = symMap[CachedHashStringRef(name)];
  if (!sym) {
    sym = reinterpret_cast<Symbol *>(make<SymbolUnion>());
    sym->isUsedInRegularObj = false;
    sym->pendingArchiveLoad = false;
    sym->canInline = true;
    inserted = true;

    if (isEC() && name.starts_with("EXP+"))
      expSymbols.push_back(sym);
  }
  return {sym, inserted};
}

std::pair<Symbol *, bool> SymbolTable::insert(StringRef name, InputFile *file) {
  std::pair<Symbol *, bool> result = insert(name);
  if (!file || !isa<BitcodeFile>(file))
    result.first->isUsedInRegularObj = true;
  return result;
}

void SymbolTable::initializeLoadConfig() {
  auto sym =
      dyn_cast_or_null<DefinedRegular>(findUnderscore("_load_config_used"));
  if (!sym) {
    if (isEC()) {
      Warn(ctx) << "EC version of '_load_config_used' is missing";
      return;
    }
    if (ctx.config.machine == ARM64X) {
      Warn(ctx) << "native version of '_load_config_used' is missing for "
                   "ARM64X target";
      return;
    }
    if (ctx.config.guardCF & GuardCFLevel::CF)
      Warn(ctx)
          << "Control Flow Guard is enabled but '_load_config_used' is missing";
    if (ctx.config.dependentLoadFlags)
      Warn(ctx) << "_load_config_used not found, /dependentloadflag will have "
                   "no effect";
    return;
  }

  SectionChunk *sc = sym->getChunk();
  if (!sc->hasData) {
    Err(ctx) << "_load_config_used points to uninitialized data";
    return;
  }
  uint64_t offsetInChunk = sym->getValue();
  if (offsetInChunk + 4 > sc->getSize()) {
    Err(ctx) << "_load_config_used section chunk is too small";
    return;
  }

  ArrayRef<uint8_t> secContents = sc->getContents();
  loadConfigSize =
      *reinterpret_cast<const ulittle32_t *>(&secContents[offsetInChunk]);
  if (offsetInChunk + loadConfigSize > sc->getSize()) {
    Err(ctx) << "_load_config_used specifies a size larger than its containing "
                "section chunk";
    return;
  }

  uint32_t expectedAlign = ctx.config.is64() ? 8 : 4;
  if (sc->getAlignment() < expectedAlign)
    Warn(ctx) << "'_load_config_used' is misaligned (expected alignment to be "
              << expectedAlign << " bytes, got " << sc->getAlignment()
              << " instead)";
  else if (!isAligned(Align(expectedAlign), offsetInChunk))
    Warn(ctx) << "'_load_config_used' is misaligned (section offset is 0x"
              << Twine::utohexstr(sym->getValue()) << " not aligned to "
              << expectedAlign << " bytes)";

  loadConfigSym = sym;
}

void SymbolTable::addEntryThunk(Symbol *from, Symbol *to) {
  entryThunks.push_back({from, to});
}

void SymbolTable::addExitThunk(Symbol *from, Symbol *to) {
  exitThunks[from] = to;
}

void SymbolTable::initializeECThunks() {
  if (!isArm64EC(ctx.config.machine))
    return;

  for (auto it : entryThunks) {
    Defined *to = it.second->getDefined();
    if (!to)
      continue;
    auto *from = dyn_cast_or_null<DefinedRegular>(it.first->getDefined());
    // We need to be able to add padding to the function and fill it with an
    // offset to its entry thunks. To ensure that padding the function is
    // feasible, functions are required to be COMDAT symbols with no offset.
    if (!from || !from->getChunk()->isCOMDAT() ||
        cast<DefinedRegular>(from)->getValue()) {
      Err(ctx) << "non COMDAT symbol '" << from->getName() << "' in hybrid map";
      continue;
    }
    from->getChunk()->setEntryThunk(to);
  }

  for (ImportFile *file : ctx.importFileInstances) {
    if (!file->impchkThunk)
      continue;

    Symbol *sym = exitThunks.lookup(file->thunkSym);
    if (!sym)
      sym = exitThunks.lookup(file->impECSym);
    if (sym)
      file->impchkThunk->exitThunk = sym->getDefined();
  }

  // On ARM64EC, the __imp_ symbol references the auxiliary IAT, while the
  // __imp_aux_ symbol references the regular IAT. However, x86_64 code expects
  // both to reference the regular IAT, so adjust the symbol if necessary.
  parallelForEach(ctx.objFileInstances, [&](ObjFile *file) {
    if (file->getMachineType() != AMD64)
      return;
    for (auto &sym : file->getMutableSymbols()) {
      auto impSym = dyn_cast_or_null<DefinedImportData>(sym);
      if (impSym && impSym->file->impchkThunk && sym == impSym->file->impECSym)
        sym = impSym->file->impSym;
    }
  });
}

void SymbolTable::initializeSameAddressThunks() {
  for (auto iter : ctx.config.sameAddresses) {
    auto sym = dyn_cast_or_null<DefinedRegular>(iter.first->getDefined());
    if (!sym || !sym->isLive())
      continue;
    auto nativeSym =
        dyn_cast_or_null<DefinedRegular>(iter.second->getDefined());
    if (!nativeSym || !nativeSym->isLive())
      continue;
    Defined *entryThunk = sym->getChunk()->getEntryThunk();
    if (!entryThunk)
      continue;

    // Replace symbols with symbols referencing the thunk. Store the original
    // symbol as equivalent DefinedSynthetic instances for use in the thunk
    // itself.
    auto symClone = make<DefinedSynthetic>(sym->getName(), sym->getChunk(),
                                           sym->getValue());
    auto nativeSymClone = make<DefinedSynthetic>(
        nativeSym->getName(), nativeSym->getChunk(), nativeSym->getValue());
    SameAddressThunkARM64EC *thunk =
        make<SameAddressThunkARM64EC>(nativeSymClone, symClone, entryThunk);
    sameAddressThunks.push_back(thunk);

    replaceSymbol<DefinedSynthetic>(sym, sym->getName(), thunk);
    replaceSymbol<DefinedSynthetic>(nativeSym, nativeSym->getName(), thunk);
  }
}

Symbol *SymbolTable::addUndefined(StringRef name, InputFile *f,
                                  bool overrideLazy) {
  auto [s, wasInserted] = insert(name, f);
  if (wasInserted || (s->isLazy() && overrideLazy)) {
    replaceSymbol<Undefined>(s, name);
    if (ctx.config.importSlots && name.starts_with("__imp_"))
      impUndefs.push_back(s);
    return s;
  }
  if (s->isLazy())
    forceLazy(s);
  return s;
}

Symbol *SymbolTable::addGCRoot(StringRef name, bool aliasEC) {
  Symbol *b = addUndefined(name);
  if (!b->isGCRoot) {
    b->isGCRoot = true;
    ctx.config.gcroot.push_back(b);
  }

  // On ARM64EC, a symbol may be defined in either its mangled or demangled form
  // (or both). Define an anti-dependency symbol that binds both forms, similar
  // to how compiler-generated code references external functions.
  if (aliasEC && isEC()) {
    if (std::optional<std::string> mangledName =
            getArm64ECMangledFunctionName(name)) {
      auto u = dyn_cast<Undefined>(b);
      if (u && !u->weakAlias) {
        Symbol *t = addUndefined(saver().save(*mangledName));
        u->setWeakAlias(t, true);
      }
    } else if (std::optional<std::string> demangledName =
                   getArm64ECDemangledFunctionName(name)) {
      Symbol *us = addUndefined(saver().save(*demangledName));
      auto u = dyn_cast<Undefined>(us);
      if (u && !u->weakAlias)
        u->setWeakAlias(b, true);
    }
  }
  return b;
}

// On ARM64EC, a function symbol may appear in both mangled and demangled forms:
// - ARM64EC archives contain only the mangled name, while the demangled symbol
//   is defined by the object file as an alias.
// - x86_64 archives contain only the demangled name (the mangled name is
//   usually defined by an object referencing the symbol as an alias to a guess
//   exit thunk).
// - ARM64EC import files contain both the mangled and demangled names for
//   thunks.
// If more than one archive defines the same function, this could lead
// to different libraries being used for the same function depending on how they
// are referenced. Avoid this by checking if the paired symbol is already
// defined before adding a symbol to the table.
template <typename T>
bool checkLazyECPair(SymbolTable *symtab, StringRef name, InputFile *f) {
  if (name.starts_with("__imp_"))
    return true;
  std::string pairName;
  if (std::optional<std::string> mangledName =
          getArm64ECMangledFunctionName(name))
    pairName = std::move(*mangledName);
  else if (std::optional<std::string> demangledName =
               getArm64ECDemangledFunctionName(name))
    pairName = std::move(*demangledName);
  else
    return true;

  Symbol *sym = symtab->find(pairName);
  if (!sym)
    return true;
  if (sym->pendingArchiveLoad)
    return false;
  if (auto u = dyn_cast<Undefined>(sym))
    return !u->weakAlias || u->isAntiDep;
  // If the symbol is lazy, allow it only if it originates from the same
  // archive.
  auto lazy = dyn_cast<T>(sym);
  return lazy && lazy->file == f;
}

void SymbolTable::addLazyArchive(ArchiveFile *f, const Archive::Symbol &sym) {
  StringRef name = sym.getName();
  if (isEC() && !checkLazyECPair<LazyArchive>(this, name, f))
    return;
  auto [s, wasInserted] = insert(name);
  if (wasInserted) {
    replaceSymbol<LazyArchive>(s, f, sym);
    return;
  }
  auto *u = dyn_cast<Undefined>(s);
  if (!u || (u->weakAlias && !u->isECAlias(machine)) || s->pendingArchiveLoad)
    return;
  s->pendingArchiveLoad = true;
  f->addMember(sym);
}

void SymbolTable::addLazyObject(InputFile *f, StringRef n) {
  assert(f->lazy);
  if (isEC() && !checkLazyECPair<LazyObject>(this, n, f))
    return;
  auto [s, wasInserted] = insert(n, f);
  if (wasInserted) {
    replaceSymbol<LazyObject>(s, f, n);
    return;
  }
  auto *u = dyn_cast<Undefined>(s);
  if (!u || (u->weakAlias && !u->isECAlias(machine)) || s->pendingArchiveLoad)
    return;
  s->pendingArchiveLoad = true;
  f->lazy = false;
  ctx.driver.addFile(f);
}

void SymbolTable::addLazyDLLSymbol(DLLFile *f, DLLFile::Symbol *sym,
                                   StringRef n) {
  auto [s, wasInserted] = insert(n);
  if (wasInserted) {
    replaceSymbol<LazyDLLSymbol>(s, f, sym, n);
    return;
  }
  auto *u = dyn_cast<Undefined>(s);
  if (!u || (u->weakAlias && !u->isECAlias(machine)) || s->pendingArchiveLoad)
    return;
  s->pendingArchiveLoad = true;
  f->makeImport(sym);
}

static std::string getSourceLocationBitcode(BitcodeFile *file) {
  std::string res("\n>>> defined at ");
  StringRef source = file->obj->getSourceFileName();
  if (!source.empty())
    res += source.str() + "\n>>>            ";
  res += toString(file);
  return res;
}

static std::string getSourceLocationObj(ObjFile *file, SectionChunk *sc,
                                        uint32_t offset, StringRef name) {
  std::optional<std::pair<StringRef, uint32_t>> fileLine;
  if (sc)
    fileLine = getFileLine(sc, offset);
  if (!fileLine)
    fileLine = file->getVariableLocation(name);

  std::string res;
  llvm::raw_string_ostream os(res);
  os << "\n>>> defined at ";
  if (fileLine)
    os << fileLine->first << ":" << fileLine->second << "\n>>>            ";
  os << toString(file);
  return res;
}

static std::string getSourceLocation(InputFile *file, SectionChunk *sc,
                                     uint32_t offset, StringRef name) {
  if (!file)
    return "";
  if (auto *o = dyn_cast<ObjFile>(file))
    return getSourceLocationObj(o, sc, offset, name);
  if (auto *b = dyn_cast<BitcodeFile>(file))
    return getSourceLocationBitcode(b);
  return "\n>>> defined at " + toString(file);
}

// Construct and print an error message in the form of:
//
//   lld-link: error: duplicate symbol: foo
//   >>> defined at bar.c:30
//   >>>            bar.o
//   >>> defined at baz.c:563
//   >>>            baz.o
void SymbolTable::reportDuplicate(Symbol *existing, InputFile *newFile,
                                  SectionChunk *newSc,
                                  uint32_t newSectionOffset) {
  COFFSyncStream diag(ctx, ctx.config.forceMultiple ? DiagLevel::Warn
                                                    : DiagLevel::Err);
  diag << "duplicate symbol: " << printSymbol(existing);

  DefinedRegular *d = dyn_cast<DefinedRegular>(existing);
  if (d && isa<ObjFile>(d->getFile())) {
    diag << getSourceLocation(d->getFile(), d->getChunk(), d->getValue(),
                              existing->getName());
  } else {
    diag << getSourceLocation(existing->getFile(), nullptr, 0, "");
  }
  diag << getSourceLocation(newFile, newSc, newSectionOffset,
                            existing->getName());
}

Symbol *SymbolTable::addAbsolute(StringRef n, COFFSymbolRef sym) {
  auto [s, wasInserted] = insert(n, nullptr);
  s->isUsedInRegularObj = true;
  if (wasInserted || isa<Undefined>(s) || s->isLazy())
    replaceSymbol<DefinedAbsolute>(s, ctx, n, sym);
  else if (auto *da = dyn_cast<DefinedAbsolute>(s)) {
    if (da->getVA() != sym.getValue())
      reportDuplicate(s, nullptr);
  } else if (!isa<DefinedCOFF>(s))
    reportDuplicate(s, nullptr);
  return s;
}

Symbol *SymbolTable::addAbsolute(StringRef n, uint64_t va) {
  auto [s, wasInserted] = insert(n, nullptr);
  s->isUsedInRegularObj = true;
  if (wasInserted || isa<Undefined>(s) || s->isLazy())
    replaceSymbol<DefinedAbsolute>(s, ctx, n, va);
  else if (auto *da = dyn_cast<DefinedAbsolute>(s)) {
    if (da->getVA() != va)
      reportDuplicate(s, nullptr);
  } else if (!isa<DefinedCOFF>(s))
    reportDuplicate(s, nullptr);
  return s;
}

Symbol *SymbolTable::addSynthetic(StringRef n, Chunk *c) {
  auto [s, wasInserted] = insert(n, nullptr);
  s->isUsedInRegularObj = true;
  if (wasInserted || isa<Undefined>(s) || s->isLazy())
    replaceSymbol<DefinedSynthetic>(s, n, c);
  else if (!isa<DefinedCOFF>(s))
    reportDuplicate(s, nullptr);
  return s;
}

Symbol *SymbolTable::addRegular(InputFile *f, StringRef n,
                                const coff_symbol_generic *sym, SectionChunk *c,
                                uint32_t sectionOffset, bool isWeak) {
  auto [s, wasInserted] = insert(n, f);
  if (wasInserted || !isa<DefinedRegular>(s) || s->isWeak)
    replaceSymbol<DefinedRegular>(s, f, n, /*IsCOMDAT*/ false,
                                  /*IsExternal*/ true, sym, c, isWeak);
  else if (!isWeak)
    reportDuplicate(s, f, c, sectionOffset);
  return s;
}

std::pair<DefinedRegular *, bool>
SymbolTable::addComdat(InputFile *f, StringRef n,
                       const coff_symbol_generic *sym) {
  auto [s, wasInserted] = insert(n, f);
  if (wasInserted || !isa<DefinedRegular>(s)) {
    replaceSymbol<DefinedRegular>(s, f, n, /*IsCOMDAT*/ true,
                                  /*IsExternal*/ true, sym, nullptr);
    return {cast<DefinedRegular>(s), true};
  }
  auto *existingSymbol = cast<DefinedRegular>(s);
  if (!existingSymbol->isCOMDAT)
    reportDuplicate(s, f);
  return {existingSymbol, false};
}

Symbol *SymbolTable::addCommon(InputFile *f, StringRef n, uint64_t size,
                               const coff_symbol_generic *sym, CommonChunk *c) {
  auto [s, wasInserted] = insert(n, f);
  if (wasInserted || !isa<DefinedCOFF>(s))
    replaceSymbol<DefinedCommon>(s, f, n, size, sym, c);
  else if (auto *dc = dyn_cast<DefinedCommon>(s))
    if (size > dc->getSize())
      replaceSymbol<DefinedCommon>(s, f, n, size, sym, c);
  return s;
}

DefinedImportData *SymbolTable::addImportData(StringRef n, ImportFile *f,
                                              ImportLocation &location) {
  auto [s, wasInserted] = insert(n, nullptr);
  s->isUsedInRegularObj = true;
  if (wasInserted || isa<Undefined>(s) || s->isLazy()) {
    replaceSymbol<DefinedImportData>(s, n, f, location);
    return cast<DefinedImportData>(s);
  }

  reportDuplicate(s, f);
  return nullptr;
}

Defined *SymbolTable::addImportThunk(StringRef name, DefinedImportData *id,
                                     ImportThunkChunk *chunk) {
  auto [s, wasInserted] = insert(name, nullptr);
  s->isUsedInRegularObj = true;
  if (wasInserted || isa<Undefined>(s) || s->isLazy()) {
    replaceSymbol<DefinedImportThunk>(s, ctx, name, id, chunk);
    return cast<Defined>(s);
  }

  reportDuplicate(s, id->file);
  return nullptr;
}

void SymbolTable::addLibcall(StringRef name) {
  Symbol *sym = findUnderscore(name);
  if (!sym)
    return;

  if (auto *l = dyn_cast<LazyArchive>(sym)) {
    MemoryBufferRef mb = l->getMemberBuffer();
    if (isBitcode(mb))
      addUndefined(sym->getName());
  } else if (LazyObject *o = dyn_cast<LazyObject>(sym)) {
    if (isBitcode(o->file->mb))
      addUndefined(sym->getName());
  }
}

Symbol *SymbolTable::find(StringRef name) const {
  return symMap.lookup(CachedHashStringRef(name));
}

Symbol *SymbolTable::findUnderscore(StringRef name) const {
  if (machine == I386)
    return find(("_" + name).str());
  return find(name);
}

// Return all symbols that start with Prefix, possibly ignoring the first
// character of Prefix or the first character symbol.
std::vector<Symbol *> SymbolTable::getSymsWithPrefix(StringRef prefix) {
  std::vector<Symbol *> syms;
  for (auto pair : symMap) {
    StringRef name = pair.first.val();
    if (name.starts_with(prefix) || name.starts_with(prefix.drop_front()) ||
        name.drop_front().starts_with(prefix) ||
        name.drop_front().starts_with(prefix.drop_front())) {
      syms.push_back(pair.second);
    }
  }
  return syms;
}

Symbol *SymbolTable::findMangle(StringRef name) {
  if (Symbol *sym = find(name)) {
    if (auto *u = dyn_cast<Undefined>(sym)) {
      // We're specifically looking for weak aliases that ultimately resolve to
      // defined symbols, hence the call to getWeakAlias() instead of just using
      // the weakAlias member variable. This matches link.exe's behavior.
      if (Symbol *weakAlias = u->getWeakAlias())
        return weakAlias;
    } else {
      return sym;
    }
  }

  // Efficient fuzzy string lookup is impossible with a hash table, so iterate
  // the symbol table once and collect all possibly matching symbols into this
  // vector. Then compare each possibly matching symbol with each possible
  // mangling.
  std::vector<Symbol *> syms = getSymsWithPrefix(name);
  auto findByPrefix = [&syms](const Twine &t) -> Symbol * {
    std::string prefix = t.str();
    for (auto *s : syms)
      if (s->getName().starts_with(prefix))
        return s;
    return nullptr;
  };

  // For non-x86, just look for C++ functions.
  if (machine != I386)
    return findByPrefix("?" + name + "@@Y");

  if (!name.starts_with("_"))
    return nullptr;
  // Search for x86 stdcall function.
  if (Symbol *s = findByPrefix(name + "@"))
    return s;
  // Search for x86 fastcall function.
  if (Symbol *s = findByPrefix("@" + name.substr(1) + "@"))
    return s;
  // Search for x86 vectorcall function.
  if (Symbol *s = findByPrefix(name.substr(1) + "@@"))
    return s;
  // Search for x86 C++ non-member function.
  return findByPrefix("?" + name.substr(1) + "@@Y");
}

bool SymbolTable::findUnderscoreMangle(StringRef sym) {
  Symbol *s = findMangle(mangle(sym));
  return s && !isa<Undefined>(s);
}

// Symbol names are mangled by prepending "_" on x86.
StringRef SymbolTable::mangle(StringRef sym) {
  assert(machine != IMAGE_FILE_MACHINE_UNKNOWN);
  if (machine == I386)
    return saver().save("_" + sym);
  return sym;
}

StringRef SymbolTable::mangleMaybe(Symbol *s) {
  // If the plain symbol name has already been resolved, do nothing.
  Undefined *unmangled = dyn_cast<Undefined>(s);
  if (!unmangled)
    return "";

  // Otherwise, see if a similar, mangled symbol exists in the symbol table.
  Symbol *mangled = findMangle(unmangled->getName());
  if (!mangled)
    return "";

  // If we find a similar mangled symbol, make this an alias to it and return
  // its name.
  Log(ctx) << unmangled->getName() << " aliased to " << mangled->getName();
  unmangled->setWeakAlias(addUndefined(mangled->getName()));
  return mangled->getName();
}

// Windows specific -- find default entry point name.
//
// There are four different entry point functions for Windows executables,
// each of which corresponds to a user-defined "main" function. This function
// infers an entry point from a user-defined "main" function.
StringRef SymbolTable::findDefaultEntry() {
  assert(ctx.config.subsystem != IMAGE_SUBSYSTEM_UNKNOWN &&
         "must handle /subsystem before calling this");

  if (ctx.config.mingw)
    return mangle(ctx.config.subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI
                      ? "WinMainCRTStartup"
                      : "mainCRTStartup");

  if (ctx.config.subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI) {
    if (findUnderscoreMangle("wWinMain")) {
      if (!findUnderscoreMangle("WinMain"))
        return mangle("wWinMainCRTStartup");
      Warn(ctx) << "found both wWinMain and WinMain; using latter";
    }
    return mangle("WinMainCRTStartup");
  }
  if (findUnderscoreMangle("wmain")) {
    if (!findUnderscoreMangle("main"))
      return mangle("wmainCRTStartup");
    Warn(ctx) << "found both wmain and main; using latter";
  }
  return mangle("mainCRTStartup");
}

WindowsSubsystem SymbolTable::inferSubsystem() {
  if (ctx.config.dll)
    return IMAGE_SUBSYSTEM_WINDOWS_GUI;
  if (ctx.config.mingw)
    return IMAGE_SUBSYSTEM_WINDOWS_CUI;
  // Note that link.exe infers the subsystem from the presence of these
  // functions even if /entry: or /nodefaultlib are passed which causes them
  // to not be called.
  bool haveMain = findUnderscoreMangle("main");
  bool haveWMain = findUnderscoreMangle("wmain");
  bool haveWinMain = findUnderscoreMangle("WinMain");
  bool haveWWinMain = findUnderscoreMangle("wWinMain");
  if (haveMain || haveWMain) {
    if (haveWinMain || haveWWinMain) {
      Warn(ctx) << "found " << (haveMain ? "main" : "wmain") << " and "
                << (haveWinMain ? "WinMain" : "wWinMain")
                << "; defaulting to /subsystem:console";
    }
    return IMAGE_SUBSYSTEM_WINDOWS_CUI;
  }
  if (haveWinMain || haveWWinMain)
    return IMAGE_SUBSYSTEM_WINDOWS_GUI;
  return IMAGE_SUBSYSTEM_UNKNOWN;
}

void SymbolTable::addUndefinedGlob(StringRef arg) {
  Expected<GlobPattern> pat = GlobPattern::create(arg);
  if (!pat) {
    Err(ctx) << "/includeglob: " << toString(pat.takeError());
    return;
  }

  SmallVector<Symbol *, 0> syms;
  forEachSymbol([&syms, &pat](Symbol *sym) {
    if (pat->match(sym->getName())) {
      syms.push_back(sym);
    }
  });

  for (Symbol *sym : syms)
    addGCRoot(sym->getName());
}

// Convert stdcall/fastcall style symbols into unsuffixed symbols,
// with or without a leading underscore. (MinGW specific.)
static StringRef killAt(StringRef sym, bool prefix) {
  if (sym.empty())
    return sym;
  // Strip any trailing stdcall suffix
  sym = sym.substr(0, sym.find('@', 1));
  if (!sym.starts_with("@")) {
    if (prefix && !sym.starts_with("_"))
      return saver().save("_" + sym);
    return sym;
  }
  // For fastcall, remove the leading @ and replace it with an
  // underscore, if prefixes are used.
  sym = sym.substr(1);
  if (prefix)
    sym = saver().save("_" + sym);
  return sym;
}

static StringRef exportSourceName(ExportSource s) {
  switch (s) {
  case ExportSource::Directives:
    return "source file (directives)";
  case ExportSource::Export:
    return "/export";
  case ExportSource::ModuleDefinition:
    return "/def";
  case ExportSource::ExportAll:
    return "/export-all-symbols";
  default:
    llvm_unreachable("unknown ExportSource");
  }
}

static int exportSourcePriority(ExportSource s) {
  switch (s) {
  case ExportSource::Directives:
    return 1;
  case ExportSource::ExportAll:
    // Give directives (embedded in object files, from dllexport attributes)
    // and linker generated exports (from /export-all-symbols) differing
    // priority, to avoid warnings about conflicts between the two. In
    // practice, there shouldn't be any conflicts between the two, as both
    // should set the DATA flag consistently. Both have lower priority than
    // def files and explicit export arguments.
    return 2;
  case ExportSource::Export:
  case ExportSource::ModuleDefinition:
    // Give the same priority to export arguments and def files; this produces
    // warnings if they are duplicate and if they differ.
    return 3;
  default:
    llvm_unreachable("unknown ExportSource");
  }
}

// Performs error checking on all /export arguments.
// It also sets ordinals.
void SymbolTable::fixupExports() {
  llvm::TimeTraceScope timeScope("Fixup exports");
  // Symbol ordinals must be unique.
  std::set<uint16_t> ords;
  for (Export &e : exports) {
    if (e.ordinal == 0)
      continue;
    if (!ords.insert(e.ordinal).second)
      Fatal(ctx) << "duplicate export ordinal: " << e.name;
  }

  for (Export &e : exports) {
    if (!e.exportAs.empty()) {
      e.exportName = e.exportAs;
      continue;
    }

    StringRef sym =
        !e.forwardTo.empty() || e.extName.empty() ? e.name : e.extName;
    if (machine == I386 && sym.starts_with("_")) {
      // In MSVC mode, a fully decorated stdcall function is exported
      // as-is with the leading underscore (with type IMPORT_NAME).
      // In MinGW mode, a decorated stdcall function gets the underscore
      // removed, just like normal cdecl functions.
      if (ctx.config.mingw || !sym.contains('@')) {
        e.exportName = sym.substr(1);
        continue;
      }
    }
    if (isEC() && !e.data && !e.constant) {
      if (std::optional<std::string> demangledName =
              getArm64ECDemangledFunctionName(sym)) {
        e.exportName = saver().save(*demangledName);
        continue;
      }
    }
    e.exportName = sym;
  }

  if (ctx.config.killAt && machine == I386) {
    for (Export &e : exports) {
      e.name = killAt(e.name, true);
      e.exportName = killAt(e.exportName, false);
      e.extName = killAt(e.extName, true);
      e.symbolName = killAt(e.symbolName, true);
    }
  }

  // Uniquefy by name.
  DenseMap<StringRef, std::pair<Export *, unsigned>> map(exports.size());
  std::vector<Export> v;
  for (Export &e : exports) {
    auto pair = map.insert(std::make_pair(e.exportName, std::make_pair(&e, 0)));
    bool inserted = pair.second;
    if (inserted) {
      pair.first->second.second = v.size();
      v.push_back(e);
      continue;
    }
    Export *existing = pair.first->second.first;
    if (e == *existing || e.name != existing->name)
      continue;
    // If the existing export comes from .OBJ directives, we are allowed to
    // overwrite it with /DEF: or /EXPORT without any warning, as MSVC link.exe
    // does.
    // Also silently override exports from /export-all-symbols with ones from
    // def files and /EXPORT.
    int existingPriority = exportSourcePriority(existing->source);
    int newPriority = exportSourcePriority(e.source);
    if (existingPriority < newPriority) {
      // New definition with higher priority; don't warn, and replace the
      // existing definition with this one.
      *existing = e;
      v[pair.first->second.second] = e;
      continue;
    }
    if (existingPriority > newPriority) {
      // New definition with lower priority; ignore the new one silently
      // without warning.
      continue;
    }

    if (existing->source == e.source) {
      Warn(ctx) << "duplicate " << exportSourceName(existing->source)
                << " option: " << e.name;
    } else {
      Warn(ctx) << "duplicate export: " << e.name << " first seen in "
                << exportSourceName(existing->source) << ", now in "
                << exportSourceName(e.source);
    }
  }
  exports = std::move(v);

  // Sort by name.
  llvm::sort(exports, [](const Export &a, const Export &b) {
    return a.exportName < b.exportName;
  });
}

void SymbolTable::assignExportOrdinals() {
  // Assign unique ordinals if default (= 0).
  uint32_t max = 0;
  for (Export &e : exports)
    max = std::max(max, (uint32_t)e.ordinal);
  for (Export &e : exports)
    if (e.ordinal == 0)
      e.ordinal = ++max;
  if (max <= std::numeric_limits<uint16_t>::max())
    return;

  // Name the files that define the most exported symbols, which are where the
  // export set has to be narrowed.
  MapVector<InputFile *, uint32_t> counts;
  for (Export &e : exports)
    if (e.sym)
      ++counts[e.sym->getFile()];
  SmallVector<std::pair<InputFile *, uint32_t>, 0> byCount(counts.takeVector());
  llvm::stable_sort(byCount, [](const auto &a, const auto &b) {
    return a.second > b.second;
  });
  auto diag = Fatal(ctx);
  diag << "too many exported symbols (got " << max << ", max "
       << Twine(std::numeric_limits<uint16_t>::max()) << ")";
  constexpr size_t maxFiles = 10;
  for (const auto &[file, count] : ArrayRef(byCount).take_front(maxFiles))
    diag << "\n>>> " << count << " defined in " << file;
  if (byCount.size() > maxFiles)
    diag << "\n>>> and " << byCount.size() - maxFiles << " more files";
}

void SymbolTable::parseModuleDefs(StringRef path) {
  llvm::TimeTraceScope timeScope("Parse def file");
  std::unique_ptr<MemoryBuffer> mb =
      CHECK(MemoryBuffer::getFile(path, /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false,
                                  /*IsVolatile=*/true),
            "could not open " + path);
  COFFModuleDefinition m = check(parseCOFFModuleDefinition(
      mb->getMemBufferRef(), machine, ctx.config.mingw));

  // Include in /reproduce: output if applicable.
  ctx.driver.takeBuffer(std::move(mb));

  if (ctx.config.outputFile.empty())
    ctx.config.outputFile = std::string(saver().save(m.OutputFile));
  ctx.config.importName = std::string(saver().save(m.ImportName));
  if (m.ImageBase)
    ctx.config.imageBase = m.ImageBase;
  if (m.StackReserve)
    ctx.config.stackReserve = m.StackReserve;
  if (m.StackCommit)
    ctx.config.stackCommit = m.StackCommit;
  if (m.HeapReserve)
    ctx.config.heapReserve = m.HeapReserve;
  if (m.HeapCommit)
    ctx.config.heapCommit = m.HeapCommit;
  if (m.MajorImageVersion)
    ctx.config.majorImageVersion = m.MajorImageVersion;
  if (m.MinorImageVersion)
    ctx.config.minorImageVersion = m.MinorImageVersion;
  if (m.MajorOSVersion)
    ctx.config.majorOSVersion = m.MajorOSVersion;
  if (m.MinorOSVersion)
    ctx.config.minorOSVersion = m.MinorOSVersion;

  for (COFFShortExport e1 : m.Exports) {
    Export e2;
    // Renamed exports are parsed and set as "ExtName = Name". If Name has
    // the form "OtherDll.Func", it shouldn't be a normal exported
    // function but a forward to another DLL instead. This is supported
    // by both MS and GNU linkers.
    if (!e1.ExtName.empty() && e1.ExtName != e1.Name &&
        StringRef(e1.Name).contains('.')) {
      e2.name = saver().save(e1.ExtName);
      e2.forwardTo = saver().save(e1.Name);
    } else {
      e2.name = saver().save(e1.Name);
      e2.extName = saver().save(e1.ExtName);
    }
    e2.exportAs = saver().save(e1.ExportAs);
    e2.importName = saver().save(e1.ImportName);
    e2.ordinal = e1.Ordinal;
    e2.noname = e1.Noname;
    e2.data = e1.Data;
    e2.isPrivate = e1.Private;
    e2.constant = e1.Constant;
    e2.source = ExportSource::ModuleDefinition;
    exports.push_back(e2);
  }
}

// Parse a string of the form of "<from>=<to>".
void SymbolTable::parseAlternateName(StringRef s) {
  auto [from, to] = s.split('=');
  if (from.empty() || to.empty())
    Fatal(ctx) << "/alternatename: invalid argument: " << s;
  auto it = alternateNames.find(from);
  if (it != alternateNames.end() && it->second != to)
    Fatal(ctx) << "/alternatename: conflicts: " << s;
  alternateNames.insert(it, std::make_pair(from, to));
}

void SymbolTable::resolveAlternateNames() {
  // Add weak aliases. Weak aliases is a mechanism to give remaining
  // undefined symbols final chance to be resolved successfully.
  for (auto pair : alternateNames) {
    StringRef from = pair.first;
    StringRef to = pair.second;
    Symbol *sym = find(from);
    if (!sym)
      continue;
    if (auto *u = dyn_cast<Undefined>(sym)) {
      if (u->weakAlias) {
        // On ARM64EC, anti-dependency aliases are treated as undefined
        // symbols unless a demangled symbol aliases a defined one, which
        // is part of the implementation.
        if (!isEC() || !u->isAntiDep)
          continue;
        if (!isa<Undefined>(u->weakAlias) &&
            !isArm64ECMangledFunctionName(u->getName()))
          continue;
      }

      // Check if the destination symbol is defined. If not, skip it.
      // It may still be resolved later if more input files are added.
      // Also skip anti-dependency targets, as they can't be chained anyway.
      Symbol *toSym = find(to);
      if (!toSym)
        continue;
      auto toUndef = dyn_cast<Undefined>(toSym);
      if (toUndef && (!toUndef->weakAlias || toUndef->isAntiDep))
        continue;
      toSym->isUsedInRegularObj = true;
      if (toSym->isLazy())
        forceLazy(toSym);
      u->setWeakAlias(toSym);
    }
  }
}

// Parses /aligncomm option argument.
void SymbolTable::parseAligncomm(StringRef s) {
  auto [name, align] = s.split(',');
  if (name.empty() || align.empty()) {
    Err(ctx) << "/aligncomm: invalid argument: " << s;
    return;
  }
  int v;
  if (align.getAsInteger(0, v)) {
    Err(ctx) << "/aligncomm: invalid argument: " << s;
    return;
  }
  alignComm[std::string(name)] = std::max(alignComm[std::string(name)], 1 << v);
}

Symbol *SymbolTable::addUndefined(StringRef name) {
  return addUndefined(name, nullptr, false);
}

std::string SymbolTable::printSymbol(Symbol *sym) const {
  std::string name = maybeDemangleSymbol(ctx, sym->getName());
  if (ctx.hybridSymtab)
    return name + (isEC() ? " (EC symbol)" : " (native symbol)");
  return name;
}

void SymbolTable::compileBitcodeFiles() {
  if (bitcodeFileInstances.empty())
    return;

  // Collect the bitcode library functions that are not safe to call because
  // they were not yet brought in the link. (Such symbols are lazy.)
  llvm::BumpPtrAllocator alloc;
  llvm::StringSaver saver(alloc);
  SmallVector<StringRef> bitcodeLibFuncs;
  // Triple must be captured before the bitcode is moved into the compiler.
  // Note that the below assumes that the set of possible libfuncs is roughly
  // equivalent for all bitcode translation units.
  llvm::Triple tt =
      llvm::Triple(bitcodeFileInstances.front()->obj->getTargetTriple());
  for (StringRef libFunc : lto::LTO::getLibFuncSymbols(tt, saver)) {
    if (Symbol *sym = find(libFunc)) {
      if (auto *l = dyn_cast<LazyArchive>(sym)) {
        if (isBitcode(l->getMemberBuffer()))
          bitcodeLibFuncs.push_back(libFunc);
      } else if (auto *o = dyn_cast<LazyObject>(sym)) {
        if (isBitcode(o->file->mb))
          bitcodeLibFuncs.push_back(libFunc);
      }
    }
  }

  ScopedTimer t(ctx.ltoTimer);
  lto.reset(new BitcodeCompiler(ctx));
  lto->setBitcodeLibFuncs(bitcodeLibFuncs);
  {
    llvm::TimeTraceScope addScope("Add bitcode file instances");
    for (BitcodeFile *f : bitcodeFileInstances)
      lto->add(*f);
  }

  for (InputFile *newObj : lto->compile()) {
    ObjFile *obj = cast<ObjFile>(newObj);
    obj->ltoOutput = true;
    obj->parse();
    ctx.objFileInstances.push_back(obj);
  }
}

void SymbolTable::waitForLTOCleanup() {
  if (lto)
    lto->waitForLTOCleanup();
}

} // namespace lld::coff
