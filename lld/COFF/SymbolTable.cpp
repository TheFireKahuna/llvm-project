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
#include "Relocations.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Strings.h"
#include "lld/Common/Timer.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
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
#include "llvm/Support/xxhash.h"
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
// Addr. In an object LLVM made for Windows Itanium or NT-POSIX a label marks a
// point within one and names nothing.
static Symbol *getSymbol(SectionChunk *sc, uint32_t addr) {
  DefinedRegular *candidate = nullptr;

  for (Symbol *s : sc->file->getSymbols()) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (!d || !d->data || d->file != sc->file || d->getChunk() != sc ||
        d->getValue() > addr ||
        (sc->file->forkObject &&
         d->getCOFFSymbol().getStorageClass() == IMAGE_SYM_CLASS_LABEL) ||
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
  if (find(("__imp_" + name + "$tls").str()))
    diag << "\nNOTE: " << name << " is a thread-local variable of another "
         << "image, which is reached through its record " << name
         << "$tls: declare it dllimport or with default visibility.";
  // The record of a variable that the link defines is written from what a
  // definition states, and a hidden one states nothing.
  if (isTlsRecordPointerName(name))
    if (Symbol *var = find(name.slice(6, name.size() - 4));
        var && isa<Defined>(var))
      diag << "\nNOTE: the definition of " << printSymbol(var) << " in "
           << var->getFile() << " states no record: it is hidden there, or "
           << "of another producer; give the declaration its visibility.";
  // An MSVC DLL exports a thread-local only as its offset in the loader's
  // template, which an access adds to that DLL's entry of the TEB's array.
  StringRef var = isTlsRecordPointerName(name) ? name.slice(6, name.size() - 4)
                                               : name;
  if (Symbol *offset = find(("__imp___tls_offset_" + var).str()))
    diag << "\nNOTE: " << toString(offset->getFile()) << " offers " << var
         << " as a thread-local of the loader's template, through link.exe's "
         << "/experimental:tlsDllInterface (__tls_offset_" << var
         << "), which only foreign code can reach.";
  if (name.consume_front("__imp_")) {
    Symbol *imp = find(name);
    if (imp && imp->isLazy()) {
      diag << "\nNOTE: a relevant symbol '" << imp->getName()
           << "' is available in " << toString(imp->getFile())
           << " but cannot be used because it is not an import library.";
    }
  }
}

bool SymbolTable::loadMinGWSymbols() {
  bool loaded = false;
  for (Symbol *sym : getSymbols()) {
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

    // An undefined __imp_X that an object for Windows Itanium or NT-POSIX
    // refers to, and that no input provides, asks for the archive member
    // defining X, which binds to it through a local pointer.
    // A thread-local variable's record X$tls is of the member defining X.
    if (name.starts_with("__imp_")) {
      Symbol *l = sym->forkReferenced ? find(name.substr(6)) : nullptr;
      if (!l && sym->forkReferenced && isTlsRecordPointerName(name))
        l = find(name.slice(6, name.size() - 4));
      if (!l || l->pendingArchiveLoad || !l->isLazy())
        continue;
      Log(ctx) << "Loading lazy " << l->getName() << " from "
               << l->getFile()->getName() << " for " << name;
      forceLazy(l);
      loaded = true;
      continue;
    }

    if (ctx.config.autoImport || sym->forkReferenced) {
      // If we have an undefined symbol, but we have a lazy symbol we could
      // load, load it. Bitcode reaches a thread-local variable X that LTO
      // does not make the image's through __imp_X$tls.
      Symbol *l = find(("__imp_" + name).str());
      if (!l && sym->forkReferenced)
        l = find(("__imp_" + name + "$tls").str());
      if (!l || l->pendingArchiveLoad || !l->isLazy())
        continue;

      Log(ctx) << "Loading lazy " << l->getName() << " from "
               << l->getFile()->getName() << " for automatic import";
      forceLazy(l);
      loaded = true;
    }
  }

  // A vague entity that the link defines asks for the import library's
  // member that offers it, which no reference may name: bitcode refers to X
  // where its code will load __imp_X, and an object takes the address of a
  // function it defines by the function's own name.
  for (auto &[sym, entity] : vagueEntities) {
    DefinedRegular *d = getEntityDefinition(sym);
    if (!d || !isPreemptible(sym, entity.function) || replaces(entity))
      continue;
    for (Symbol *s : {sym, entity.function ? nullptr : findOwnGuard(d)}) {
      Symbol *l = s ? find(("__imp_" + s->getName()).str()) : nullptr;
      if (!l || l->pendingArchiveLoad || !l->isLazy())
        continue;
      Log(ctx) << "Loading lazy " << l->getName() << " from "
               << l->getFile()->getName() << " for its offer";
      forceLazy(l);
      loaded = true;
    }
  }
  return loaded;
}

// The name of the Itanium special name kind, _ZGV or _ZTH, of variable var.
static std::string getSpecialName(StringRef kind, StringRef var) {
  bool mangled = var.consume_front("_Z");
  return (kind + (mangled ? "" : Twine(var.size())) + var).str();
}

std::optional<std::string> getGuardName(StringRef var, uint64_t init) {
  if (init != LinkFactDynamicGuarded && init != LinkFactDynamicUnordered)
    return std::nullopt;
  return getSpecialName("_ZGV", var);
}

std::string describeShape(const COFFLinkImage::Entity &e) {
  if (e.Flags & LinkImageEntityFunction)
    return "a function";
  if (e.Flags & LinkImageEntityDescriptor)
    return "a type whose name and layout hash to " + utohexstr(e.CheckSum);
  return (Twine(e.Size) + " bytes aligned to " + Twine(1 << e.Log2Align) +
          (e.Flags >> LinkImageEntityInitShift & 2 ? " under a guard" : ""))
      .str();
}

// The guard variable of var where it has a COMDAT of its own, as another
// producer's object has it: it goes with its variable, which its name tells.
Symbol *SymbolTable::findOwnGuard(DefinedRegular *var) {
  if (var->file->forkObject)
    return nullptr;
  auto *guard = dyn_cast_or_null<DefinedRegular>(
      find(getSpecialName("_ZGV", var->getName())));
  return guard && guard->isCOMDAT && isa<ObjFile>(guard->file) ? guard
                                                               : nullptr;
}

Symbol *SymbolTable::findOfferedInit(Symbol *sym, const VagueEntity &entity) {
  if (entity.init != LinkFactTLSDynamic)
    return nullptr;
  return find(getSpecialName("_ZTH", sym->getName().drop_back(strlen("$tls"))));
}

DefinedRegular *SymbolTable::getEntityDefinition(Symbol *sym) {
  TlsRecordChunk *record = TlsRecordChunk::get(sym);
  return dyn_cast<DefinedRegular>(record ? record->var : sym);
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
  // reference itself to point at the IAT entry.
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
              << imp->getName() << " from " << imp->getFile()
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
/// undefs. objFiles and bitcodeFiles (if not nullptr) are used to report where
/// undefined symbols are referenced.
void SymbolTable::reportProblemSymbols(const SmallPtrSetImpl<Symbol *> &undefs,
                                       bool needBitcodeFiles) {
  // Return early if there is nothing to report (which should be
  // the common case).
  if (undefs.empty())
    return;

  for (Symbol *b : ctx.config.gcroot)
    if (undefs.contains(b))
      errorOrWarn(ctx) << "<root>: undefined symbol: " << printSymbol(b);

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
  for (Symbol *sym : getSymbols()) {
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
    // A sealed value is a word of the image's own, which the link makes once
    // the code of bitcode is generated.
    if (sym->forkReferenced &&
        (name.ends_with("$val") || name.ends_with("$init")))
      continue;
    // The link defines the bounds of the loader's template after the code of
    // bitcode is generated.
    if (is_contained({"_tls_start", "_tls_end"},
                     machine == I386 ? name.drop_front() : name))
      continue;
    if ((ctx.config.autoImport || sym->forkReferenced) &&
        (impSymbol(name) ||
         (sym->forkReferenced && impSymbol((name + "$tls").str()))))
      continue;
    if (tpoffWords.count(sym) && find(("__imp_" + name + "$tls").str()))
      continue;
    undefs.insert(sym);
  }

  reportProblemSymbols(undefs, /*needBitcodeFiles=*/true);
}

void SymbolTable::addStartStopSymbols() {
  MapVector<StringRef, SectionRun> runs;
  for (Symbol *sym : getSymbols()) {
    StringRef name = sym->getName();
    if (!isa<Undefined>(sym) || !sym->isUsedInRegularObj ||
        (machine == I386 && !name.consume_front("_")))
      continue;
    if (is_contained({"_etext", "etext", "_edata", "edata", "_end", "end"},
                     name)) {
      // A plain reference, or a weak one, which resolves to its zero default.
      auto *alias = cast<Undefined>(sym)->getWeakAlias();
      auto *abs = dyn_cast_or_null<DefinedAbsolute>(alias);
      if (alias && !(abs && abs->getVA() == 0))
        continue;
      if (!alias)
        replaceSymbol<DefinedSynthetic>(sym, sym->getName(), nullptr);
      boundarySymbols.push_back(sym);
      continue;
    }
    bool isStop = name.consume_front("__stop_");
    if ((isStop || name.consume_front("__start_")) &&
        isValidCIdentifier(name)) {
      SectionRun &run = runs[name];
      run.name = name;
      (isStop ? run.stop : run.start) = sym;
    }
  }
  if (runs.empty())
    return;

  for (Chunk *c : ctx.driver.getChunks())
    if (auto *sc = dyn_cast<SectionChunk>(c); sc && &sc->file->symtab == this)
      if (auto it = runs.find(sc->getSectionName().split('$').first);
          it != runs.end())
        it->second.chunks.push_back(sc);

  for (SectionRun &run : llvm::make_second_range(runs)) {
    // With no section to bound, the reference stays undefined.
    if (run.chunks.empty())
      continue;
    if (!run.start || !run.stop) {
      std::string other = (run.start ? "__stop_" : "__start_") + run.name.str();
      if (isa_and_nonnull<Defined>(find(mangle(other)))) {
        Err(ctx) << (run.start ? run.start : run.stop)->getName()
                 << " cannot be defined by the linker: " << mangle(other)
                 << " is defined by an input";
        continue;
      }
    }
    for (Symbol *sym : {run.start, run.stop})
      if (sym)
        replaceSymbol<DefinedSynthetic>(sym, sym->getName(), nullptr);
    // Folding a section of the run away would drop an entry.
    for (SectionChunk *sc : run.chunks)
      sc->keepDistinct = true;
    sectionRuns.push_back(std::move(run));
  }
}

void SymbolTable::resolveRemainingUndefines(std::vector<Undefined *> &aliases) {
  llvm::TimeTraceScope timeScope("Resolve remaining undefined symbols");
  SmallPtrSet<Symbol *, 8> undefs;
  SmallVector<DefinedLocalImport *, 0> localImports;
  SmallVector<Symbol *, 0> sealedValues;

  for (Symbol *sym : getSymbols()) {
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef)
      continue;
    if (!sym->isUsedInRegularObj)
      continue;

    StringRef name = undef->getName();

    // A weak alias may have been resolved, so check for that. A vague
    // variable's name that falls back to its default, as a type_info object
    // of a class that every unit saw incomplete falls back to its
    // placeholder, is no entity of the image's: the default is one under a
    // name of its own. A function's plain entry is its argument entry's.
    if (undef->getWeakAlias()) {
      if (auto it = vagueEntities.find(undef);
          it != vagueEntities.end() && !it->second.function)
        vagueEntities.erase(it);
      aliases.push_back(undef);
      continue;
    }

    // If we can resolve a symbol by removing __imp_ prefix, do that.
    // This odd rule is for compatibility with MSVC linker.
    if (name.starts_with("__imp_")) {
      // An object for Windows Itanium or NT-POSIX reads a sealed value,
      // __imp_X$val or __imp_X$init, as it reads a pointer of the image's own.
      // In the ntposix model __imp___tp_register$init is the image's
      // place word.
      bool ntposix = sym->forkReferenced && !ctx.tlsRuntime().empty();
      if (ntposix && name == COFF::TP::RegisterImport) {
        getPlaceWord();
        continue;
      }
      if (sym->forkReferenced &&
          (name.ends_with("$val") || name.ends_with("$init"))) {
        sealedValues.push_back(sym);
        continue;
      }
      auto findLocalSym = [&](StringRef n) {
        Symbol *sym = find(n);
        return sym ? sym->getDefined() : nullptr;
      };

      StringRef impName = name.substr(strlen("__imp_"));
      Defined *imp = findLocalSym(impName);
      // X$tp that nothing defines is X, where X is defined.
      if (!imp && sym->forkReferenced && impName.ends_with("$tp"))
        imp = findLocalSym(impName.drop_back(3));
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
      // A strong reference never binds to the default of a weak one.
      auto *x = dyn_cast_or_null<Undefined>(find(impName));
      bool weak = x && (x->weakKind == Symbol::NoLibrary ||
                        x->weakKind == Symbol::Library);
      if (sym->strongImport && weak &&
          (x->forkReferenced || sym->forkReferenced))
        imp = nullptr;
      // For an object for Windows Itanium or NT-POSIX, __imp_X of a weak
      // external X whose alternate is an import is that import's address
      // table entry, as X is its thunk or the variable it points to.
      auto *thunk = dyn_cast_or_null<DefinedImportThunk>(imp);
      Symbol *entry = thunk ? thunk->wrappedSym
                      : isa_and_nonnull<DefinedImportData>(imp)
                          ? impSymbol(imp->getName())
                          : nullptr;
      if (entry && sym->forkReferenced) {
        undef->setWeakAlias(entry);
        aliases.push_back(undef);
        continue;
      }
      if (imp) {
        replaceSymbol<DefinedLocalImport>(sym, ctx, name, imp);
        localImports.push_back(cast<DefinedLocalImport>(sym));
        localImports.back()->getChunk()->weakDefault = weak;
        continue;
      }
    }

    // We don't want to report missing Microsoft precompiled headers symbols.
    // A proper message will be emitted instead in PDBLinker::aquirePrecompObj
    if (name.contains("_PchSym_"))
      continue;

    if ((ctx.config.autoImport || sym->forkReferenced) &&
        handleMinGWAutomaticImport(sym, name))
      continue;

    // Another image's thread-local that GOTTPOFF relocations name is read
    // through the word that its record gives (below).
    if (tpoffWords.count(sym) && !ctx.tlsRuntime().empty() &&
        find(("__imp_" + name + "$tls").str()))
      continue;

    undefs.insert(sym);
  }

  // ELF's initial-exec: a GOTTPOFF relocation reads the word of its
  // thread-local's offset from the thread pointer, which the image's fill
  // writes from the record of another image whose import library offers
  // __tp_unit, or of the image's own. Where the link knows the image's place,
  // it writes the offset into the code instead, as ELF relaxes initial-exec to
  // local-exec.
  bool linkPlace =
      !ctx.config.dll || isa_and_nonnull<DefinedAbsolute>(find(COFF::TP::Place));
  for (auto &[x, word] : tpoffWords) {
    if (ctx.tlsRuntime().empty())
      break;
    StringRef name = x->getName();
    std::string rec = ("__imp_" + name + "$tls").str();
    if (isa<Defined>(x) ? linkPlace : !find(rec))
      continue;
    DefinedImportData *imp = isa<Defined>(x) ? nullptr : findImport(rec, name);
    if (imp && !ctx.driver.findImportMember(COFF::TP::Unit, imp->getDLLName()))
      Err(ctx) << "GOTTPOFF relocation against " << name
               << ", a thread-local of " << imp->getDLLName()
               << ", whose thread-locals are at no fixed offset from the "
                  "thread pointer; reach it through its record "
               << name << "$tls";
    else if (!imp && !TlsRecordChunk::get(find(rec.substr(6))))
      Err(ctx) << "GOTTPOFF relocation against " << name
               << ", a thread-local of the image, whose place the link does "
                  "not know and which has no record";
    else if (!word)
      word = addUndefined(saver().save("__imp_" + name + "$tpoff"));
    if (word && isa<Undefined>(word) && !llvm::is_contained(sealedValues, word))
      sealedValues.push_back(word);
  }

  // An owned-model importer of a record whose import library offers
  // __tp_unit has the record's $tpoff words in reserve, which the image
  // keeps where the scan relaxes a chain of the record to read them.
  for (ImportFile *f : ctx.tlsRuntime().empty()
                           ? ArrayRef<ImportFile *>()
                           : ArrayRef<ImportFile *>(ctx.importFileInstances)) {
    StringRef rec = f->impSym ? f->impSym->getName() : "";
    if (&f->symtab != this || !isTlsRecordPointerName(rec) ||
        !ctx.driver.findImportMember(COFF::TP::Unit, f->dllName))
      continue;
    for (StringRef word : {"$tpoff", "$guard$tpoff"})
      if (Symbol *s = addUndefined(saver().save(rec.drop_back(4) + word));
          isa<Undefined>(s) && !llvm::is_contained(sealedValues, s))
        sealedValues.push_back(s);
  }

  // A sealed value is a pointer that holds zero until the image's fill writes
  // it from import X, which is loaded for it; every $init comes before every
  // $val, and each kind is in the order of its names, but that a variable's
  // $tpoff words are a pair, its guard's second.
  auto order = [](Symbol *s) {
    StringRef name = s->getName();
    bool tpoff = name.consume_back("$tpoff");
    bool guard = tpoff && name.consume_back("$guard");
    return std::tuple(name.ends_with("$val"), name, tpoff, guard);
  };
  llvm::sort(sealedValues,
             [&](Symbol *a, Symbol *b) { return order(a) < order(b); });
  for (Symbol *sym : sealedValues) {
    StringRef name = sym->getName(), var = name;
    bool init = name.ends_with("$init");
    // __imp_X$tpoff, or __imp_X$guard$tpoff of X's guard, is computed from
    // the record that __imp_X$tls points at: an import, or a pointer to the
    // image's own record, which then stays. It holds no offset until then.
    bool tpoff = var.consume_back("$tpoff");
    bool guard = tpoff && var.consume_back("$guard");
    StringRef ptr =
        tpoff ? saver().save(var + "$tls") : name.drop_back(4 + init);
    Defined *source = findImport(ptr, "the word " + name);
    if (Symbol *rec = find(ptr.drop_front(6));
        tpoff && !source && TlsRecordChunk::get(rec)) {
      Symbol *p = insert(ptr).first;
      if (!isa<DefinedLocalImport>(p)) {
        replaceSymbol<DefinedLocalImport>(p, ctx, ptr, cast<Defined>(rec));
        localImports.push_back(cast<DefinedLocalImport>(p));
      }
      source = cast<DefinedLocalImport>(p);
      ctx.config.gcroot.push_back(rec);
    }
    if (!source) {
      undefs.insert(sym);
      continue;
    }
    replaceSymbol<DefinedLocalImport>(
        sym, ctx, name,
        make<DefinedAbsolute>(ctx, name, tpoff ? TP_UNFILLED : 0));
    LocalImportChunk *c = cast<DefinedLocalImport>(sym)->getChunk();
    c->filled = true;
    // An ARM64 adrp gives both words of the pair one page.
    if (tpoff && !guard)
      c->setAlignment(16);
    ctx.fill.words.push_back({c, 0, source, tpoff ? 8 + 4 * guard : 0,
                              tpoff  ? FillWord::TlsOffset
                              : init ? FillWord::Init
                                     : FillWord::Value});
  }

  for (auto &[x, word] : tpoffWords)
    if (auto *li = dyn_cast_or_null<DefinedLocalImport>(word))
      li->getChunk()->uses |= LocalImportChunk::Keep;

  // Lay the pointers out by name, so that reordering the inputs moves none.
  llvm::sort(localImports, [](DefinedLocalImport *a, DefinedLocalImport *b) {
    return a->getName() < b->getName();
  });
  for (DefinedLocalImport *li : localImports)
    localImportChunks.push_back(li->getChunk());

  // A DLL linked as ELF links a shared object imports what nothing defines
  // from the executable that loads it, by name: as a function where an object
  // refers to one, and as data otherwise.
  if (ctx.config.unresolvedHostImports && ctx.config.dll && !undefs.empty()) {
    DenseSet<StringRef> functions;
    for (ObjFile *file : ctx.objFileInstances)
      for (const SymbolRef &ref : file->getCOFFObj()->symbols())
        if (COFFSymbolRef s = file->getCOFFObj()->getCOFFSymbol(ref);
            s.isUndefined() && s.getComplexType() == IMAGE_SYM_DTYPE_FUNCTION)
          functions.insert(check(file->getCOFFObj()->getSymbolName(s)));
    StringSet<> imported;
    for (Symbol *sym : undefs) {
      StringRef name = sym->getName();
      name.consume_front("__imp_");
      if (imported.insert(name).second)
        ctx.driver.addFile(ImportFile::create(
            ctx, machine, name, "",
            functions.contains(name) ? IMPORT_CODE : IMPORT_DATA, IMPORT_NAME));
    }
    // Data that an object refers to directly is read through its pointer.
    undefs.remove_if([&](Symbol *sym) {
      return !isa<Undefined>(sym) ||
             handleMinGWAutomaticImport(sym, sym->getName());
    });
  }

  // Remaining undefined symbols are not fatal if /force is specified. They are
  // replaced with dummy defined symbols once none is left to automatically
  // import X, which would otherwise find __imp_X's dummy.
  if (ctx.config.forceUnresolved)
    for (Symbol *sym : undefs)
      replaceSymbol<DefinedAbsolute>(sym, ctx, sym->getName(), 0);

  reportProblemSymbols(undefs, /*needBitcodeFiles=*/false);
}

// Code refers to __imp_X from a dllimport declaration of X, which LNK4217
// reports when X is local; a .refptr cell or -wrap's pointer is no __imp_X,
// the default of a weak reference to X is no X, and an overridable default's
// check word is the program's choice.
void SymbolTable::reportLocalImport(StringRef where, DefinedLocalImport *li) {
  if (ctx.config.warnLocallyDefinedImported && !li->getChunk()->cell &&
      !li->getChunk()->weakDefault && !li->getChunk()->filled &&
      find(li->getName()) == li)
    Warn(ctx) << where << ": locally defined symbol imported: "
              << printSymbol(li->getTarget()) << " (defined in "
              << li->getTarget()->getFile() << ") [LNK4217]";
}

void SymbolTable::bindLocalImports() {
  // A live .refptr.X cell, which holds only X's address, binds as a local
  // import pointer to X when X is in the image, where a reference to it can
  // be rewritten: on ARM64, and on x86-64 the cells that objects for Windows
  // Itanium or NT-POSIX define or refer to.
  bool x64 = machine == AMD64 && ctx.hasForkObject;
  if (!ctx.hybridSymtab && (machine == ARM64 || x64))
    forEachSymbol([&](Symbol *s) {
      auto *d = dyn_cast<DefinedRegular>(s);
      if (!d || !d->getName().starts_with(".refptr."))
        return;
      SectionChunk *sc = d->getChunk();
      if (!sc->live || sc->getSize() != 8 || sc->getRelocs().size() != 1 ||
          !sc->children().empty() ||
          endian::read64le(sc->getContents().data()) ||
          (x64 && !sc->file->forkObject && !d->forkReferenced))
        return;
      const coff_relocation &rel = sc->getRelocs()[0];
      Symbol *x = sc->file->getSymbol(rel.SymbolTableIndex);
      Defined *target = x ? x->getDefined() : nullptr;
      if (rel.Type != (x64 ? IMAGE_REL_AMD64_ADDR64 : IMAGE_REL_ARM64_ADDR64) ||
          rel.VirtualAddress || !target ||
          x->getName() != d->getName().drop_front(8) ||
          isa<DefinedImportData, DefinedImportThunk, DefinedAbsolute>(target))
        return;
      replaceSymbol<DefinedLocalImport>(d, ctx, d->getName(), target);
      LocalImportChunk *c = cast<DefinedLocalImport>(d)->getChunk();
      c->cell = sc;
      cells[sc] = c;
      localImportChunks.push_back(c);
    });

  // Another name that reaches a cell's section keeps the cell.
  if (!cells.empty())
    for (ObjFile *file : ctx.objFileInstances)
      if (&file->symtab == this)
        for (Symbol *s : file->getSymbols())
          if (auto *d = dyn_cast_or_null<DefinedRegular>(s);
              d && d->getCOFFSymbol().isExternal())
            if (LocalImportChunk *c = cells.lookup(d->getChunk()))
              c->uses |= LocalImportChunk::Keep;
  for (Symbol *b : ctx.config.gcroot)
    if (auto *li = dyn_cast<DefinedLocalImport>(b)) {
      li->getChunk()->uses |= LocalImportChunk::Keep;
      reportLocalImport("<root>", li);
    }
}

std::pair<Symbol *, bool> SymbolTable::insert(StringRef name) {
  CachedHashStringRef key(name);
  auto [it, inserted] = symMap.try_emplace(key, symVector.size());
  if (!inserted)
    return {symVector[it->second], false};

  Symbol *sym = reinterpret_cast<Symbol *>(make<SymbolUnion>());
  sym->isUsedInRegularObj = false;
  sym->forkReferenced = false;
  sym->strongImport = false;
  sym->pendingArchiveLoad = false;
  sym->canInline = true;
  symVector.push_back(sym);
  if (isEC() && name.starts_with("EXP+"))
    expSymbols.push_back(sym);

  if (!archiveSymbols.empty()) {
    auto ai = archiveSymbols.find(key);
    if (ai != archiveSymbols.end()) {
      replaceSymbol<LazyArchive>(sym, ai->second.first, ai->second.second);
      return {sym, false};
    }
  }
  return {sym, true};
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

// X_$fo_bdd$ holds the binary decision diagram by which Windows selects a body
// for X as it maps the image, and X_$fo_rvas$ the candidates its leaves index.
// X becomes a stub that branches to the default body, a branch Windows
// rewrites to the selected one. In an object that LLVM made for Windows
// Itanium or NT-POSIX the function's stub is the compiler's and X stays the
// default body, which the object alone may define.
void SymbolTable::initializeFunctionOverrides() {
  for (SectionChunk *bdd : functionOverrideSections) {
    StringRef name = bdd->getSectionName();
    if (!name.consume_back("_$fo_bdd$"))
      continue;
    auto fail = [&](const Twine &msg) {
      Err(ctx) << bdd->file << ": function override " << name << ": " << msg;
    };
    bool fork = bdd->file->forkObject;
    Symbol *sym = nullptr;
    if (fork && bdd->sym && !bdd->sym->isExternal) {
      for (Symbol *s : bdd->file->getSymbols())
        if (auto *d = dyn_cast_or_null<DefinedRegular>(s);
            d && !d->isExternal && d->getName() == name)
          sym = d;
    } else {
      sym = find(name);
    }
    // An object may hold the records of functions the image does not have,
    // but a function defined other than in a section cannot take a stub.
    auto *x = dyn_cast_or_null<DefinedRegular>(sym);
    if (!x) {
      if (isa_and_nonnull<Defined>(sym))
        Warn(ctx) << bdd->file << ": function override " << name << ": " << name
                  << " is "
                  << (isa<DefinedImportThunk>(sym) ? "an import"
                      : isa<DefinedAbsolute>(sym)  ? "absolute"
                      : isa<DefinedCommon>(sym)    ? "a common symbol"
                                                   : "linker-defined")
                  << "; ignoring the record";
      continue;
    }
    MachineTypes machine = ctx.config.machine;
    if (machine != AMD64 && machine != ARM64) {
      fail("not supported on this machine");
      continue;
    }
    auto rvas = llvm::find_if(functionOverrideSections, [&](SectionChunk *c) {
      StringRef rvasName = c->getSectionName();
      return c->file == bdd->file && rvasName.consume_back("_$fo_rvas$") &&
             rvasName == name;
    });
    if (rvas == functionOverrideSections.end()) {
      fail("needs " + name + "_$fo_rvas$");
      continue;
    }
    std::vector<Defined *> candidates;
    for (Symbol *s : (*rvas)->symbols())
      candidates.push_back(dyn_cast_or_null<Defined>(s));
    if (llvm::is_contained(candidates, nullptr)) {
      fail("undefined candidate");
      continue;
    }

    // A node is the terminal, a leaf selecting a candidate, or a test whose
    // edges both lead forward, so that every walk ends.
    ArrayRef<uint8_t> diagram = bdd->getContents();
    auto *info = reinterpret_cast<const coff_bdd_info *>(diagram.data());
    if (diagram.size() < sizeof(*info) || info->Version != 1) {
      fail("unsupported diagram version");
      continue;
    }
    if (info->BDDSize % sizeof(coff_bdd_dynamic_relocation) ||
        info->BDDSize > diagram.size() - sizeof(*info)) {
      fail("invalid diagram size " + Twine(info->BDDSize));
      continue;
    }
    diagram = diagram.take_front(sizeof(*info) + info->BDDSize);
    ArrayRef<coff_bdd_dynamic_relocation> nodes(
        reinterpret_cast<const coff_bdd_dynamic_relocation *>(info + 1),
        info->BDDSize / sizeof(coff_bdd_dynamic_relocation));
    auto isValid = [&](size_t i) {
      uint16_t l = nodes[i].Left, r = nodes[i].Right;
      return (l == 0 && r == 0) ||
             (l == i && r == i && nodes[i].Value < candidates.size()) ||
             (l > i && r > i && l < nodes.size() && r < nodes.size());
    };
    size_t bad = 0;
    while (bad != nodes.size() && isValid(bad))
      ++bad;
    if (bad != nodes.size()) {
      fail("diagram node " + Twine(bad) +
           " has an edge that does not lead forward or an index out of range");
      continue;
    }

    auto *fo = make<FunctionOverride>();
    fo->diagram = diagram;
    fo->candidates = std::move(candidates);
    functionOverrides.push_back(fo);
    x->getChunk()->keepDistinct = true;
    if (fork) {
      fo->body = x;
      ctx.functionOverrideBodies[x] = fo;
      continue;
    }

    // The stub begins with a copy of X's KCFI prefix.
    fo->body = make<DefinedRegular>(*x);
    fo->stub = make<FunctionOverrideThunk>(*fo, getTypePrefix(x, name), machine,
                                           !ctx.config.doGC);
    if (x->getChunk()->sym == x)
      x->getChunk()->sym = fo->body;
    replaceSymbol<DefinedSynthetic>(x, x->getName(), fo->stub,
                                    fo->stub->getEntry());
  }
}

bool SymbolTable::replacedByOffer(const OwnDefault &o) const {
  return !ctx.config.dll && overrideAnchor && o.slot != VagueSlot &&
         isa_and_nonnull<DefinedImportData>(overrideAnchor->slots[o.slot]);
}

void SymbolTable::bindOverrideCells() {
  if (isEC() || ctx.hybridSymtab)
    return;
  auto add = [&](Defined *word, Chunk *c, uint32_t offset, unsigned slot,
                 CellUse use) {
    ctx.fill.words.push_back({c, offset, word,
                              int64_t(use | (use >= VagueCell ? 0 : slot << 5)),
                              FillWord::Cell});
  };
  // An executable has no cells but the words of a default that it defines
  // where a DLL's offer replaces it, which its fill writes from its own table.
  auto cells = [&](const OwnDefault &o) {
    return ctx.config.dll || replacedByOffer(o);
  };
  // The image's own defaults' words, in .sealed: the check words, then the
  // address words that a reference keeps or a word of data that holds the
  // default's address, which is computed from it unless the word's object
  // keeps its own addresses.
  for (auto &[at, o] : ownDefaults)
    if (o.check && cells(o)) {
      add(o.check, nullptr, 0, o.slot, CheckCell);
      llvm::erase(localImportChunks, o.check->getChunk());
    }
  for (ObjFile *file : ctx.objFileInstances) {
    if (!file->forkObject || ownDefaults.empty())
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live ||
          (sc->header->Characteristics &
           (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_DISCARDABLE)))
        continue;
      for (const coff_relocation &rel : sc->getRelocs())
        if (OwnDefault *o = findOwnDefault(sc->getRelocTarget(rel));
            o && cells(*o) &&
            rel.Type == (machine == AMD64 ? IMAGE_REL_AMD64_ADDR64
                                          : IMAGE_REL_ARM64_ADDR64) &&
            takesAddress(*sc, rel)) {
          o->address->getChunk()->uses |= LocalImportChunk::Keep;
          ctx.fill.words.push_back(
              {sc, rel.VirtualAddress, o->address,
               int64_t(endian::read64le(sc->getContents().data() +
                                        rel.VirtualAddress)),
               FillWord::Address});
        }
    }
  }
  for (auto &[at, o] : ownDefaults)
    if (cells(o) && o.address->getChunk()->isLive()) {
      llvm::erase(localImportChunks, o.address->getChunk());
      add(o.address, nullptr, 0, o.slot,
          o.slot == VagueSlot ? VagueCell : AddressCell);
    }
  if (!ctx.config.dll)
    return;

  // The name word of each type_info object that the DLL keeps preemptible,
  // the word at 8 of the object, which its section holds.
  for (auto &[sym, entity] : vagueEntities)
    if (DefinedRegular *d =
            entity.descriptor && offerTable ? getKeptEntity(sym) : nullptr;
        d && d->getChunk()->live)
      if (std::optional<COFFLinkImage::Entity> e =
              getImageEntity(sym, d, entity);
          e && (e->Flags & LinkImageEntityPreemptible))
        add(d, d->getChunk(), d->getValue() + 8, 0, NameCell);

  // The imports of defaults: the entry, which holds the default's address,
  // and the second import that the calls of one that forwards read. The
  // imports of a vague entity that its DLL keeps preemptible follow the
  // instance that the program uses, as that DLL does, and the offer table
  // names each entry with the entity's shape, which a fill that searches
  // compares with what it finds.
  auto vague = [&](ImportFile *f) -> const COFFLinkImage::Entity * {
    const COFFLinkImage::Entity *e =
        f->impSym ? ctx.driver.getOfferedEntity(f->impSym) : nullptr;
    return e && (e->Flags & LinkImageEntityPreemptible) &&
                   !isTlsRecordName(e->Name)
               ? e
               : nullptr;
  };
  // A word of thread-local data that holds one is no slot: the loader would
  // bind it in the template alone, after threads have copied it, and the
  // fill writes it in every thread from the import's entry, which is a cell.
  llvm::erase_if(ctx.importSlots, [&](const ImportSlot &s) {
    ImportFile *f = s.sym->file;
    if (f->binding || s.chunk->getSectionName().split('$').first != ".tls" ||
        (findOverrideSlot(f->externalName) < 0 && !vague(f)))
      return false;
    for (Relocation &r : s.chunk->getScanned()->relocs)
      if (r.expr == R_IMPORT_SLOT && r.offset == s.offset)
        r.expr = R_FILL_WORD;
    f->uses |= ImportFile::Keep;
    ctx.fill.words.push_back({s.chunk, s.offset, s.sym, 0, FillWord::Address});
    return true;
  });
  for (ImportFile *f : ctx.importFileInstances) {
    if (!f->live || f->binding || !f->impSym)
      continue;
    if (int slot = findOverrideSlot(f->externalName); slot >= 0) {
      if (f->uses & ~ImportFile::InPlace)
        add(f->impSym, nullptr, 0, slot,
            f->impSym->getName().contains(LinkOverrideCallPrefix)
                ? CallCell
                : AddressCell);
    } else if (const COFFLinkImage::Entity *e = vague(f)) {
      f->uses |= ImportFile::Keep;
      add(f->impSym, nullptr, 0, 0, VagueCell);
      if (offerTable)
        offerTable->entries.push_back(
            {f->impSym->getExternalName(), f->impSym,
             uint16_t(e->Flags | LinkImageEntityIndirect), e->Log2Align,
             e->Size});
    }
  }
  for (const ImportSlot &s : ctx.importSlots) {
    if (s.sym->file->binding)
      continue;
    if (int slot = findOverrideSlot(s.sym->file->externalName); slot >= 0)
      add(nullptr, s.chunk, s.offset, slot, AddressCell);
    else if (vague(s.sym->file))
      add(s.sym, s.chunk, s.offset, 0, VagueCell);
  }
}

