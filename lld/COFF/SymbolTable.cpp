//===- SymbolTable.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Binding.h"
#include "COFFLinkerContext.h"
#include "Config.h"
#include "Driver.h"
#include "LTO.h"
#include "PDB.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Timer.h"
#include "llvm/BinaryFormat/COFFBinding.h"
#include "llvm/ADT/BitVector.h"
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

  // A DLL exports a record of a thread-local variable in place of the
  // variable, which another image cannot reach by name.
  StringRef name = undefDiag.sym->getName();
  if (find(("__imp_" + name + "$tls").str()))
    diag << "\n>>> " << name
         << " is a thread-local variable of another image, which exports "
            "its record "
         << name
         << "$tls in its place; mark the declaration with default "
            "visibility so that the record is used";
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

    if (ctx.config.autoImport) {
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

bool SymbolTable::loadLocalImportMembers() {
  std::vector<Symbol *> lazies;
  for (auto &i : symMap) {
    Symbol *sym = i.second;
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef || undef->getWeakAlias())
      continue;
    StringRef name = sym->getName();
    if (!name.consume_front("__imp_"))
      continue;
    Symbol *l = find(name);
    if (!l || l->pendingArchiveLoad || !l->isLazy())
      continue;
    lazies.push_back(l);
  }
  for (Symbol *l : lazies) {
    Log(ctx) << "Loading lazy " << l->getName() << " from "
             << l->getFile()->getName() << " for __imp_" << l->getName();
    forceLazy(l);
  }
  return !lazies.empty();
}

Defined *SymbolTable::impSymbol(StringRef name) {
  if (name.starts_with("__imp_"))
    return nullptr;
  return dyn_cast_or_null<Defined>(find(("__imp_" + name).str()));
}

bool SymbolTable::isLocalImport(StringRef impName) {
  if (!ctx.config.importSlots || !impName.consume_front("__imp_"))
    return false;
  Symbol *s = find(impName);
  Defined *d = s ? s->getDefined() : nullptr;
  return d && !isa<DefinedImportData, DefinedImportThunk>(d);
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
              << imp->getName() << " from " << cast<DefinedRegular>(imp)->file
              << "; unexpected symbol type";
    return false;
  }
  sym->replaceKeepingName(imp, impSize);
  sym->isRuntimePseudoReloc = true;

  // Native import slots let an observable compiler cell keep its own address.
  // Classify the uses before coalescing it with the IAT.
  if (ctx.config.importSlots && isa<DefinedImportData>(imp))
    return true;

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
/// undefs. objFiles and bitcodeFiles (if not nullptr) are used to report
/// where undefined symbols are referenced.
void SymbolTable::reportProblemSymbols(
    const SmallPtrSetImpl<Symbol *> &undefs, bool needBitcodeFiles) {
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
    if (ctx.config.autoImport && impSymbol(name))
      continue;
    undefs.insert(sym);
  }

  reportProblemSymbols(undefs, true);
}

void SymbolTable::resolveRemainingUndefines(std::vector<Undefined *> &aliases) {
  llvm::TimeTraceScope timeScope("Resolve remaining undefined symbols");
  SmallPtrSet<Symbol *, 8> undefs;

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
        localImports.push_back(cast<DefinedLocalImport>(sym));
        continue;
      }
    }

    // We don't want to report missing Microsoft precompiled headers symbols.
    // A proper message will be emitted instead in PDBLinker::aquirePrecompObj
    if (name.contains("_PchSym_"))
      continue;

    if (ctx.config.autoImport && handleMinGWAutomaticImport(sym, name))
      continue;

    // Remaining undefined symbols are not fatal if /force is specified.
    // They are replaced with dummy defined symbols.
    if (ctx.config.forceUnresolved)
      replaceSymbol<DefinedAbsolute>(sym, ctx, name, 0);
    undefs.insert(sym);
  }

  reportProblemSymbols(undefs, false);
}

static bool compareBindingSymbols(Symbol *a, Symbol *b) {
  return a->getName() < b->getName();
}

static void collectCanonicalImports(SymbolTable &symtab,
                                    ArrayRef<BindingRequirement> bindings,
                                    SmallVectorImpl<Symbol *> &lazies) {
  for (const auto &binding : bindings) {
    if (!(binding.flags & COFF::BindingCanonical))
      continue;
    auto *def = dyn_cast<DefinedRegular>(getBindingTarget(binding.symbol));
    if (!def || !isReplaceableBinding(def))
      continue;
    for (Symbol *candidate : {static_cast<Symbol *>(def), binding.symbol}) {
      Symbol *imp = symtab.find(("__imp_" + candidate->getName()).str());
      if (imp && !imp->pendingArchiveLoad && imp->isLazy())
        lazies.push_back(imp);
    }
  }
}