int SymbolTable::findOverrideSlot(StringRef name) const {
  for (auto [slot, d] : enumerate(overrides.Slots))
    if (!d.Name.empty() && d.Name == name)
      return slot;
  return -1;
}

SymbolTable::OwnDefault *SymbolTable::findOwnDefault(Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedRegular>(s ? s->getDefined() : nullptr);
  if (!d || ownDefaults.empty())
    return nullptr;
  auto it = ownDefaults.find({d->getChunk(), d->getValue()});
  return it == ownDefaults.end() ? nullptr : &it->second;
}

ArrayRef<uint8_t> SymbolTable::getTypePrefix(DefinedRegular *sym,
                                             StringRef function) {
  std::string name = ("__cfi_" + function).str();
  for (Symbol *s : cast<ObjFile>(sym->getFile())->getSymbols())
    if (auto *cfi = dyn_cast_or_null<DefinedRegular>(s);
        cfi && cfi->getChunk() == sym->getChunk() &&
        cfi->getValue() <= sym->getValue() && cfi->getName() == name)
      return sym->getChunk()->getContents().slice(
          cfi->getValue(), sym->getValue() - cfi->getValue());
  return {};
}

bool SymbolTable::replaces(const VagueEntity &e) const {
  return e.overridable && !ctx.config.dll;
}

std::optional<uint32_t> SymbolTable::getTypeId(DefinedRegular *sym,
                                               Symbol *name, StringRef key) {
  // The patchable prefix nops that follow the ID, of 4 bytes each on ARM64.
  uint32_t nops = 0;
  if (!(sym->typePrefix || name->typePrefix) ||
      key.split('.').second.getAsInteger(10, nops))
    return std::nullopt;
  uint64_t at = nops * (machine == AMD64 ? 1 : 4) + 4;
  ArrayRef<uint8_t> c = sym->getChunk()->getContents();
  if (sym->getValue() < at || sym->getValue() > c.size())
    return std::nullopt;
  return endian::read32le(c.data() + sym->getValue() - at);
}

void SymbolTable::addOverrideDefault(unsigned slot,
                                     const COFFLinkOverrides::Default &d,
                                     StringRef key, const Twine &from) {
  bool typed = d.TypeId.has_value();
  for (auto [i, other] : enumerate(overrides.Slots)) {
    bool same = i == slot && other.Name == d.Name &&
                other.Forward == d.Forward && other.Drops == d.Drops &&
                (!typed || !other.TypeId || other.TypeId == d.TypeId);
    if (other.Name.empty() || same ||
        (i != slot && other.Name != d.Name &&
         (!typed || !other.TypeId || overrides.PrefixKey == key)))
      continue;
    Err(ctx) << from << ": overridable default " << d.Name << " of slot "
             << slot << " is not stated as " << other.Name << " of slot " << i
             << " is";
    return;
  }
  COFFLinkOverrides::Default &to = overrides.Slots[slot];
  if (!to.TypeId)
    to = d;
  if (typed)
    overrides.PrefixKey = key;
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
  if (f && f->forkObject)
    s->forkReferenced = true;
  if (wasInserted || (s->isLazy() && overrideLazy)) {
    // A strong reference to a weak external still finds the archive symbol.
    if (auto *l = dyn_cast<LazyArchive>(s))
      archiveSymbols.try_emplace(CachedHashStringRef(l->sym.getName()), l->file,
                                 l->sym);
    else if (auto *l = dyn_cast<LazyObject>(s))
      lazyObjects.try_emplace(CachedHashStringRef(l->getName()), l->file);
    replaceSymbol<Undefined>(s, name);
    return s;
  }
  if (s->isLazy())
    forceLazy(s);
  return s;
}