bool SymbolTable::loadCanonicalImports() {
  SmallVector<Symbol *, 0> lazies;
  for (ObjFile *file : ctx.objFileInstances)
    if (&file->symtab == this)
      collectCanonicalImports(*this, file->getBindingRequirements(), lazies);
  for (BitcodeFile *file : bitcodeFileInstances)
    collectCanonicalImports(*this, file->getBindingRequirements(), lazies);
  llvm::sort(lazies, compareBindingSymbols);
  lazies.erase(std::unique(lazies.begin(), lazies.end()), lazies.end());
  for (Symbol *sym : lazies)
    forceLazy(sym);
  return !lazies.empty();
}

// External symbols have SymbolUnion storage; object-local symbols do not.
// Allocate only the local aliases that actually change kind, rather than
// growing every input symbol to accommodate an import. Pointer cells and
// references to their pointees retain distinct relocation semantics.
Symbol *makeImportAlias(DefinedRegular *symbol, DefinedImportData *import,
                        bool pointee) {
  bool root = symbol->isGCRoot;
  bool used = symbol->isUsedInRegularObj;
  Symbol *result = symbol;
  if (!symbol->getCOFFSymbol().isExternal())
    result = make<DefinedImportData>(symbol->getName(), import->file,
                                     import->location);
  result->replaceKeepingName(import, sizeof(DefinedImportData));
  result->isGCRoot = root;
  result->isUsedInRegularObj = used;
  result->isRuntimePseudoReloc = pointee;
  return result;
}

void SymbolTable::redirectSymbols(
    const DenseMap<Symbol *, Symbol *> &redirects) {
  if (redirects.empty())
    return;
  for (ObjFile *file : ctx.objFileInstances)
    if (owns(file))
      file->redirectSymbols(redirects);
  for (Symbol *&root : ctx.config.gcroot)
    if (Symbol *replacement = redirects.lookup(root))
      root = replacement;
  for (Export &exp : exports)
    if (Symbol *replacement = redirects.lookup(exp.sym))
      exp.sym = replacement;
  for (DefinedLocalImport *local : localImports)
    if (Symbol *replacement = redirects.lookup(local->wrappedSym))
      local->wrappedSym = cast<Defined>(replacement);
  for (const auto &entry : symMap)
    if (auto *undefined = dyn_cast<Undefined>(entry.second))
      if (Symbol *replacement = redirects.lookup(undefined->weakAlias))
        undefined->weakAlias = replacement;
  DenseMap<Symbol *, BindingEntity *> entities;
  for (const auto &entry : bindingEntities) {
    BindingEntity *entity = entry.second;
    if (Symbol *replacement = redirects.lookup(entity->symbol))
      entity->symbol = replacement;
    if (Symbol *replacement = redirects.lookup(entity->requirement))
      entity->requirement = replacement;
    for (Symbol *&requirement : entity->requirements)
      if (Symbol *replacement = redirects.lookup(requirement))
        requirement = replacement;
    entities.try_emplace(entity->symbol, entity);
  }
  bindingEntities = std::move(entities);
  DenseMap<Symbol *, BindingEntity *> requirements;
  for (const auto &entry : bindingRequirements) {
    Symbol *replacement = redirects.lookup(entry.first);
    requirements.try_emplace(replacement ? replacement : entry.first,
                             entry.second);
  }
  bindingRequirements = std::move(requirements);
}

// Canonical requirements use the ordinary symbol nodes, including conditional
// weak aliases. Unlike shared mutable COMDAT data, an immutable losing RTTI
// contribution may remain as dormant bytes. Its active references must all be
// redirected; a direct reference is never permission to keep a second identity.
void SymbolTable::bindCanonicalSymbols() {
  bool hasRTTIABI = false;
  for (ObjFile *file : ctx.objFileInstances)
    if (&file->symtab == this && file->hasRTTIABI()) {
      hasRTTIABI = true;
      break;
    }
  if (!hasRTTIABI)
    return;

  // The former extended representation carried no ABI marker. Reject its
  // unversioned descriptor definitions, including losing COMDAT definitions,
  // so selection or /force cannot hide a layout mismatch. C and MS ABI inputs
  // do not define Itanium RTTI names and need no new marker.
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this || file->hasRTTIABI())
      continue;
    COFFObjectFile *obj = file->getCOFFObj();
    for (const SymbolRef &sym : obj->symbols()) {
      if (obj->getCOFFSymbol(sym).getSectionNumber() <= 0)
        continue;
      StringRef name = cantFail(sym.getName());
      if (machine == I386)
        name.consume_front("_");
      if (name.starts_with("_ZTI"))
        Fatal(ctx) << toString(file) << ": unversioned RTTI definition " << name
                   << "; rebuild for the canonical RTTI ABI";
    }
  }

  SmallPtrSet<Symbol *, 32> required;
  DenseMap<Symbol *, uint32_t> kinds;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    for (const auto &binding : file->getBindingRequirements()) {
      Defined *def = binding.symbol->getDefined();
      if (binding.flags & COFF::BindingCanonical) {
        if (!def || isa<DefinedAbsolute>(def))
          Fatal(ctx) << toString(file) << ": canonical binding "
                     << binding.symbol->getName()
                     << " requires a real definition; /force cannot supply its "
                        "identity";
        required.insert(def);
      }
      if (!def)
        continue;
      uint32_t kind = binding.flags & (COFF::BindingRTTI | COFF::BindingName);
      auto [it, inserted] = kinds.try_emplace(def, kind);
      if (!inserted && it->second != kind)
        Fatal(ctx) << toString(file) << ": conflicting binding kinds for "
                   << binding.symbol->getName();
    }
  }

  SmallVector<Symbol *, 0> ordered(required.begin(), required.end());
  llvm::sort(ordered, compareBindingSymbols);
  DenseMap<ChunkAndOffset, SmallPtrSet<Symbol *, 4>> addressAliases;
  for (Symbol *sym : ordered)
    if (auto *def = dyn_cast<DefinedRegular>(sym))
      if (isReplaceableBinding(def) && def->getChunk()->live)
        addressAliases.try_emplace({def->getChunk(), def->getValue()});
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    for (Symbol *sym : file->getSymbols()) {
      auto *def = dyn_cast_or_null<DefinedRegular>(sym);
      if (!def)
        continue;
      auto it = addressAliases.find({def->getChunk(), def->getValue()});
      if (it != addressAliases.end())
        it->second.insert(sym);
    }
  }
  DenseMap<Symbol *, Symbol *> redirects;
  for (Symbol *sym : ordered) {
    auto *def = dyn_cast<DefinedRegular>(sym);
    if (!def || !isReplaceableBinding(def) || !def->getChunk()->live)
      continue;
    auto *imp = getBindingImport(sym);
    if (!imp)
      continue;
    if (isEC() || ctx.hybridSymtab || !ctx.config.importSlots)
      Fatal(ctx) << "canonical binding of " << sym->getName()
                 << " requires native import slots on a supported architecture";
    if (ctx.config.delayLoads.count(imp->getDLLName()))
      Fatal(ctx) << "canonical provider " << imp->getDLLName()
                 << " cannot be delay loaded";

    // Preserve auto-import's distinction between a pointer cell and its
    // pointee. Requirements follow local aliases that need a larger node.
    Log(ctx) << "Binding canonical " << sym->getName() << " to "
             << imp->getDLLName();
    imp->file->live = true;
    // COFF permits additional names (including section symbols) at the same
    // address. They are address aliases, not independent identities. Preserve
    // the canonical target through those names before replacing the leader.
    auto &aliases =
        addressAliases.find({def->getChunk(), def->getValue()})->second;
    aliases.erase(sym);
    canonicalResiduals.try_emplace(def->getChunk(), imp);
    for (Symbol *alias : aliases) {
      BindingEntity *entity = bindingRequirements.lookup(alias);
      if (!entity)
        entity = bindingEntities.lookup(alias);
      if (!entity || !(entity->flags & BindingCanonical))
        continue;
      DefinedImportData *other = getBindingImport(alias);
      if (!other || !sameImportedTarget(imp, other))
        Fatal(ctx) << "canonical address alias " << alias->getName()
                   << " has a different owner from " << sym->getName()
                   << "; the alias cannot discard either binding requirement";
    }
    for (Export &exp : exports) {
      if (!exp.forwardTo.empty() ||
          (exp.sym != sym && exp.name != sym->getName() &&
           (!exp.sym || exp.sym->getDefined() != sym) &&
           !aliases.contains(exp.sym)))
        continue;
      StringRef dll = imp->getDLLName();
      dll.consume_back_insensitive(".dll");
      exp.forwardTo = saver().save(dll + "." + imp->getExternalName());
    }
    redirects[sym] = makeImportAlias(def, imp, /*pointee=*/true);
    for (Symbol *alias : aliases)
      redirects[alias] = makeImportAlias(cast<DefinedRegular>(alias), imp,
                                         /*pointee=*/true);
  }
  redirectSymbols(redirects);
}