// A strong reference drops a NOLIBRARY or LIBRARY weak alias and loads the
// archive member or lazy object the weak external kept out, and makes an
// import that it names one the loader binds.
void SymbolTable::addStrongReference(Symbol *s) {
  if (auto *thunk = dyn_cast<DefinedImportThunk>(s))
    thunk->wrappedSym->file->binding = ImportFile::Loader;
  auto *u = dyn_cast<Undefined>(s);
  if (!u || !u->weakAlias ||
      (u->weakKind != Symbol::NoLibrary && u->weakKind != Symbol::Library))
    return;
  u->setWeakAlias(nullptr);
  if (s->pendingArchiveLoad)
    return;
  CachedHashStringRef key(s->getName());
  if (auto it = archiveSymbols.find(key); it != archiveSymbols.end()) {
    s->pendingArchiveLoad = true;
    it->second.first->addMember(it->second.second);
  } else if (InputFile *f = lazyObjects.lookup(key); f && f->lazy) {
    s->pendingArchiveLoad = true;
    f->lazy = false;
    ctx.driver.addFile(f);
  }
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
        u->setWeakAlias(t, Symbol::AntiDependency);
      }
    } else if (std::optional<std::string> demangledName =
                   getArm64ECDemangledFunctionName(name)) {
      Symbol *us = addUndefined(saver().save(*demangledName));
      auto u = dyn_cast<Undefined>(us);
      if (u && !u->weakAlias)
        u->setWeakAlias(b, Symbol::AntiDependency);
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
    return !u->weakAlias || u->isAntiDep();
  // If the symbol is lazy, allow it only if it originates from the same
  // archive.
  auto lazy = dyn_cast<T>(sym);
  return lazy && lazy->file == f;
}

// Returns whether the definition f offers for s is loaded. A weak alias keeps
// it out, except a LIBRARY one and an ARM64EC anti-dependency.
bool SymbolTable::loadsLazy(Symbol *s, InputFile *f) {
  auto *u = dyn_cast<Undefined>(s);
  if (!u || s->pendingArchiveLoad)
    return false;
  if (!u->weakAlias || u->isECAlias(machine) || u->weakKind == Symbol::Library)
    return true;
  if (u->weakKind == Symbol::WeakDefinition)
    warnWeakDefinition(s, f, u->weakAlias->getFile());
  return false;
}

// GNU ld loads an archive member that defines a weak definition's name.
void SymbolTable::warnWeakDefinition(Symbol *s, InputFile *lazy,
                                     InputFile *weak) {
  Warn(ctx) << lazy << " defines " << toString(ctx, *s)
            << ", whose weak definition in " << weak << " is used";
}