void SymbolTable::validateCanonicalResiduals() {
  if (canonicalResiduals.empty())
    return;
  // A symbol at an interior offset is not a same-address alias. Without an
  // exact export it cannot continue to expose the discarded local identity.
  // Perform this walk after all alias copies have resolved, and ignore debug
  // descriptions: dormant bytes are permitted, active bindings are not.
  for (ObjFile *file : ctx.objFileInstances) {
    if (!owns(file))
      continue;
    for (Chunk *chunk : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(chunk);
      if (!sc || !sc->live || sc->isCodeView() || sc->isDWARF())
        continue;
      // An entirely transferred immutable contribution is itself dormant.
      if (canonicalResiduals.contains(sc))
        continue;
      for (const coff_relocation &rel : sc->getRelocs()) {
        Symbol *symbol = file->getSymbol(rel.SymbolTableIndex);
        auto *def = symbol
                        ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                        : nullptr;
        if (!def || !def->data)
          continue;
        DefinedImportData *import = canonicalResiduals.lookup(def->getChunk());
        if (!import)
          continue;
        Fatal(ctx) << toString(file) << ": canonical reference through "
                   << symbol->getName() << " at offset " << rel.VirtualAddress
                   << " in " << sc->getSectionName()
                   << " still reaches local contribution offset "
                   << def->getValue() << "; selected provider is "
                   << import->getDLLName() << ":" << import->getExternalName()
                   << "; an exact native binding is required";
      }
    }
  }
  for (const Export &exp : exports) {
    if (!exp.forwardTo.empty())
      continue;
    Symbol *symbol = exp.sym ? exp.sym : find(exp.name);
    auto *def = symbol ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                       : nullptr;
    if (def && def->data && canonicalResiduals.contains(def->getChunk()))
      Fatal(ctx) << "export " << exp.name
                 << " exposes residual canonical storage through "
                 << symbol->getName()
                 << "; an exact native binding is required";
  }
}

// A stub is a candidate for binding to another image's copy of the variable
// when it is the only thing that reaches a COMDAT definition here.
DefinedRegular *getCompilerStubTarget(SymbolTable &symtab, Symbol *sym) {
  auto *d = dyn_cast<DefinedRegular>(sym);
  if (!d || !d->getName().starts_with(".refptr."))
    return nullptr;
  auto *sc = dyn_cast_or_null<SectionChunk>(d->getChunk());
  if (!sc || !sc->live || d->getValue() != 0 ||
      sc->getSize() != symtab.ctx.config.wordsize ||
      sc->getRelocs().size() != 1)
    return nullptr;
  Symbol *target = *sc->symbols().begin();
  return target ? dyn_cast<DefinedRegular>(getBindingTarget(target)) : nullptr;
}

// A COMDAT variable the compiler left preemptable -- an inline variable, a
// template static data member, a static local of an inline function, or the
// guard of one of those -- is reached through a stub, so that one image's copy
// can stand for every image's. Choosing a DLL's copy needs the import library
// member that holds it to be in the link first.
bool SymbolTable::loadSharedWeakImports() {
  std::vector<Symbol *> lazies;
  forEachSymbol([&](Symbol *sym) {
    StringRef name = sym->getName();
    if (!name.consume_front(".refptr.") || !isa<DefinedRegular>(sym))
      return;
    Symbol *imp = find(("__imp_" + name).str());
    if (imp && !imp->pendingArchiveLoad && imp->isLazy())
      lazies.push_back(imp);
  });
  for (BitcodeFile *file : bitcodeFileInstances) {
    if (!file->obj)
      continue;
    unsigned index = 0;
    for (const lto::InputFile::Symbol &symbol : file->obj->symbols()) {
      Symbol *requirement = file->getSymbols()[index++];
      if (!symbol.isCOFFImportCandidate())
        continue;
      auto *target = dyn_cast<DefinedRegular>(getBindingTarget(requirement));
      if (!target || !target->isCOMDAT)
        continue;
      for (Symbol *name : {static_cast<Symbol *>(target), requirement}) {
        Symbol *import = find(("__imp_" + name->getName()).str());
        if (import && !import->pendingArchiveLoad && import->isLazy())
          lazies.push_back(import);
      }
    }
  }
  llvm::sort(lazies, compareBindingSymbols);
  lazies.erase(std::unique(lazies.begin(), lazies.end()), lazies.end());
  for (Symbol *l : lazies) {
    Log(ctx) << "Loading lazy " << l->getName() << " from "
             << l->getFile()->getName() << " for a shared COMDAT variable";
    forceLazy(l);
  }
  return !lazies.empty();
}

// Binds such a stub to a DLL's copy when one is offered and nothing in this
// image needs the local one, so that the program holds a single instance of
// the variable, as it would on ELF. The local copy goes away with its
// initializer, which the compiler put in the same COMDAT, and an export of it
// from this image becomes a forwarder, so that anything importing it from here
// reaches the same instance.
void SymbolTable::bindSharedWeakData() {
  bindSharedWeakDataGroups();
  struct Candidate {
    Defined *stub;
    DefinedRegular *var;
    DefinedImportData *imp;
    bool keep = false;
  };
  SmallVector<Candidate, 0> candidates;
  forEachSymbol([&](Symbol *sym) {
    DefinedRegular *var = getCompilerStubTarget(*this, sym);
    if (!var)
      return;
    // The marked ABI binds the entire lifetime group above. Retain the
    // established conservative policy for other COFF inputs.
    if (auto *file = dyn_cast_or_null<ObjFile>(var->getFile()))
      if (file->hasRTTIABI())
        return;
    if (BindingEntity *entity = bindingEntities.lookup(var))
      if (entity->flags & BindingCanonical)
        return;
    auto *sc = dyn_cast_or_null<SectionChunk>(var->getChunk());
    if (!sc || !sc->live || !sc->isCOMDAT())
      return;
    auto *imp = dyn_cast_or_null<DefinedImportData>(
        find(("__imp_" + var->getName()).str()));
    if (imp)
      candidates.push_back({cast<Defined>(sym), var, imp});
  });
  if (candidates.empty())
    return;

  // The order decides nothing but diagnostics, and the symbol table is not
  // ordered.
  llvm::sort(candidates, [](const Candidate &a, const Candidate &b) {
    return a.var->getName() < b.var->getName();
  });

  // The variable's own COMDAT reaches it, and so does the stub; every other
  // reference is one this image cannot redirect, and keeps the local copy.
  DenseMap<SectionChunk *, unsigned> group;
  std::function<void(SectionChunk *, unsigned)> claim = [&](SectionChunk *sc,
                                                            unsigned i) {
    if (!group.insert({sc, i}).second)
      return;
    for (SectionChunk &c : sc->children())
      claim(&c, i);
  };
  for (auto [i, c] : llvm::enumerate(candidates)) {
    claim(cast<SectionChunk>(c.var->getChunk()), i);
    claim(cast<SectionChunk>(c.stub->getChunk()), i);
  }

  DenseMap<Symbol *, unsigned> vars;
  for (auto [i, c] : llvm::enumerate(candidates))
    vars.insert({c.var, i});
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != this)
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live || sc->isDWARF() || sc->isCodeView())
        continue;
      auto owner = group.find(sc);
      for (Symbol *b : sc->symbols()) {
        auto it = b ? vars.find(b) : vars.end();
        if (it != vars.end() &&
            (owner == group.end() || owner->second != it->second))
          candidates[it->second].keep = true;
      }
    }
  }

  std::function<void(SectionChunk *)> drop = [&](SectionChunk *sc) {
    if (!sc->live)
      return;
    sc->live = false;
    for (SectionChunk &c : sc->children())
      drop(&c);
  };

  for (Candidate &c : candidates) {
    Export *exp = nullptr;
    for (Export &e : exports)
      if (e.forwardTo.empty() &&
          (e.sym == c.var || e.name == c.var->getName()))
        exp = &e;
    // Anything else that asked for the variable by name wants this copy.
    if (c.keep || (llvm::is_contained(ctx.config.gcroot, c.var) && !exp))
      continue;

    Log(ctx) << "Binding " << c.var->getName() << " to the copy in "
             << c.imp->getDLLName();
    c.imp->file->live = true;
    cast<SectionChunk>(c.stub->getChunk())->live = false;
    c.stub->replaceKeepingName(c.imp, sizeof(DefinedImportData));
    drop(cast<SectionChunk>(c.var->getChunk()));
    if (exp) {
      StringRef dll = c.imp->getDLLName();
      dll.consume_back_insensitive(".dll");
      exp->forwardTo = saver().save(dll + "." + c.imp->getExternalName());
    }
  }
}