void SymbolTable::addLazyArchive(ArchiveFile *f, const Archive::Symbol &sym) {
  StringRef name = sym.getName();
  if (isEC() && !checkLazyECPair<LazyArchive>(this, name, f))
    return;
  CachedHashStringRef key(name);
  auto it = symMap.find(key);
  if (it == symMap.end()) {
    // Hybrid links check each lazy symbol against its counterpart as they
    // enter it, and before the machine is known the link may become one.
    if (ctx.config.machine == IMAGE_FILE_MACHINE_UNKNOWN || ctx.hybridSymtab)
      replaceSymbol<LazyArchive>(insert(name).first, f, sym);
    else
      archiveSymbols.try_emplace(key, f, sym);
    return;
  }
  Symbol *s = symVector[it->second];
  if (!loadsLazy(s, f)) {
    // A later strong reference to a weak external finds the member.
    if (isa<Undefined>(s))
      archiveSymbols.try_emplace(key, f, sym);
    return;
  }
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
  if (!loadsLazy(s, f)) {
    if (isa<Undefined>(s))
      lazyObjects.try_emplace(CachedHashStringRef(n), f);
    return;
  }
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
  if (!loadsLazy(s, f))
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

void SymbolTable::addTlsRecord(Symbol *var, Symbol *guard, InputFile *file) {
  TlsRecordChunk *&r = tlsRecords[var];
  if (!r) {
    StringRef name = saver().save(var->getName() + "$tls");
    auto [s, wasInserted] = insert(name);
    if (!wasInserted && !isa<Undefined>(s) && !s->isLazy()) {
      reportDuplicate(s, file);
      tlsRecords.pop_back();
      return;
    }
    r = make<TlsRecordChunk>(*this, s, var, file);
    replaceSymbol<DefinedSynthetic>(s, name, r);
  }
  // Every copy of the variable names one guard: a second, of a copy that
  // the link discarded, would be of an initialization that never runs.
  if (guard && r->guard && r->guard != guard)
    Err(ctx) << "thread-local variable " << printSymbol(var)
             << " has the guard " << printSymbol(r->guard) << " and in " << file
             << " the guard " << printSymbol(guard);
  else if (guard)
    r->guard = guard;
}

void SymbolTable::checkTlsTemplates() {
  for (StringRef name : {"_tls_start", "_tls_end"}) {
    Symbol *sym = findUnderscore(name);
    if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
        d && d->getFile()->tlsModel)
      Err(ctx) << d->getFile() << ": defines " << sym->getName()
               << ", the bound of the loader's thread-local template, which "
                  "the linker defines";
    else if (isa_and_nonnull<Undefined>(sym) &&
             !cast<Undefined>(sym)->getWeakAlias()) {
      Chunk *&c = ctx.tlsBounds[name == "_tls_end"];
      if (!c)
        c = make<EmptyChunk>();
      replaceSymbol<DefinedSynthetic>(sym, sym->getName(), c);
    }
  }
  // Each copy of a C selectany thread-local is in its object's template, so
  // copies of both models are an error whichever the link would keep.
  MapVector<DefinedRegular *, SetVector<InputFile *>> copies;
  for (auto [leader, file] : tlsCopies) {
    copies[leader].insert(leader->getFile());
    copies[leader].insert(file);
  }
  for (auto &[leader, files] : copies) {
    if (llvm::all_equal(
            llvm::map_range(files, [](InputFile *f) { return f->tlsModel; })))
      continue;
    COFFSyncStream e = Err(ctx);
    e << "thread-local " << toString(ctx, *leader)
      << " has copies of two models";
    for (bool model : {true, false}) {
      e << "\n>>> in the " << (model ? "image's" : "loader's") << " template:";
      for (InputFile *f : files)
        if (f->tlsModel == model)
          e << " " << f;
    }
  }
}

// The slot is an in-place import slot of the runtime's release, which only
// an import of it reaches: a -static runtime's callback is the release.
void SymbolTable::keepThreadEndSlot() {
  Symbol *end = findUnderscore("__wincrt_thread_end");
  auto *lazy = dyn_cast_or_null<LazyArchive>(end);
  if (ctx.config.dll ||
      !(isa_and_nonnull<DefinedImportThunk, DefinedImportData>(end) ||
        (lazy && identify_magic(lazy->getMemberBuffer().getBuffer()) ==
                     file_magic::coff_import_library)))
    return;
  // A callback is the image's own unless the slot's archive gives it.
  Symbol *slot = findUnderscore("__wincrt_thread_end_slot");
  StringRef rt;
  if (auto *l = dyn_cast_or_null<LazyArchive>(slot))
    rt = l->file->getName();
  else if (auto *d = dyn_cast_or_null<DefinedRegular>(slot))
    rt = d->getFile()->parentName;
  auto refers = [&](StringRef name) {
    Symbol *s = findUnderscore(name);
    return s && s->isUsedInRegularObj;
  };
  if (refers("__cxa_thread_atexit_impl") ||
      refers("__llvm_kcfi_cxa_thread_atexit_impl") ||
      llvm::any_of(ctx.objFileInstances, [&](ObjFile *f) {
        return &f->symtab == this && f->parentName != rt &&
               llvm::any_of(f->getChunks(), [](Chunk *c) {
                 auto *sc = dyn_cast<SectionChunk>(c);
                 return sc && sc->getSize() &&
                        sc->getSectionName().starts_with(".CRT$XL");
               });
      }))
    addGCRoot(mangle("__wincrt_thread_end_slot"));
}

TlsPlaceChunk *SymbolTable::getPlaceWord() {
  if (!placeWord) {
    placeWord = make<TlsPlaceChunk>(ctx);
    placeSym =
        dyn_cast<Defined>(addSynthetic(COFF::TP::RegisterImport, placeWord));
    keySym = make<DefinedSynthetic>(
        saver().save(Twine(COFF::TP::RegisterImport) + "+16"), placeWord,
        TP_KEY_AT);
  }
  return placeWord;
}

// An image of the owned model whose form is direct and which exports a record
// offers its importers $tpoff words: its import library says so by an export
// of __tp_unit, at its place word (spec section 15.1).
void SymbolTable::exportTlsUnit() {
  if (ctx.tlsRuntime().empty() || ctx.tlsWide ||
      llvm::none_of(exports,
                    [](const Export &e) { return TlsRecordChunk::get(e.sym); }))
    return;
  getPlaceWord();
  Export e;
  e.name = COFF::TP::Unit;
  e.data = true;
  e.sym = addSynthetic(e.name, placeWord);
  exports.push_back(e);
}