// Classify compiler cells separately from their pointees. Only recognized
// reads are redirected: exported addresses, section aliases and interior uses
// still name the original cell. This also permits local reads to become direct
// addresses when another use requires the cell to remain in the output.
void SymbolTable::bindCompilerStubs() {
  struct Cell {
    SectionChunk *chunk;
    Symbol *symbol;
    Defined *target;
    bool observable;
  };
  struct Use {
    SectionChunk *chunk;
    uint32_t relocation;
    unsigned cell;
  };
  SmallVector<Cell, 0> cells;
  DenseMap<SectionChunk *, unsigned> indices;
  for (const auto &entry : symMap) {
    auto *symbol = dyn_cast<DefinedRegular>(entry.second);
    if (!symbol || !symbol->data || symbol->getValue() ||
        !symbol->getName().starts_with(".refptr."))
      continue;
    SectionChunk *chunk = symbol->getChunk();
    if (!chunk->live || chunk->getSize() != ctx.config.wordsize ||
        chunk->getRelocs().size() != 1 || !chunk->children().empty() ||
        (chunk->getOutputCharacteristics() & permMask) != IMAGE_SCN_MEM_READ)
      continue;
    const coff_relocation &rel = chunk->getRelocs().front();
    if (rel.VirtualAddress ||
        chunk->getPointerAddend(rel) != std::optional<int64_t>(0))
      continue;
    Symbol *reference = chunk->file->getSymbol(rel.SymbolTableIndex);
    Defined *target = reference ? reference->getDefined() : nullptr;
    if (!isa_and_nonnull<DefinedRegular, DefinedCommon>(target)) {
      // An __imp_ reference names a pointer cell, not the imported object.
      // Only an auto-imported pointee can replace the value of this cell.
      auto *import = dyn_cast_or_null<DefinedImportData>(target);
      if (!import || !import->isRuntimePseudoReloc ||
          ctx.config.delayLoads.contains(import->getDLLName().lower()))
        continue;
    }
    auto [it, inserted] = indices.try_emplace(chunk, cells.size());
    if (inserted)
      cells.push_back({chunk, symbol, target, bool(symbol->isGCRoot)});
    else
      cells[it->second].observable |= symbol->isGCRoot;
  }
  if (cells.empty())
    return;

  for (const Export &exp : exports) {
    if (!exp.forwardTo.empty())
      continue;
    Symbol *symbol = exp.sym ? exp.sym : find(exp.name);
    auto *def = symbol ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                       : nullptr;
    if (def && def->data)
      if (auto it = indices.find(def->getChunk()); it != indices.end())
        cells[it->second].observable = true;
  }
  SmallVector<Use, 0> uses;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!owns(file))
      continue;
    for (Symbol *symbol : file->getSymbols()) {
      auto *def = dyn_cast_or_null<DefinedRegular>(symbol);
      if (def && def->data && def->isGCRoot)
        if (auto it = indices.find(def->getChunk()); it != indices.end())
          cells[it->second].observable = true;
    }
    for (Chunk *chunk : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(chunk);
      if (!sc || !sc->live || sc->isCodeView() || sc->isDWARF())
        continue;
      for (auto [i, rel] : enumerate(sc->getRelocs())) {
        Symbol *symbol = file->getSymbol(rel.SymbolTableIndex);
        auto *def = symbol
                        ? dyn_cast_or_null<DefinedRegular>(symbol->getDefined())
                        : nullptr;
        if (!def || !def->data)
          continue;
        auto it = indices.find(def->getChunk());
        if (it == indices.end())
          continue;
        SectionChunk::ImportRefForm form = sc->getImportRefForm(rel);
        bool pointerRead = form == SectionChunk::ImportRefForm::Load ||
                           form == SectionChunk::ImportRefForm::Call ||
                           form == SectionChunk::ImportRefForm::Jump ||
                           form == SectionChunk::ImportRefForm::PageBase ||
                           form == SectionChunk::ImportRefForm::PageOffset;
        if (def->getValue() || !pointerRead ||
            !(sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
          cells[it->second].observable = true;
        else
          uses.push_back({sc, uint32_t(i), it->second});
      }
    }
  }

  // Reuse range-thunk relocation rewriting: input symbols, addends, debug
  // coordinates and instruction sizes remain intact. Allocate a new symbol
  // index once per object/target, and copy only relocation arrays we change.
  DenseMap<Defined *, Symbol *> targets;
  DenseMap<std::pair<ObjFile *, Symbol *>, uint32_t> symbols;
  DenseMap<SectionChunk *, MutableArrayRef<coff_relocation>> replacements;
  for (const Use &use : uses) {
    Cell &cell = cells[use.cell];
    // ARM page-base and page-offset relocations must describe the same cell.
    // Keep the whole pair if an opaque use prevents proving that equivalence.
    if (machine == ARM64 && cell.observable)
      continue;
    Symbol *&target = targets[cell.target];
    if (!target) {
      if (auto *import = dyn_cast<DefinedImportData>(cell.target)) {
        target = import->file->impSym;
      } else {
        auto *local =
            make<DefinedLocalImport>(ctx, cell.symbol->getName(), cell.target);
        localImports.push_back(local);
        target = local;
      }
    }
    ObjFile *file = use.chunk->file;
    auto [it, inserted] = symbols.try_emplace({file, target}, 0);
    if (inserted)
      it->second = file->addSyntheticSymbol(target);
    MutableArrayRef<coff_relocation> &relocs = replacements[use.chunk];
    if (relocs.empty()) {
      ArrayRef<coff_relocation> original = use.chunk->getRelocs();
      relocs = MutableArrayRef<coff_relocation>(
          bAlloc().Allocate<coff_relocation>(original.size()), original.size());
      std::copy(original.begin(), original.end(), relocs.begin());
    }
    relocs[use.relocation].SymbolTableIndex = it->second;
  }
  for (const auto &entry : replacements)
    entry.first->setRelocs(entry.second);
  for (const Cell &cell : cells)
    if (!cell.observable)
      cell.chunk->live = false;
}