// An image of the ntposix model has a place word where it has thread-local
// data and is a DLL, its records name it, its template holds a word that the
// fill computes, or code names it. Its fill registers the image and writes
// the word, the first of .sealed, unless the image states its own place or
// is an executable whose template it does not write, whose word holds the
// executable's place from the start.
void SymbolTable::selectPlaceWord() {
  if (ctx.tlsRuntime().empty())
    return;
  auto isTls = [](Chunk *c) {
    auto *sc = dyn_cast_or_null<SectionChunk>(c);
    return sc && sc->live && sc->file->tlsModel &&
           sc->getSectionName().split('$').first == ".tls";
  };
  bool words = llvm::any_of(ctx.fill.words,
                            [&](const FillWord &w) { return isTls(w.chunk); });
  bool tls = ctx.config.dll && llvm::any_of(ctx.objFileInstances, [&](auto *f) {
               return llvm::any_of(f->getChunks(), isTls);
             });
  bool records = llvm::any_of(tlsRecords, [](auto &r) {
    return r.second->live && r.second->isNamed();
  });
  if (!placeWord && !words && !tls && !records && !ctx.tlsWide)
    return;
  getPlaceWord();
  if (isa_and_nonnull<DefinedAbsolute>(find(COFF::TP::Place)) ||
      (!ctx.config.dll && !words))
    return;
  DefinedImportData *imp = findImport(Twine("__imp_") + COFF::TP::Register,
                                      "the image's thread-local place");
  // An executable's words are reported where the fill selects its entry.
  if (!imp) {
    if (ctx.config.dll)
      Err(ctx) << "the image's thread-locals need its place, "
               << COFF::TP::RegisterImport << ", which ntposix.dll's "
               << COFF::TP::Register << " gives; link ntposix.lib";
    return;
  }
  placeWord->registers = true;
  imp->file->live = true;
  imp->file->uses |= ImportFile::Keep;
  ctx.fill.words.insert(ctx.fill.words.begin(),
                        {placeWord, 0, imp, 0, FillWord::Init});
  // A wide DLL's fill writes its key from the runtime's.
  if (!ctx.tlsWide || !ctx.config.dll)
    return;
  if (DefinedImportData *key =
          findImport(Twine("__imp_") + COFF::TP::Key, "the image's key")) {
    key->file->live = true;
    key->file->uses |= ImportFile::Keep;
    ctx.fill.words.push_back(
        {placeWord, TP_KEY_AT, key, 0, FillWord::Value});
  } else {
    Err(ctx) << "the image's wide thread-locals need its key, "
             << COFF::TP::KeyImport << ", which ntposix.dll's "
             << COFF::TP::Key << " gives; link ntposix.lib";
  }
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
  // Only an object for Windows Itanium or NT-POSIX states an export in the
  // name of a definition: X$so<K> or X$ap<K> for an address inside X, and
  // f@V or f@@V for a version of f.
  if (f->forkObject && n.find_first_of("$@") != StringRef::npos)
    namedExports.push_back(s);
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
                                              Chunk *&location) {
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

Symbol *SymbolTable::find(StringRef name) {
  CachedHashStringRef key(name);
  if (auto it = symMap.find(key); it != symMap.end())
    return symVector[it->second];
  // An archive's symbol enters under the archive's copy of the name, which
  // outlives the link.
  auto it = archiveSymbols.find(key);
  return it == archiveSymbols.end() ? nullptr : insert(it->first.val()).first;
}

DefinedImportData *SymbolTable::findImport(const Twine &name,
                                           const Twine &why) {
  Symbol *s = find(name.str());
  if (auto *l = dyn_cast_or_null<LazyArchive>(s); l && !s->pendingArchiveLoad) {
    MemoryBufferRef mb = l->getMemberBuffer();
    if (identify_magic(mb.getBuffer()) == file_magic::coff_import_library) {
      StringRef lib = l->file->getName();
      Log(ctx) << "Loading lazy " << s->getName() << " from " << lib << " for "
               << why;
      ctx.driver.addArchiveBuffer(mb, s->getName(), lib, 0, false);
    }
  }
  return dyn_cast_or_null<DefinedImportData>(s);
}

Symbol *SymbolTable::findUnderscore(StringRef name) {
  if (machine == I386)
    return find(("_" + name).str());
  return find(name);
}

std::vector<Symbol *>
SymbolTable::findAll(function_ref<bool(StringRef)> match) {
  for (auto &i : archiveSymbols)
    if (match(i.first.val()))
      insert(i.first.val());
  std::vector<Symbol *> syms;
  for (Symbol *sym : symVector)
    if (match(sym->getName()))
      syms.push_back(sym);
  return syms;
}

// Return all symbols that start with Prefix, possibly ignoring the first
// character of Prefix or the first character symbol.
std::vector<Symbol *> SymbolTable::getSymsWithPrefix(StringRef prefix) {
  return findAll([&](StringRef name) {
    return name.starts_with(prefix) || name.starts_with(prefix.drop_front()) ||
           name.drop_front().starts_with(prefix) ||
           name.drop_front().starts_with(prefix.drop_front());
  });
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

  for (Symbol *sym : findAll([&](StringRef name) { return pat->match(name); }))
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
// An import library's pointer for a name of a vague entity is the offer of
// the DLL that keeps the entity, which the image takes where it reads the
// pointer.
DefinedImportData *SymbolTable::getOfferOwner(StringRef name) {
  auto *imp =
      dyn_cast_or_null<DefinedImportData>(find(("__imp_" + name).str()));
  return imp && imp->file->live ? imp : nullptr;
}

// Another image's instance may stand for a default or a unique entity, but
// not for a default one that -Bsymbolic or -Bsymbolic-functions binds in a
// DLL. A vague entity is weak, so the non-weak kinds bind none, and a unique
// one stays one in the program under each.
bool SymbolTable::isPreemptible(const Symbol *sym, bool function) {
  BsymbolicKind kind =
      ctx.config.dll ? ctx.config.bsymbolic : BsymbolicKind::None;
  return sym->visibility == LinkFactUnique ||
         (sym->visibility == LinkFactDefault &&
          !isBoundBySymbolic(kind, kind == BsymbolicKind::All, function,
                             /*isWeak=*/true));
}

// A delay-loaded DLL's offer is not taken: its data has no delay-loaded form,
// and binding it would load the DLL when the image starts. Nor is that of a
// DLL that -ignore-offers names.
bool SymbolTable::honoursOffer(const ImportFile *file) {
  std::string dll = StringRef(file->dllName).lower();
  return !ctx.config.delayLoads.contains(dll) &&
         !ctx.config.ignoredOffers.contains(dll);
}

DefinedImportData *SymbolTable::findTakenOffer(StringRef name) {
  auto *imp =
      dyn_cast_or_null<DefinedImportData>(find(("__imp_" + name).str()));
  return imp && honoursOffer(imp->file) ? imp : nullptr;
}

bool SymbolTable::takesOffer(Symbol *sym, bool function) {
  return isPreemptible(sym, function) && findTakenOffer(sym->getName());
}

// An object of another producer states no facts, so its vague entities are
// those its symbols show: each external COMDAT definition that the link may
// choose among, by a name that takes part, a function only where its address
// matters to the object. The object's own copy is the one it reads where its
// code reaches a variable in a way that cannot follow the variable to
// another image, so the image keeps the entity as a protected one.
void SymbolTable::addForeignEntities(ObjFile *file) {
  std::optional<SmallPtrSet<Symbol *, 8>> taken = getTakenFunctions(ctx, file);
  // The variables of which the object has a copy, by their names and their
  // guards'.
  DenseMap<Symbol *, Symbol *> copies;
  ArrayRef<Symbol *> symbols = file->getSymbols();
  for (size_t i = 0, e = symbols.size(); i != e; ++i) {
    auto *d = dyn_cast_or_null<DefinedRegular>(symbols[i]);
    if (!d || !d->isCOMDAT || !isVagueEntityName(d->getName()))
      continue;
    COFFSymbolRef ref = check(file->getCOFFObj()->getSymbol(i));
    SectionChunk *c = d->getChunk();
    if (!c || ref.isUndefined() || !ref.isExternal() ||
        c->selection == IMAGE_COMDAT_SELECT_NODUPLICATES)
      continue;
    bool hidden =
        d->file == file && (ctx.driver.isLocalByVersionScript(d->getName()) ||
                            ctx.driver.isExcludedLib(file));
    // A thread-local variable has no address that another image could take,
    // and the object reaches its image's instance, which the image therefore
    // keeps. Another image shares it by the record that an object for Windows
    // Itanium or NT-POSIX states for it, in either order.
    if (c->getSectionName().starts_with(".tls")) {
      d->visibility = std::max<unsigned>(
          d->visibility, hidden ? LinkFactHidden : LinkFactProtected);
      if (TlsRecordChunk *r =
              tlsRecords.empty() ? nullptr : tlsRecords.lookup(d))
        r->sym->visibility = d->visibility;
      continue;
    }
    uint32_t chars = c->header->Characteristics;
    if (chars & IMAGE_SCN_CNT_CODE) {
      if (taken && !taken->count(d))
        continue;
      vagueEntities[d].function = true;
    } else {
      // A variable with a guard is initialized under it.
      Symbol *guard = findOwnGuard(d);
      VagueEntity &entity = vagueEntities[d];
      entity.init =
          std::max<unsigned>(entity.init, guard ? LinkFactDynamicGuarded
                                          : chars & IMAGE_SCN_MEM_WRITE
                                              ? LinkFactConstantMutable
                                              : LinkFactConstantImmutable);
      copies[d] = d;
      if (guard)
        copies[guard] = d;
    }
    d->visibility = std::max<unsigned>(d->visibility, hidden ? LinkFactHidden
                                                             : LinkFactDefault);
  }
  if (copies.empty())
    return;
  for (Chunk *chunk : file->getChunks())
    if (auto *sc = dyn_cast<SectionChunk>(chunk);
        sc && (sc->header->Characteristics & IMAGE_SCN_CNT_CODE))
      for (auto [s, rel] : zip(sc->symbols(), sc->getRelocs()))
        if (Symbol *var = copies.lookup(s);
            var && var->visibility < LinkFactProtected &&
            !takesAddress(*sc, rel))
          var->visibility = LinkFactProtected;
}

void SymbolTable::addForeignEntities() {
  if (ctx.hasForkObject)
    for (size_t e = ctx.objFileInstances.size(); foreignRead != e;) {
      ObjFile *file = ctx.objFileInstances[foreignRead++];
      if (!file->forkObject && &file->symtab == this)
        addForeignEntities(file);
    }
}

// These images bind symbols through thunks and a second import address
// table, which no offer's binding accounts for.
static void reportHybridEntity(COFFLinkerContext &ctx, InputFile *file,
                               Symbol *sym) {
  Err(ctx) << file << ": " << toString(ctx, *sym)
           << " is a vague entity, which an ARM64EC or ARM64X image cannot "
              "share with another; give it hidden visibility";
}

// The DLL's instance becomes the image's as an automatically imported
// variable does: each external symbol that the definition's COMDAT group
// defines, the guard and the interior names with the variable, becomes the
// import of its name, so that references are bound as any reference to an
// import is. The group's sections are dropped, not left to liveness, which
// -opt:noref would have keep the copy and run its initializer. A group moves
// whole or not at all: the image would otherwise initialize or guard an
// instance that is not the one its code reads. A function's copies are one
// by the one-definition rule, so its calls stay with the image's own and
// only what takes its address binds the DLL's, through the import that the
// entity names; liveness then keeps the copy while a call or jump is left.
// Of a thread-local variable's group, the records of the variable and of its
// guard become the DLL's and the two leave the image: the group's code,
// which initializes the variable in each thread, reaches both through the
// records and stays.
void SymbolTable::preemptVagueEntities() {
  // The groups that some DLL provides, by the file that defines them, and
  // the other names that one provides, which move only with their groups.
  MapVector<InputFile *, SmallVector<DefinedRegular *, 1>> leaders;
  SmallVector<Symbol *, 0> members;
  SmallPtrSet<Symbol *, 4> reported;
  for (auto &[sym, entity] : vagueEntities) {
    DefinedRegular *d = getEntityDefinition(sym);
    DefinedImportData *imp =
        d && isPreemptible(sym, entity.function) && !replaces(entity)
            ? findTakenOffer(sym->getName())
            : nullptr;
    if (!imp)
      continue;
    if (isEC() || ctx.hybridSymtab)
      return reportHybridEntity(ctx, d->file, sym);
    if (entity.function) {
      entity.owner = imp;
      sym->addressImported = true;
      // The definition may have taken the place of the import's thunk.
      if (imp->file->thunkSym == d)
        imp->file->thunkSym = nullptr;
      // A DLL's definition that would replace an overridable default is an
      // offer, which the program's executable takes or not. The DLL's own
      // calls reach what the program chose, through the default's import.
      if (entity.overridable && ctx.config.dll && !imp->file->thunkSym)
        imp->file->makeUnnamedThunk();
    } else if (isa<Undefined>(sym)) {
      // A weak external binds to the offer in place of its default, which is
      // an entity of its own.
      handleMinGWAutomaticImport(sym, sym->getName());
    } else if (d->isCOMDAT) {
      leaders[d->file].push_back(d);
    } else {
      members.push_back(sym);
    }
  }

  for (auto &[file, groupLeaders] : leaders) {
    // Every external symbol of the file's that a group defines, with the
    // group: for an object, by the group's sections, and for bitcode, by its
    // COMDAT.
    DenseMap<uintptr_t, size_t> groupOf;
    std::vector<SmallVector<Symbol *, 4>> groups(groupLeaders.size());
    auto *obj = dyn_cast<ObjFile>(file);
    auto *bitcode = dyn_cast<BitcodeFile>(file);
    if (!obj && !(bitcode && bitcode->obj))
      continue;
    for (size_t i = 0, e = groupLeaders.size(); obj && i != e; ++i) {
      SectionChunk *c = groupLeaders[i]->getChunk();
      groupOf[uintptr_t(c)] = i;
      for (SectionChunk &child : c->children())
        groupOf[uintptr_t(&child)] = i;
    }
    if (obj) {
      for (Symbol *s : obj->getSymbols())
        if (auto *d = dyn_cast_or_null<DefinedRegular>(s);
            d && d->file == file && d->isExternal)
          if (auto it = groupOf.find(uintptr_t(d->getChunk()));
              it != groupOf.end())
            groups[it->second].push_back(d);
    } else {
      for (bool collect : {false, true})
        for (auto [objSym, s] :
             zip(bitcode->obj->symbols(), bitcode->getSymbols())) {
          auto *d = dyn_cast<DefinedRegular>(s);
          if (!d || d->file != file || objSym.getComdatIndex() == -1)
            continue;
          if (collect) {
            if (auto it = groupOf.find(objSym.getComdatIndex());
                it != groupOf.end())
              groups[it->second].push_back(d);
          } else if (auto it = llvm::find(groupLeaders, d);
                     it != groupLeaders.end()) {
            groupOf[objSym.getComdatIndex()] = it - groupLeaders.begin();
          }
        }
    }

    for (size_t i = 0, e = groupLeaders.size(); i != e; ++i) {
      DefinedRegular *leader = groupLeaders[i];
      TlsRecordChunk *record = tlsRecords.lookup(leader);
      if (record) {
        SmallVector<Symbol *, 4> shared;
        for (Symbol *s : groups[i])
          if (TlsRecordChunk *r = tlsRecords.lookup(s))
            shared.push_back(r->sym);
        groups[i] = std::move(shared);
      }
      DefinedImportData *imp =
          findTakenOffer(record ? record->sym->getName() : leader->getName());
      auto *guard = cast_or_null<DefinedRegular>(findOwnGuard(leader));
      if (guard)
        groups[i].push_back(guard);
      // The copy and the instance that stands for it have one shape.
      const COFFLinkImage::Entity *theirs =
          obj ? ctx.driver.getOfferedEntity(imp) : nullptr;
      auto it = theirs ? vagueEntities.find(leader) : vagueEntities.end();
      if (std::optional<COFFLinkImage::Entity> ours =
              it != vagueEntities.end()
                  ? getImageEntity(leader, leader, it->second)
                  : std::nullopt;
          ours && !ours->sameShape(*theirs)) {
        reported.insert_range(groups[i]);
        Err(ctx) << file << ": " << toString(ctx, *leader) << " is "
                 << describeShape(*ours) << ", and " << describeShape(*theirs)
                 << " in " << imp->getDLLName()
                 << ", which provides it; the two are not one definition";
        continue;
      }
      if (protectedOwners.contains(leader))
        for (Symbol *s : groups[i])
          takeImport(s->getName(), imp->getDLLName(), /*code=*/false);
      bool whole = true;
      for (Symbol *s : groups[i]) {
        DefinedImportData *other = findTakenOffer(s->getName());
        if (other && other->getDLLName().equals_insensitive(imp->getDLLName()))
          continue;
        whole = false;
        reported.insert_range(groups[i]);
        Err(ctx) << file << ": " << toString(ctx, *leader) << " is provided by "
                 << imp->getDLLName() << ", but " << toString(ctx, *s)
                 << ", which its COMDAT group also defines, is "
                 << (other ? "provided by " + other->getDLLName().str() : "not")
                 << "; give both the visibility they have in "
                 << imp->getDLLName();
      }
      if (!whole)
        continue;
      // Bitcode does not say which loads only read a constant's value, which
      // the image may read in a copy of its own. LTO therefore compiles a
      // group of constants, with every reference in the form of an import,
      // and the group moves once its object states that. So does a
      // thread-local group, whose code LTO compiles.
      if (bitcode && (record || all_of(groups[i], [&](Symbol *s) {
            auto it = vagueEntities.find(s);
            return it != vagueEntities.end() && !it->second.function &&
                   it->second.init <= LinkFactConstantMutable;
          }))) {
        for (Symbol *s : groups[i])
          s->addressImported |= !record;
        reported.insert_range(groups[i]);
        continue;
      }
      // The names of a group that is one read-only section stay those of
      // the bytes that the image has of it, which are the DLL's where the
      // DLL states the checksum of a constant and it is this section's.
      if (SectionChunk *c = obj && !record ? leader->getChunk() : nullptr;
          c &&
          !(c->header->Characteristics &
            (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_CODE)) &&
          all_of(groups[i],
                 [&](Symbol *s) {
                   return cast<DefinedRegular>(s)->getChunk() == c;
                 }) &&
          theirs && (theirs->Flags & LinkImageEntityCheckSum) &&
          theirs->CheckSum == c->checksum)
        for (Symbol *s : groups[i]) {
          auto *copy = make<DefinedRegular>(*cast<DefinedRegular>(s));
          copy->isExternal = copy->isCOMDAT = false;
          DefinedImportData *name = findTakenOffer(s->getName());
          auto *li = make<DefinedLocalImport>(ctx, name->getName(), copy);
          valueCopies[name] = li;
          localImportChunks.push_back(li->getChunk());
        }
      for (DefinedRegular *d : {obj && !record ? leader : nullptr, guard}) {
        if (!d)
          continue;
        SectionChunk *c = d->getChunk();
        preempted.push_back(c);
        for (SectionChunk &child : c->children())
          preempted.push_back(&child);
      }
      for (Symbol *s : groups[i]) {
        if (TlsRecordChunk *r = TlsRecordChunk::get(s)) {
          r->live = false;
          preempted.push_back(cast<DefinedRegular>(r->var)->getChunk());
        }
        handleMinGWAutomaticImport(s, s->getName());
      }
    }
  }
  for (SectionChunk *c : preempted)
    c->live = false;

  for (Symbol *s : members)
    if (DefinedRegular *d = getEntityDefinition(s); d && !reported.count(s))
      Err(ctx) << d->file << ": " << toString(ctx, *s) << " is provided by "
               << findTakenOffer(s->getName())->getDLLName()
               << ", which does not provide the vague entity whose COMDAT "
                  "group defines it; give both the visibility they have in "
                  "that DLL";
}

void SymbolTable::takeImport(StringRef name, StringRef dll, bool code) {
  if (auto *imp =
          dyn_cast_or_null<DefinedImportData>(find(("__imp_" + name).str()))) {
    if (imp->getDLLName().equals_insensitive(dll))
      return;
    llvm::erase(ctx.importFileInstances, imp->file);
    replaceSymbol<Undefined>(imp, imp->getName());
  }
  ctx.driver.addFile(ImportFile::create(
      ctx, machine, name, dll, code ? IMPORT_CODE : IMPORT_DATA, IMPORT_NAME,
      "", ctx.driver.getImportHint(name, dll)));
}

void SymbolTable::bindValueOnlySites() {
  if (valueCopies.empty())
    return;
  // Whether each word of a copy holds what the DLL's does: the address of an
  // import that the loader binds at once, or of a function that a DLL offers,
  // which an in-place slot holds, or an absolute value.
  auto serves = [&](SectionChunk *c) {
    return all_of(zip(c->symbols(), c->getRelocs()), [&](const auto &word) {
      auto [s, rel] = word;
      if (rel.Type != (machine == AMD64 ? IMAGE_REL_AMD64_ADDR64
                                        : IMAGE_REL_ARM64_ADDR64) ||
          uint64_t(rel.VirtualAddress) + 8 > c->getSize() ||
          endian::read64le(c->getContents().data() + rel.VirtualAddress))
        return false;
      DefinedImportData *imp = nullptr;
      if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s))
        imp = thunk->wrappedSym;
      else if (auto *data = dyn_cast_or_null<DefinedImportData>(s))
        imp = data->isRuntimePseudoReloc ? data : nullptr;
      else if (s && s->addressImported)
        imp = vagueEntities.find(s)->second.owner;
      return isa_and_nonnull<DefinedAbsolute>(s) ||
             (imp && !ctx.config.delayLoads.contains(
                         StringRef(imp->file->dllName).lower()));
    });
  };
  // The sites that read each copy, or -1 for one that does not serve.
  MapVector<DefinedLocalImport *, int> sites;
  DenseMap<DefinedLocalImport *, DefinedLocalImport *> alone;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    DenseMap<DefinedLocalImport *, uint32_t> indices;
    for (const RelocFact &f : file->siteFacts) {
      auto *sc = const_cast<SectionChunk *>(f.chunk);
      ArrayRef<coff_relocation> rels = sc->getRelocs();
      DefinedLocalImport *li =
          f.fact == SiteFactValueOnly || f.fact == SiteFactValueOnlyAccess
              ? valueCopies.lookup(sc->getRelocTarget(rels[f.reloc]))
              : nullptr;
      if (!li)
        continue;
      SectionChunk *copy = cast<DefinedRegular>(li->getTarget())->getChunk();
      auto [it, isNew] = sites.try_emplace(li, -1);
      if (isNew && serves(copy)) {
        it->second = 0;
        // The copy has no name, and is live where a site reads it.
        SmallPtrSet<SectionChunk *, 2> kept = {copy};
        for (SectionChunk &child : copy->children())
          kept.insert(&child);
        llvm::erase_if(preempted,
                       [&](SectionChunk *c) { return kept.contains(c); });
        for (SectionChunk *c : kept)
          c->live = !ctx.config.doGC;
        copy->sym = nullptr;
      }
      if (it->second < 0)
        continue;
      // A site that is bound alone has a pointer apart, which no load
      // names: it must not take a register to hold what a load of the other
      // pointer left there.
      DefinedLocalImport *to = li;
      if (f.fact == SiteFactValueOnlyAccess) {
        DefinedLocalImport *&apart = alone[li];
        if (!apart) {
          apart = make<DefinedLocalImport>(ctx, li->getName(), li->getTarget());
          localImportChunks.push_back(apart->getChunk());
        }
        to = apart;
      }
      // The relocations are made to name the pointer as they are made to
      // name a range extension thunk.
      if (rels.data() ==
          file->getCOFFObj()->getRelocations(sc->header).data()) {
        auto *own = bAlloc().Allocate<coff_relocation>(rels.size());
        llvm::copy(rels, own);
        sc->setRelocs(ArrayRef(own, rels.size()));
        rels = sc->getRelocs();
      }
      auto [index, added] = indices.try_emplace(to);
      if (added)
        index->second = file->addRangeThunkSymbol(to);
      const_cast<coff_relocation &>(rels[f.reloc]).SymbolTableIndex =
          index->second;
      ++it->second;
    }
  }
  for (auto &[li, count] : sites)
    if (count > 0)
      Log(ctx) << "Keeping the copy of "
               << li->getName().substr(strlen("__imp_")) << " in "
               << li->getTarget()->getFile() << " for " << count << " sites";
}