void SymbolTable::bindLocalImports() {
  llvm::TimeTraceScope timeScope("Bind local imports");
  // On ARM64EC an import pointer has two forms with distinct meanings; every
  // reference reads the pointer there, as before.
  bool rewrite = !isEC();
  SmallPtrSet<DefinedLocalImport *, 8> needPointer;
  auto readsPointer = [&](DefinedLocalImport *li, InputFile *file) {
    needPointer.insert(li);
    if (!ctx.config.warnLocallyDefinedImported ||
        li->getName().starts_with(".refptr."))
      return;
    Symbol *imp = li->wrappedSym;
    if (file)
      Warn(ctx) << file << ": locally defined symbol imported: "
                << printSymbol(imp) << " (defined in " << imp->getFile()
                << ") [LNK4217]";
    else
      Warn(ctx) << "<root>: locally defined symbol imported: "
                << printSymbol(imp) << " (defined in "
                << toString(imp->getFile()) << ") [LNK4217]";
  };

  for (DefinedLocalImport *li : localImports)
    if (li->isGCRoot)
      readsPointer(li, nullptr);

  // The address of a function in a delay-loaded DLL stays the thunk's: its
  // import entry holds the loader's stub until the first call, and every
  // other reference goes through the thunk.
  auto delayLoaded = [&](DefinedImportData *imp) {
    return ctx.config.delayLoads.contains(
        StringRef(imp->file->dllName).lower());
  };

  // An import thunk that every reference bypasses, because each one is an
  // in-place slot or an address-taking load rewritten to read the import
  // entry, is left out of the image, and with it out of the Control Flow Guard
  // address-taken table.
  SmallPtrSet<DefinedImportThunk *, 8> thunksNamed, thunksReached;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!owns(file))
      continue;
    // One warning per file and symbol, as before.
    SmallPtrSet<DefinedLocalImport *, 4> warned;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      for (const coff_relocation &rel : sc->getRelocs()) {
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s);
        if (thunk && rewrite && ctx.config.importSlots) {
          thunksNamed.insert(thunk);
          bool bypassed =
              !delayLoaded(thunk->wrappedSym) &&
              (sc->getImportSlotTarget(rel) ||
               sc->getImportRefForm(rel) == SectionChunk::ImportRefForm::Lea);
          if (!bypassed && !sc->isDWARF())
            thunksReached.insert(thunk);
        }
        if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s)) {
          SectionChunk::ImportRefForm form =
              rewrite ? sc->getImportRefForm(rel)
                      : SectionChunk::ImportRefForm::None;
          if (form != SectionChunk::ImportRefForm::None &&
              form != SectionChunk::ImportRefForm::Lea)
            continue;
          if (machine == ARM64)
            li->rewrite = false;
          if (warned.insert(li).second)
            readsPointer(li, file);
          continue;
        }
        // Static data that holds an imported address is a slot the loader
        // fills in place.
        if (DefinedImportData *imp = ctx.config.importSlots
                                         ? sc->getImportSlotTarget(rel)
                                         : nullptr) {
          if (!imp->inPlace && !delayLoaded(imp))
            imp->inPlace = true;
        } else if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s)) {
          DefinedImportData *imp = thunk->wrappedSym;
          if (rewrite && !imp->addressTaken &&
              sc->getImportRefForm(rel) == SectionChunk::ImportRefForm::Lea &&
              !delayLoaded(imp))
            imp->addressTaken = true;
        }
      }
    }
  }

  for (DefinedImportThunk *thunk : thunksNamed)
    if (!thunksReached.contains(thunk) && !thunk->isGCRoot)
      thunk->getChunk()->live = false;

  for (DefinedLocalImport *li : localImports) {
    if (!needPointer.contains(li))
      continue;
    li->getChunk()->live = true;
    localImportChunks.push_back(li->getChunk());
  }
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
    if (ctx.config.guardCF != GuardCFLevel::Off)
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
  if (wasInserted || (s->isLazy() && (overrideLazy || isLocalImport(name)))) {
    replaceSymbol<Undefined>(s, name);
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
  // An import-form reference to a symbol the link defines binds to that
  // definition; an import library's entry for it would only clash with the
  // definition under the plain name.
  if (isLocalImport(name))
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
  if (!u || (u->weakAlias && !u->isECAlias(machine)) || s->pendingArchiveLoad ||
      isLocalImport(n))
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
                                              ChunkAndOffset &location) {
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
  case ExportSource::Linker:
    return "linker-generated interface";
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
    if (existing->source == ExportSource::Directives) {
      *existing = e;
      v[pair.first->second.second] = e;
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

void SymbolTable::assignExportOrdinals(bool fillGaps) {
  if (fillGaps) {
    // A jointly finalized output set can allocate private ordinals densely
    // around explicitly fixed public ordinals, including ordinal 65535.
    BitVector used(1U << 16);
    used.set(0);
    for (const Export &e : exports)
      if (e.ordinal)
        used.set(e.ordinal);
    unsigned next = 1;
    for (Export &e : exports) {
      if (e.ordinal)
        continue;
      while (next < used.size() && used.test(next))
        ++next;
      if (next == used.size())
        Fatal(ctx) << "too many exported symbols (max 65535)";
      e.ordinal = next;
      used.set(next++);
    }
    return;
  }
  // Assign unique ordinals if default (= 0).
  uint32_t max = 0;
  for (Export &e : exports)
    max = std::max(max, (uint32_t)e.ordinal);
  for (Export &e : exports)
    if (e.ordinal == 0)
      e.ordinal = ++max;
  if (max > std::numeric_limits<uint16_t>::max())
    Fatal(ctx) << "too many exported symbols (got " << max << ", max "
               << Twine(std::numeric_limits<uint16_t>::max()) << ")";
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

bool SymbolTable::resolveAlternateNames() {
  bool changed = false;
  // Add weak aliases. Weak aliases is a mechanism to give remaining
  // undefined symbols final chance to be resolved successfully.
  for (auto pair : alternateNames) {
    StringRef from = pair.first;
    StringRef to = pair.second;
    Symbol *sym = find(from);
    // A name referenced in import form only takes the alias as well: the
    // import-form reference binds to what the name resolves to.
    if (!sym) {
      Symbol *imp = find(("__imp_" + from).str());
      if (!isa_and_nonnull<Undefined>(imp))
        continue;
      sym = addUndefined(from);
      sym->isUsedInRegularObj = imp->isUsedInRegularObj;
    }
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
      changed |= u->weakAlias != toSym;
      u->setWeakAlias(toSym);
    }
  }
  return changed;
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

void SymbolTable::prepareLTOBindings() {
  for (BitcodeFile *file : bitcodeFileInstances) {
    size_t index = 0;
    for (const lto::InputFile::Symbol &objSym : file->obj->symbols()) {
      Symbol *sym = file->getSymbols()[index++];
      Defined *target = dyn_cast<Defined>(getBindingTarget(sym));
      auto *regular = dyn_cast_or_null<DefinedRegular>(target);
      if (objSym.isExecutable() ||
          (!objSym.isWeak() && (!regular || !regular->isCOMDAT)))
        continue;
      StringRef name = target ? target->getName() : sym->getName();
      if (target ? getBindingImport(target) != nullptr
                 : find(("__imp_" + name).str()) != nullptr)
        ltoImportAlternatives.insert(sym);
    }
  }
}

void SymbolTable::compileBitcodeFiles() {
  if (bitcodeFileInstances.empty())
    return;

  prepareLTOBindings();
  ScopedTimer t(ctx.ltoTimer);
  lto.reset(new BitcodeCompiler(ctx));
  {
    llvm::TimeTraceScope addScope("Add bitcode file instances");
    lto->add(bitcodeFileInstances);
  }
  for (InputFile *newObj : lto->compile()) {
    ObjFile *obj = cast<ObjFile>(newObj);
    obj->parse();
    ctx.objFileInstances.push_back(obj);
  }
}

} // namespace lld::coff