DefinedRegular *SymbolTable::getKeptEntity(Symbol *sym) {
  DefinedRegular *d = getEntityDefinition(sym);
  SectionChunk *c = d ? d->getChunk() : nullptr;
  if (!c || !c->isCOMDAT() || !c->live || getOfferOwner(sym->getName()))
    return nullptr;
  return d;
}

bool SymbolTable::isAddition(Symbol *sym) const {
  return baseline && sym->visibility != LinkFactHidden &&
         !baseline->contains(sym->getName());
}

std::optional<COFFLinkImage::Entity>
SymbolTable::getImageEntity(Symbol *sym, DefinedRegular *d,
                            const VagueEntity &entity) {
  if (!d || d->getChunk()->sym != d)
    return std::nullopt;
  SectionChunk *c = d->getChunk();
  bool function = c->getOutputCharacteristics() & IMAGE_SCN_CNT_CODE;
  COFFLinkImage::Entity e;
  e.Name = sym->getName();
  if (entity.descriptor) {
    // A type_info object takes part where its name word holds the non-unique
    // bit, and the length of the blob that it names in bits 48 to 62, or the
    // saturated field and the blob's own length ahead of it.
    auto *name = dyn_cast_or_null<DefinedRegular>(
        find(("_ZTS" + e.Name.drop_front(4)).str()));
    ArrayRef<uint8_t> ti = c->getContents().drop_front(d->getValue());
    if (!name || !name->getChunk() || ti.size() < 16 || !(ti[15] & 0x80))
      return std::nullopt;
    ArrayRef<uint8_t> blob = name->getChunk()->getContents();
    uint64_t at = name->getValue(),
             size = endian::read16le(ti.data() + 14) & 0x7FFF;
    if (size == 0x7FFF && at >= 8)
      size = endian::read64le(blob.data() + at - 8);
    if (at > blob.size() || blob.size() - at < size)
      return std::nullopt;
    e.Flags = sym->visibility | LinkImageEntityDescriptor |
              (isAddition(sym) ? LinkImageEntityAddition : 0);
    if (ctx.config.dll && isPreemptible(sym, false))
      e.Flags |= LinkImageEntityPreemptible;
    e.Size = size;
    e.Log2Align = 0;
    e.CheckSum = xxh3_64bits(blob.slice(at, size));
    return e;
  }
  e.Flags = sym->visibility | entity.init << LinkImageEntityInitShift |
            (d->file->forkObject ? 0 : LinkImageEntityForeign) |
            (function ? LinkImageEntityFunction : 0);
  // Another image's instance may stand for a DLL's, which the DLL then
  // reaches through a cell, a thread-local variable's record apart.
  if (ctx.config.dll && isPreemptible(sym, function) && !entity.overridable)
    e.Flags |= LinkImageEntityPreemptible;
  if (isAddition(sym))
    e.Flags |= LinkImageEntityAddition;
  e.Size = function ? 0 : c->file->getComdatLength(c);
  e.Log2Align = function ? 0 : Log2_32(c->getAlignment());
  e.CheckSum = 0;
  if (!function && entity.init == LinkFactConstantImmutable) {
    e.Flags |= LinkImageEntityCheckSum;
    e.CheckSum = c->checksum;
  }
  return e;
}

bool SymbolTable::forwardExport(Export &e, Symbol &sym,
                                DefinedImportData *owner, StringRef what,
                                bool own) {
  // The loader takes what follows a forwarder's last dot as the name, or as
  // the ordinal after a number sign.
  StringRef name = own ? owner->getExternalName() : e.name;
  if (name.contains('.')) {
    Err(ctx) << owner->file << ": cannot forward the " << what << " of "
             << toString(ctx, sym) << ", which " << owner->getDLLName()
             << " exports as " << name << ", a name with a dot";
    return false;
  }
  if (name.empty())
    name = saver().save("#" + Twine(owner->getOrdinal()));
  e.forwardTo = saver().save(owner->getDLLName() + "." + name);
  if (own) {
    e.owner = owner->file;
    e.data = owner->file->hdr->getType() != IMPORT_CODE;
  }
  return true;
}

void SymbolTable::forwardImportedExports() {
  DenseSet<Symbol *> forwarded;
  for (Export &e : exports) {
    auto *u = dyn_cast_or_null<Undefined>(e.sym);
    auto *thunk = u && u->forkReferenced
                      ? dyn_cast_or_null<DefinedImportThunk>(u->getWeakAlias())
                      : nullptr;
    if (thunk && e.forwardTo.empty() &&
        forwardExport(e, *u, thunk->wrappedSym, "export")) {
      // An importer names the export, not the alternate that /export found.
      forwarded.insert(u);
      e.sym = nullptr;
      e.symbolName = "";
    }
  }
  // The export roots nothing: an import that only it named is not imported.
  if (!forwarded.empty())
    llvm::erase_if(ctx.config.gcroot,
                   [&](Symbol *s) { return forwarded.contains(s); });
}

void SymbolTable::addOffers() {
  DenseMap<StringRef, size_t> exported;
  for (size_t i = 0, e = exports.size(); i != e; ++i)
    exported.try_emplace(exports[i].name, i);

  // Offers sym, a name of the entity key: the entity's own, or its
  // initialization function, which the DLL that offers the entity offers by
  // its name.
  auto offer = [&](Symbol *sym, Symbol *key) {
    Export e;
    e.name = sym->getName();
    auto it = exported.find(e.name);
    // Being offered an entity makes no executable export it.
    if (!ctx.config.dll && it == exported.end())
      return;
    InputFile *file;
    if (DefinedImportData *owner = getOfferOwner(key->getName())) {
      // An importer imports the entity's own names from the owner, and the
      // initialization function from this DLL, which forwards it.
      file = owner->file;
      if (!forwardExport(e, *sym, owner, "offer", sym == key))
        return;
      // The image's own export of the name would be of a second instance.
      if (it != exported.end()) {
        exports[it->second].forwardTo = e.forwardTo;
        exports[it->second].owner = e.owner;
        exports[it->second].sym = nullptr;
        return;
      }
    } else {
      DefinedRegular *d = getKeptEntity(key);
      if (d && sym != key)
        d = dyn_cast<DefinedRegular>(sym);
      if (!d || it != exported.end())
        return;
      file = d->file;
      e.sym = cast<Defined>(sym);
      e.data =
          !(d->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE);
    }
    if (isEC() || ctx.hybridSymtab)
      return reportHybridEntity(ctx, file, sym);
    if (!ctx.driver.admitExport(*this, e, file))
      return;
    exports.push_back(e);
    // The offer of a thread-local variable is its record, which the image
    // then holds.
    if (TlsRecordChunk *record = TlsRecordChunk::get(sym)) {
      record->live = true;
      sym->isUsedInRegularObj = true;
    }
  };
  for (auto &[sym, entity] : vagueEntities) {
    if (sym->visibility == LinkFactHidden)
      continue;
    offer(sym, sym);
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (Symbol *guard = d && !entity.function ? findOwnGuard(d) : nullptr)
      offer(guard, guard);
    if (Symbol *init = findOfferedInit(sym, entity))
      offer(init, sym);

    // The table lists the entity and its guard. A DLL reaches one that it
    // keeps preemptible through a cell, its import pointer, which the fill
    // binds to the instance that the executable's table names; the table
    // names the cell.
    std::optional<COFFLinkImage::Entity> e;
    if (!offerTable || entity.overridable || TlsRecordChunk::get(sym) ||
        !(e = getImageEntity(sym, getKeptEntity(sym), entity)))
      continue;
    std::optional<std::string> guard =
        getGuardName(sym->getName(), entity.init);
    for (Symbol *s : {sym, guard ? find(*guard) : nullptr}) {
      auto *x = dyn_cast_or_null<DefinedRegular>(s);
      if (!x || !x->getChunk())
        continue;
      // Distinct functions have distinct addresses, which the table states.
      x->getChunk()->keepDistinct |= entity.function;
      Defined *at = x;
      uint16_t flags = e->Flags;
      // A type_info object has no cell: the fill binds its name word.
      if ((flags & LinkImageEntityPreemptible) && !entity.descriptor) {
        auto *cell =
            dyn_cast_or_null<DefinedLocalImport>(impSymbol(s->getName()));
        if (!cell || cell->getTarget() != x)
          cell = make<DefinedLocalImport>(
              ctx, saver().save("__imp_" + s->getName()), x);
        cell->getChunk()->filled = true;
        cell->getChunk()->uses |= LocalImportChunk::Keep;
        ownDefaults[{x->getChunk(), d ? d->getValue() : 0}] = {
            VagueSlot, nullptr, cell, nullptr};
        at = cell;
        flags |= LinkImageEntityIndirect;
      }
      offerTable->entries.push_back(
          {s->getName(), at, flags, e->Log2Align, e->Size});
    }
  }
}

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
    if (e == *existing)
      continue;
    if (e.name != existing->name) {
      // As in link.exe, the import library offers the symbol of an export
      // that takes another's name through EXPORTAS, importing that name, which
      // the export table holds once. A NONAME export names nothing, so beside
      // one, such an export is an export of its own.
      if (e.exportAs.empty() && existing->exportAs.empty())
        continue;
      if (e.noname || existing->noname) {
        v.push_back(e);
      } else if (!e.exportAs.empty()) {
        exportAsAliases.push_back(e);
      } else {
        exportAsAliases.push_back(*existing);
        *existing = e;
        v[pair.first->second.second] = e;
      }
      continue;
    }
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

// Exports what the names of the definitions added since the last call state.
//
// A definition f@V is version V of f and is exported under that name. f@@V is
// the default version: it is exported as f@V, the import library offers it as
// f, importing f@V, so that an importer keeps the version it was linked with,
// and f names it in the export table, privately, and in the image. PE binds
// each by name, so the two are ordinary exports.
//
// An export of X that the command line or a module-definition file states
// names no address inside X, where one that an object's directives state
// comes with the directives of X$so<K> and X$ap<K>. Those interior names are
// exported with each such X: under its export name, as data, and as private
// as it is.
void SymbolTable::addNamedExports() {
  DenseMap<StringRef, SmallVector<Symbol *, 2>> interiors;
  for (Symbol *s : namedExports) {
    auto *d = dyn_cast<DefinedRegular>(s);
    if (!d)
      continue;
    StringRef name = s->getName();
    if (name.contains('@')) {
      // A decorated name is no version: one of digits is a size in bytes.
      auto [plain, version] = name.split('@');
      bool isDefault = version.consume_front("@");
      if (plain.empty() || plain.starts_with("?") || version.empty() ||
          version.contains('@') || all_of(version, isDigit))
        continue;
      Export e;
      e.name = name;
      e.sym = addGCRoot(name);
      e.data =
          !(d->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE);
      e.source = ExportSource::Directives;
      if (isDefault) {
        e.extName = plain;
        e.isPrivate = true;
        exports.push_back(e);
        e.isPrivate = false;
        e.exportAs = saver().save(plain + "@" + version);
        auto it = alternateNames.try_emplace(plain, name).first;
        if (it->second != name)
          Err(ctx) << "duplicate default version of " << plain << ": "
                   << it->second << " and " << name;
        defaultVersions.emplace_back(plain, d);
      }
      exports.push_back(e);
      continue;
    }
    auto [base, offset] = name.rsplit('$');
    if (!base.empty() &&
        (offset.consume_front("so") || offset.consume_front("ap")) &&
        !offset.empty() && all_of(offset, isDigit))
      interiors[base].push_back(s);
  }
  namedExports.clear();
  // A default version is what its plain name binds in the image, as on ELF,
  // where another definition of that name is a duplicate.
  llvm::erase_if(defaultVersions, [&](auto &version) {
    auto *plain = dyn_cast_or_null<Defined>(find(version.first));
    if (plain)
      reportDuplicate(plain, version.second->getFile(),
                      version.second->getChunk(), version.second->getValue());
    return plain != nullptr;
  });
  if (interiors.empty())
    return;
  for (size_t i = 0, end = exports.size(); i != end; ++i) {
    if (exports[i].source == ExportSource::Directives ||
        !exports[i].forwardTo.empty())
      continue;
    StringRef base =
        exports[i].symbolName.empty() ? exports[i].name : exports[i].symbolName;
    auto it = interiors.find(base);
    if (it == interiors.end())
      continue;
    for (Symbol *s : it->second) {
      StringRef suffix = s->getName().substr(base.size());
      auto suffixed = [&](StringRef name) {
        return name.empty() ? name : saver().save(name + suffix);
      };
      Export e;
      e.name = e.symbolName = s->getName();
      e.extName = suffixed(exports[i].extName);
      e.exportAs = suffixed(exports[i].exportAs);
      e.sym = addGCRoot(e.name);
      e.noname = exports[i].noname;
      e.data = true;
      e.isPrivate = exports[i].isPrivate;
      e.source = exports[i].source;
      exports.push_back(e);
    }
  }
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
  auto byCount = counts.takeVector();
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
    // For an object for Windows Itanium or NT-POSIX, an undefined __imp_A
    // binds as a reference to A would: to __imp_B when B is an import, and
    // otherwise through a local pointer to A's alternate.
    if (!sym) {
      auto *imp = dyn_cast_or_null<Undefined>(find(("__imp_" + from).str()));
      if (!imp || !imp->forkReferenced || imp->weakAlias)
        continue;
      Symbol *impTo = find(("__imp_" + to).str());
      if (impTo && (impTo->isLazy() || isa<Defined>(impTo))) {
        impTo->isUsedInRegularObj = true;
        if (impTo->isLazy())
          forceLazy(impTo);
        imp->setWeakAlias(impTo);
        continue;
      }
      Symbol *toSym = find(to);
      if (!toSym || isa<Undefined>(toSym))
        continue;
      sym = addUndefined(from);
    }
    if (auto *u = dyn_cast<Undefined>(sym)) {
      if (u->weakAlias) {
        // On ARM64EC, anti-dependency aliases are treated as undefined
        // symbols unless a demangled symbol aliases a defined one, which
        // is part of the implementation.
        if (!isEC() || !u->isAntiDep())
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
      if (toUndef && (!toUndef->weakAlias || toUndef->isAntiDep()))
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
    obj->parse();
    ctx.objFileInstances.push_back(obj);
  }
  for (auto [sym, copy] : exactMatchCopies)
    if (auto *leader = dyn_cast<DefinedRegular>(sym))
      if (leader->getChunk()->getContents() != copy->getContents())
        reportDuplicate(leader, copy->file, copy);
}

void SymbolTable::waitForLTOCleanup() {
  if (lto)
    lto->waitForLTOCleanup();
}

} // namespace lld::coff
