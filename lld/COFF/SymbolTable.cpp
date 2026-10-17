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
#include "ImportSlots.h"
#include "LTO.h"
#include "LocalImports.h"
#include "OverrideTable.h"
#include "PDB.h"
#include "Symbols.h"
#include "VagueEntities.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Strings.h"
#include "lld/Common/Timer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Mangler.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/COFFModuleDefinition.h"
#include "llvm/Support/DataExtractor.h"
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
  } else if (Symbol *record = find(("__imp_" + name + "$tls").str())) {
    // A DLL exports a record of a thread-local variable in its place.
    diag << "\nNOTE: " << toString(record->getFile()) << " provides " << name
         << "$tls, the record of thread-local variable " << name
         << "; a declaration of " << name
         << " with default visibility reaches it";
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
  // Loading may bring in regular archive members, such as those of a GNU
  // import library, so the order is by name rather than by hash.
  llvm::sort(undefs, [](Symbol *a, Symbol *b) {
    return a->getName() < b->getName();
  });

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
  StringRef offset = name.substr(i);
  for (StringRef kind : interiorKinds)
    if (offset.consume_front(kind))
      return !offset.empty() && llvm::all_of(offset, isDigit)
                 ? name.take_front(i)
                 : StringRef();
  return {};
}

// Whether the lazy symbol s is offered by an import, an import library's member
// or a DLL's export, whose loading adds no reference to the link.
static bool isLazyImport(Symbol *s) {
  if (auto *a = dyn_cast<LazyArchive>(s))
    return identify_magic(a->getMemberBuffer().getBuffer()) ==
           file_magic::coff_import_library;
  return isa<LazyDLLSymbol>(s);
}

bool isShareableVagueName(StringRef name) {
  // Vtables are bound by other rules, a type_info object's name is reached
  // through the object, and data that MSVC mangles keeps the per-image copies
  // its code expects. A thread-local variable is shared through its record,
  // and its init function and wrapper stay per image.
  if (name.starts_with("?") || name.starts_with("."))
    return false;
  return !(name.size() > 3 && name.starts_with("_ZT") &&
           StringRef("VTCSHW").contains(name[3]));
}

bool isTypeInfoName(StringRef name) { return name.starts_with("_ZTI"); }

// Whether the lazy symbol s is an import library's offer of data or code, as
// type says.
static bool isLazyImportOf(Symbol *s, ImportType type) {
  auto *a = dyn_cast<LazyArchive>(s);
  if (!a || a->pendingArchiveLoad)
    return false;
  MemoryBufferRef mb = a->getMemberBuffer();
  if (identify_magic(mb.getBuffer()) != file_magic::coff_import_library)
    return false;
  auto *hdr = reinterpret_cast<const coff_import_header *>(mb.getBufferStart());
  return hdr->getType() == static_cast<int>(type);
}

static bool isLazyDataImport(Symbol *s) {
  return isLazyImportOf(s, IMPORT_DATA);
}

// Adds to functions the leaders of the COMDAT groups of functions with default
// visibility that file defines and whose addresses are significant: listed in
// its address-significance table, or any if it has none.
static void collectWeakFunctions(ObjFile *file,
                                 std::vector<Symbol *> &functions) {
  ArrayRef<Symbol *> syms = file->getSymbols();
  auto consider = [&](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (!d || d->getFile() != file || !d->isCOMDAT)
      return;
    SectionChunk *c = d->getChunk();
    if (!c || c->sym != d ||
        !(c->header->Characteristics & IMAGE_SCN_CNT_CODE) ||
        c->selection == IMAGE_COMDAT_SELECT_NODUPLICATES ||
        !isShareableVagueName(d->getName()) || !file->hasDefaultVisibility(d))
      return;
    functions.push_back(d);
  };
  if (!file->addrsigSec) {
    for (Symbol *s : syms)
      consider(s);
    return;
  }
  ArrayRef<uint8_t> contents;
  cantFail(file->getCOFFObj()->getSectionContents(file->addrsigSec, contents));
  DataExtractor data(contents, /*IsLittleEndian=*/true);
  DataExtractor::Cursor cur(0);
  while (cur && !data.eof(cur)) {
    uint64_t index = data.getULEB128(cur);
    if (cur && index < syms.size())
      consider(syms[index]);
  }
  // The table is checked where identical code folding reads it.
  consumeError(cur.takeError());
}

// The Itanium ABI's special name kind, such as GV, of the variable named
// name: kind followed by the variable's name, with the length prefix of a
// source name that the ABI leaves unmangled (_ZGV2iv for iv), built in buf
// for a lookup that keeps no copy of it.
static StringRef getSpecialName(StringRef kind, StringRef name,
                                SmallVectorImpl<char> &buf) {
  buf.clear();
  if (name.consume_front("_Z"))
    return ("_Z" + kind + name).toStringRef(buf);
  return ("_Z" + kind + Twine(name.size()) + name).toStringRef(buf);
}

static std::string getSpecialName(StringRef kind, StringRef name) {
  SmallString<128> buf;
  return getSpecialName(kind, name, buf).str();
}

std::string getGuardName(StringRef name) { return getSpecialName("GV", name); }

bool SymbolTable::isThreadLocal(Symbol *s) const {
  if (auto *d = dyn_cast_or_null<DefinedRegular>(s);
      d && isa<ObjFile>(d->getFile()))
    return d->getChunk()->getSectionName().starts_with(".tls");
  return threadLocalBitcodeData.contains(s);
}

StringRef SymbolTable::getRecordName(Symbol *s) {
  StringRef &name = recordNames[s];
  if (name.empty())
    name = saver().save(s->getName() + "$tls");
  return name;
}

StringRef SymbolTable::getSharedName(Symbol *s) {
  if (isThreadLocal(s))
    return getRecordName(s);
  return s->getName();
}

// The name under which the guard of the vague variable d is shared, which
// only lookups read, so that the link keeps no copy of it.
std::string SymbolTable::getSharedGuardName(Symbol *d) {
  std::string name = getGuardName(d->getName());
  if (isThreadLocal(d))
    name += "$tls";
  return name;
}

Symbol *SymbolTable::findVagueEntity(StringRef name) {
  Symbol *s = find(name);
  if (!isa_and_nonnull<Defined>(s) && name.ends_with("$tls"))
    if (Symbol *var = find(name.drop_back(strlen("$tls"))); isThreadLocal(var))
      return var;
  return s;
}

DefinedRegular *SymbolTable::getRecordVariable(Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedSynthetic>(s);
  auto *c =
      d ? dyn_cast_or_null<ThreadLocalRecordChunk>(d->getChunk()) : nullptr;
  return c ? cast<DefinedRegular>(c->var) : nullptr;
}

Defined *SymbolTable::getThreadLocalRecord(DefinedRegular *var) {
  StringRef name = getRecordName(var);
  if (auto *d = dyn_cast_or_null<Defined>(find(name)))
    return d;
  auto *index = dyn_cast_or_null<Defined>(findUnderscore("_tls_index"));
  if (!index) {
    Err(ctx) << "the record of thread-local variable " << printSymbol(var)
             << " needs the image's TLS index, _tls_index, which nothing "
                "defines";
    return nullptr;
  }
  // The byte that says on a thread whether the variable's initialization has
  // begun there, under the name the Itanium ABI gives the variable's guard. A
  // variable without one has no init function, which a reader that tests the
  // guard would call, and so is never tested; its record names the variable
  // itself.
  Defined *guard = var;
  if (auto *g = dyn_cast_or_null<Defined>(find(getGuardName(var->getName())))) {
    guard = g;
  } else if (isa_and_nonnull<Defined>(
                 find(getSpecialName("TH", var->getName())))) {
    Err(ctx) << var->getFile() << ": the record of thread-local variable "
             << printSymbol(var)
             << " needs the byte that guards its initialization, which "
                "nothing names "
             << getGuardName(var->getName());
    return nullptr;
  }
  auto *c = make<ThreadLocalRecordChunk>(ctx, index, var, guard);
  threadLocalRecords.push_back(c);
  return cast<Defined>(addSynthetic(name, c));
}

// Whether a data definition in a COMDAT has default visibility, so that a
// DLL's instance may preempt it, as on ELF.
static bool isDefaultVisibility(DefinedRegular *d) {
  auto *file = dyn_cast<ObjFile>(d->getFile());
  return !file || file->hasDefaultVisibility(d);
}

// Adds to chunks the sections of the COMDAT group that leader heads, and to
// members its external symbols, leader first.
static void collectComdatGroup(DefinedRegular *leader,
                               SmallVectorImpl<SectionChunk *> &chunks,
                               SmallVectorImpl<DefinedRegular *> &members) {
  size_t begin = chunks.size();
  SectionChunk *sc = leader->getChunk();
  chunks.push_back(sc);
  for (SectionChunk &child : sc->children())
    chunks.push_back(&child);
  ArrayRef<SectionChunk *> group = ArrayRef(chunks).drop_front(begin);
  members.push_back(leader);
  // Only the symbols that the object defines in the group's sections can be
  // its members, taken in the order of its symbol table.
  auto *file = cast<ObjFile>(leader->getFile());
  SmallVector<uint32_t, 8> indices;
  for (SectionChunk *c : group)
    llvm::append_range(indices,
                       file->getExternalDefinitions(c->getSectionNumber()));
  if (group.size() > 1)
    llvm::sort(indices);
  for (uint32_t i : indices)
    if (auto *d = dyn_cast_or_null<DefinedRegular>(file->getSymbol(i));
        d && d != leader && llvm::is_contained(group, d->getChunk()) &&
        d->getCOFFSymbol().isExternal())
      members.push_back(d);
}

// Adds to chunks and members the COMDAT group of the data definition d, and
// that of its guard where its group does not hold the guard, as clang emits
// it for COFF. Once liveness is decided, a guard the image discards is left.
void SymbolTable::collectWeakDataGroup(
    DefinedRegular *d, SmallVectorImpl<SectionChunk *> &chunks,
    SmallVectorImpl<DefinedRegular *> &members, bool liveOnly) {
  chunks.clear();
  members.clear();
  collectComdatGroup(d, chunks, members);
  SmallString<128> buf;
  auto *g = dyn_cast_or_null<DefinedRegular>(
      find(getSpecialName("GV", d->getName(), buf)));
  if (g && g->isCOMDAT && isa<ObjFile>(g->getFile()) &&
      (!liveOnly || g->getChunk()->live) && !llvm::is_contained(members, g))
    collectComdatGroup(g, chunks, members);
}

DefinedImportData *SymbolTable::findInteriorImport(DefinedImportData *imp,
                                                   int64_t offset) {
  if (offset <= 0)
    return nullptr;
  for (StringRef kind : interiorKinds) {
    auto *interior = dyn_cast_or_null<DefinedImportData>(
        find((imp->getName() + kind + Twine(offset)).str()));
    // Only the image that exports the object names addresses inside it.
    if (interior &&
        interior->getDLLName().equals_insensitive(imp->getDLLName()))
      return interior;
  }
  return nullptr;
}

// Whether a reference other than a weak one names the import of Name: one to
// Name that is not a weak external, or one to its import pointer from an
// object in which Name is not a weak external, as a declaration marked
// dllimport gives. A compiler reaches a weak Name through its import pointer
// in an object that keeps Name a weak external.
bool SymbolTable::isImportedStrongly(StringRef name) {
  if (Symbol *s = find(name); s && stronglyReferenced.contains(s))
    return true;
  readImportReferences();
  return strongImportPointers->contains(name);
}

// Reads which names the objects' weak externals name, and which import
// pointers a reference other than a weak one names.
void SymbolTable::readImportReferences() {
  if (!strongImportPointers) {
    strongImportPointers.emplace();
    weakExternalNames.emplace();
    functionNames.emplace();
    for (ObjFile *file : ctx.objFileInstances) {
      if (&file->symtab != this)
        continue;
      COFFObjectFile *obj = file->getCOFFObj();
      DenseSet<StringRef> weak;
      SmallVector<StringRef, 0> pointers;
      for (const SymbolRef &ref : obj->symbols()) {
        COFFSymbolRef sym = obj->getCOFFSymbol(ref);
        Expected<StringRef> symName = obj->getSymbolName(sym);
        if (!symName) {
          consumeError(symName.takeError());
          continue;
        }
        if (sym.isWeakExternal())
          weak.insert(*symName);
        else if (sym.isUndefined() && symName->starts_with("__imp_"))
          pointers.push_back(symName->drop_front(strlen("__imp_")));
        else if (sym.isUndefined() &&
                 sym.getComplexType() == IMAGE_SYM_DTYPE_FUNCTION)
          functionNames->insert(*symName);
      }
      for (StringRef p : pointers)
        if (!weak.contains(p))
          strongImportPointers->insert(p);
      weakExternalNames->insert(weak.begin(), weak.end());
    }
  }
}

// An ELF shared object may leave symbols for the executable that loads it to
// define. Under -unresolved-host-imports, a DLL imports each symbol nothing
// defines from the executable, which the fill binds by name against the
// process's own; a function declared as one is imported as code, anything
// else as data.
bool SymbolTable::importUnresolvedFromHost() {
  readImportReferences();
  DenseSet<StringRef> names;
  for (auto &[name, sym] : symMap) {
    auto *u = dyn_cast<Undefined>(sym);
    if (!u || u->getWeakAlias() || !sym->isUsedInRegularObj)
      continue;
    StringRef base = sym->getName();
    base.consume_front("__imp_");
    if (Symbol *b = find(base); b && !isa<Undefined>(b))
      continue;
    names.insert(base);
  }
  SmallVector<StringRef, 0> sorted(names.begin(), names.end());
  llvm::sort(sorted);
  for (StringRef name : sorted) {
    bool code = functionNames->contains(name) ||
                !isa_and_nonnull<Undefined>(find(name));
    Log(ctx) << "Importing " << name << " from the executable that loads the "
             << "image";
    ctx.driver.addFile(make<ImportFile>(
        ctx, makeShortImport(machine, "<host>.exe", saver().save(name),
                             code ? IMPORT_CODE : IMPORT_DATA, IMPORT_NAME)));
  }
  return !sorted.empty();
}

bool SymbolTable::loadInteriorImports() {
  if (ctx.hybridSymtab || isEC())
    return false;

  // An import from an executable is bound at run time against the process's
  // own, and one that only weak references name, as ELF binds a weak
  // undefined symbol, null where its DLL lacks it. Both need ntdll's resolver,
  // and the seal of their cells.
  for (ImportFile *f : ctx.importFileInstances) {
    if (&f->symtab != this || !f->impSym || f->isDelayLoaded())
      continue;
    if (ctx.config.dll && StringRef(f->dllName).ends_with_insensitive(".exe"))
      f->isHostImport = true;
    StringRef name = f->impSym->getName().drop_front(strlen("__imp_"));
    if (!weakExternalNames)
      readImportReferences();
    if (weakExternalNames->contains(name) && !isImportedStrongly(name))
      f->isWeakImport = true;
  }
  bool runTime = false, runTimeWeak = false;
  for (ImportFile *f : ctx.importFileInstances) {
    if (&f->symtab != this || !f->isBoundAtRunTime())
      continue;
    runTime = true;
    runTimeWeak |= !f->isHostImport;
  }
  std::vector<Symbol *> runTimeLazies;
  if (runTime) {
    StringRef names[] = {"__imp_LdrGetProcedureAddressForCaller",
                         "__imp_NtProtectVirtualMemory",
                         "__imp_LdrGetDllHandleEx"};
    for (StringRef name : ArrayRef(names).take_front(runTimeWeak ? 3 : 2))
      if (Symbol *l = find(name);
          l && l->isLazy() && !l->pendingArchiveLoad && isLazyImport(l))
        runTimeLazies.push_back(l);
  }
  bool canSeal = machine == AMD64 || machine == ARM64;
  // A DLL that reaches vague entities through cells binds them, when loaded
  // outside the executable's link, by a search of the loaded modules, which
  // pins the module it binds to and writes the cells between two protection
  // calls: those it keeps preemptible, and those the DLLs it imports from do.
  if (canSeal && ctx.config.dll && ctx.config.importSlots) {
    isPreemptibleVagueEntity(nullptr); // which computes the set
    bool cells = !preemptibleVagueEntities->empty();
    if (!cells && isa_and_nonnull<Defined>(findUnderscore("__xi_a"))) {
      StringMap<StringSet<>> offers = getPreemptibleOffers(ctx, false);
      cells = !offers.empty() &&
              llvm::any_of(ctx.importFileInstances, [&](ImportFile *f) {
                auto it = offers.find(StringRef(f->dllName).lower());
                return &f->symtab == this && f->impSym && it != offers.end() &&
                       it->second.contains(
                           f->impSym->getName().drop_front(strlen("__imp_")));
              });
    }
    if (cells)
      for (StringRef name :
           {"__imp_LdrAddRefDll", "__imp_NtProtectVirtualMemory"})
        if (Symbol *l = find(name);
            l && l->isLazy() && !l->pendingArchiveLoad && isLazyImport(l))
          runTimeLazies.push_back(l);
  }
  std::vector<Symbol *> lazies;
  bool sealed = false;
  // Nothing that loads between calls adds an object or an archive, so a word
  // of an object that an earlier call walked names and takes what it did
  // then, unless a name it was to take was still to be loaded, or a member
  // loaded since defines a symbol that an object references. Such a word
  // loads nothing more, and what it seals mattered only then: had any walk
  // sealed the image, NtProtectVirtualMemory's import was loaded then.
  size_t first = interiorImportsRewalk ? 0 : interiorImportObjects;
  interiorImportObjects = ctx.objFileInstances.size();
  interiorImportsRewalk = false;
  // Loads the names that the DLL providing an import may export for the
  // address that the word at rel in sc holds inside the import's object, and
  // finds whether the word is read-only and takes none. Returns whether a name
  // it may take is still to be loaded.
  auto walkWord = [&](SectionChunk *sc, const coff_relocation &rel) {
    Symbol *s = sc->file->getSymbol(rel.SymbolTableIndex);
    DefinedImportData *imp = getInPlaceImport(s);
    if (!imp || !sc->isAddressWord(rel))
      return false;
    int64_t addend = getAddressWordAddend(sc, rel).value_or(0);
    if (!addend)
      return false;
    bool pending = false;
    if (addend > 0)
      for (StringRef kind : interiorKinds) {
        Symbol *l = find((imp->getName() + kind + Twine(addend)).str());
        if (l && l->isLazy() && !l->pendingArchiveLoad && isLazyImport(l)) {
          lazies.push_back(l);
          pending = true;
        }
      }
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
    if (!pending && readOnly && canSeal && isa<DefinedImportData>(s) &&
        !findInteriorImport(imp, addend))
      sealed = true;
    return pending;
  };
  std::vector<std::pair<SectionChunk *, size_t>> pendingWords;
  if (first)
    for (auto [sc, i] : pendingInteriorWords)
      if (walkWord(sc, sc->getRelocs()[i]))
        pendingWords.push_back({sc, i});
  for (ObjFile *file : llvm::drop_begin(ctx.objFileInstances, first)) {
    if (&file->symtab != this ||
        llvm::none_of(file->getSymbols(), getInPlaceImport))
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || (sc->header->Characteristics &
                  (IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_CNT_CODE)))
        continue;
      for (auto [i, rel] : llvm::enumerate(sc->getRelocs()))
        if (walkWord(sc, rel))
          pendingWords.push_back({sc, i});
    }
  }
  pendingInteriorWords = std::move(pendingWords);
  // A DLL's fill writes the entries and read-only slots of the overridable
  // defaults it imports, and the read-only cells of its own defaults, between
  // two protection calls. Its own default has cells only where the DLL takes
  // its address, through the import pointer or in a word of its static data.
  if (!sealed && canSeal && ctx.config.dll && ctx.config.importSlots) {
    auto isOwnDefault = [&](StringRef name) {
      return overridablesByName.contains(name) || isOwnSlotDefault(name);
    };
    auto addressTaken = [&](StringRef name) {
      return find(("__imp_" + name).str()) != nullptr;
    };
    sealed = llvm::any_of(overridablesByName, addressTaken) ||
             llvm::any_of(overrideSlotFacts, [&](const auto &f) {
               return f.second.own && addressTaken(f.first);
             });
    if (!sealed && (!overridablesByName.empty() ||
                    llvm::any_of(overrideSlotFacts,
                                 [](const auto &f) { return f.second.own; })))
      for (ObjFile *file : llvm::drop_begin(ctx.objFileInstances, first)) {
        if (&file->symtab != this || sealed)
          continue;
        for (Chunk *c : file->getChunks()) {
          auto *sc = dyn_cast_or_null<SectionChunk>(c);
          if (!sc || (sc->header->Characteristics &
                      (IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_CNT_CODE)))
            continue;
          for (const coff_relocation &rel : sc->getRelocs()) {
            Symbol *s = file->getSymbol(rel.SymbolTableIndex);
            if (s && isOwnDefault(s->getName()) && sc->isAddressWord(rel)) {
              sealed = true;
              break;
            }
          }
          if (sealed)
            break;
        }
      }
  }
  // A DLL's cells of its own preemptible vague entities: the pointers its
  // import-form references read, in read-only data, and read-only words of
  // its static data.
  if (!sealed && canSeal && ctx.config.dll && ctx.config.importSlots) {
    sealed = llvm::any_of(localImportChunks, [](Chunk *c) {
      return !cast<LocalImportChunk>(c)->vagueEntity.empty();
    });
    for (ObjFile *file : llvm::drop_begin(ctx.objFileInstances, first)) {
      if (&file->symtab != this || sealed)
        continue;
      for (Chunk *c : file->getChunks()) {
        auto *sc = dyn_cast_or_null<SectionChunk>(c);
        if (!sc || sealed ||
            (sc->header->Characteristics &
             (IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_CNT_CODE |
              IMAGE_SCN_MEM_WRITE)))
          continue;
        for (const coff_relocation &rel : sc->getRelocs())
          if (Symbol *s = file->getSymbol(rel.SymbolTableIndex);
              s && sc->isAddressWord(rel) && isPreemptibleVagueEntity(s)) {
            sealed = true;
            break;
          }
      }
    }
  }
  if (!sealed && canSeal && ctx.config.dll) {
    readOverrideRecords();
    sealed = llvm::any_of(ctx.importFileInstances, [&](ImportFile *f) {
      if (&f->symtab != this || !f->impSym || f->isDelayLoaded())
        return false;
      StringRef name = f->impSym->getName().drop_front(strlen("__imp_"));
      return overrideImportSlot(name) || defaultsByName.contains(name);
    });
  }
  if (sealed)
    if (Symbol *l = find("__imp_NtProtectVirtualMemory");
        l && l->isLazy() && !l->pendingArchiveLoad && isLazyImport(l))
      lazies.push_back(l);

  // Each lazy is an import pointer, which no object references, or it would
  // not be lazy; the member also defines the import's own name, which one may.
  auto load = [&](Symbol *l) {
    if (isa_and_nonnull<Undefined>(
            find(l->getName().drop_front(strlen("__imp_")))))
      interiorImportsRewalk = true;
    forceLazy(l);
  };
  bool loaded = false;
  for (Symbol *l : lazies) {
    if (!l->isLazy() || l->pendingArchiveLoad)
      continue;
    Log(ctx) << "Loading lazy " << l->getName() << " from "
             << l->getFile()->getName() << " for a word of static data";
    load(l);
    loaded = true;
  }
  for (Symbol *l : runTimeLazies) {
    if (!l->isLazy() || l->pendingArchiveLoad)
      continue;
    Log(ctx) << "Loading lazy " << l->getName() << " from "
             << l->getFile()->getName() << " to bind imports at run time";
    load(l);
    loaded = true;
  }
  return loaded;
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
    // The record of a thread-local variable, which the image writes where no
    // input defines it, needs the variable and the image's TLS index, which
    // the reference loads, as a direct reference to the variable would.
    if (!l && name.ends_with("$tls")) {
      if (Symbol *var = find(name.drop_back(strlen("$tls")))) {
        if (var->isLazy() && !var->pendingArchiveLoad)
          lazies.push_back(var);
        if (Symbol *index = find(mangle("_tls_index"));
            !index || (index->isLazy() && !index->pendingArchiveLoad)) {
          addUndefined(mangle("_tls_index"));
          referenced = true;
        }
      }
      return false;
    }
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
    if (l && l->isLazy() && !l->pendingArchiveLoad && isLazyImport(l))
      lazies.push_back(l);
  }

  // What an import library offers of the COMDAT definitions of the link loads
  // the offer, as ELF's definition in a shared object preempts the same
  // definition: a function's gives the function's address to every
  // address-take, and data's, with those of the other external symbols of its
  // group and of its guard, takes the data's place.
  std::vector<Symbol *> offers;
  if (ctx.config.importSlots && !ctx.hybridSymtab && !isEC()) {
    for (; weakFunctionObjects != ctx.objFileInstances.size();
         ++weakFunctionObjects)
      if (ObjFile *file = ctx.objFileInstances[weakFunctionObjects];
          &file->symtab == this)
        collectWeakFunctions(file, weakFunctions);
    for (Symbol *sym : weakFunctions)
      if (isa<DefinedRegular>(sym))
        if (Symbol *l = find(("__imp_" + sym->getName()).str());
            l && isLazyImportOf(l, IMPORT_CODE))
          offers.push_back(l);

    auto addOffer = [&](StringRef name) {
      Symbol *l = find(("__imp_" + name).str());
      if (l && isLazyDataImport(l))
        offers.push_back(l);
      return l && (isa<DefinedImportData>(l) || isLazyDataImport(l));
    };
    SmallVector<SectionChunk *, 4> chunks;
    SmallVector<DefinedRegular *, 4> members;
    for (Symbol *sym : weakData) {
      auto *d = dyn_cast<DefinedRegular>(sym);
      if (!d || !isDefaultVisibility(d) || !addOffer(getSharedName(d)))
        continue;
      addOffer(getSharedGuardName(d));
      if (!isa<ObjFile>(d->getFile()))
        continue;
      chunks.clear();
      members.clear();
      collectComdatGroup(d, chunks, members);
      for (DefinedRegular *m : members)
        if (m != d)
          addOffer(getSharedName(m));
    }
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
  for (Symbol *l : offers) {
    if (!l->isLazy() || l->pendingArchiveLoad)
      continue;
    Log(ctx) << "Loading lazy " << l->getName() << " from "
             << l->getFile()->getName() << " for its offer";
    forceLazy(l);
    loaded = true;
  }
  return loaded;
}

Defined *SymbolTable::impSymbol(StringRef name) {
  if (name.starts_with("__imp_"))
    return nullptr;
  return dyn_cast_or_null<Defined>(find(("__imp_" + name).str()));
}

// There may exist symbols named .refptr.<name> which only consist
// of a single pointer to <name>. If it turns out <name> is
// automatically imported, we don't need to keep the .refptr.<name>
// pointer at all, but redirect all accesses to it to the IAT entry
// for __imp_<name> instead, and drop the whole .refptr.<name> chunk.
void SymbolTable::replaceRefPtr(Symbol *sym, StringRef name, Defined *imp) {
  DefinedRegular *refptr =
      dyn_cast_or_null<DefinedRegular>(find((".refptr." + name).str()));
  if (refptr && refptr->getChunk()->getSize() == ctx.config.wordsize) {
    SectionChunk *sc = dyn_cast_or_null<SectionChunk>(refptr->getChunk());
    if (sc && sc->getRelocs().size() == 1 && *sc->symbols().begin() == sym) {
      Log(ctx) << "Replacing .refptr." << name << " with " << imp->getName();
      refptr->getChunk()->live = false;
      refptr->replaceKeepingName(imp, isa<DefinedImportData>(imp)
                                          ? sizeof(DefinedImportData)
                                          : sizeof(DefinedRegular));
    }
  }
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
              << imp->getName() << " from " << imp->getFile()
              << "; unexpected symbol type";
    return false;
  }
  sym->replaceKeepingName(imp, impSize);
  sym->isRuntimePseudoReloc = true;
  replaceRefPtr(sym, name, imp);
  return true;
}

void SymbolTable::preemptSymbol(Symbol *sym, DefinedImportData *imp) {
  Log(ctx) << "Preempting " << printSymbol(sym) << " in " << sym->getFile()
           << " with its instance in " << imp->getDLLName();
  imp->file->live = true;
  preemptedGroupImports.insert(imp->file);
  sym->replaceKeepingName(imp, sizeof(DefinedImportData));
  sym->isRuntimePseudoReloc = true;
  preemptedData.insert(sym);
}

bool SymbolTable::ignoresOffersOf(DefinedImportData *imp) const {
  return imp->file->ignoresOffers();
}

void SymbolTable::bindImportsToDefinitions() {
  if (!ctx.config.importSlots || ctx.hybridSymtab || isEC())
    return;
  SmallPtrSet<Symbol *, 4> offers;
  for (const Export &e : exports)
    if (Defined *d = getOverrideOffer(e))
      offers.insert(d);
  for (ImportFile *file : ctx.importFileInstances) {
    DefinedImportData *imp = file->impSym;
    if (&file->symtab != this || !imp || find(imp->getName()) != imp)
      continue;
    StringRef name = imp->getName();
    StringRef target = name.drop_front(strlen("__imp_"));
    auto *d = dyn_cast_or_null<DefinedRegular>(findVagueEntity(target));
    if (!d || !isa<ObjFile>(d->getFile()))
      continue;
    // A definition the DLL offers is the program's only if the executable
    // selects it. The DLL's own references keep the default's import, which
    // the loader binds and the fill writes with the program's choice, so a
    // definition the program does not select is preempted in the DLL too, as
    // a definition in an ELF shared object is.
    if (offers.contains(d) && !file->isDelayLoaded()) {
      Log(ctx) << "Keeping " << name << " for " << printSymbol(d)
               << ", which the image offers";
      // Only an object that describes how its instructions use the
      // definition's address can have those uses reach the program's choice.
      auto *obj = cast<ObjFile>(d->getFile());
      if (machine == ARM64 ? !obj->hasLinkRecords()
                           : !obj->describesFunctionSites())
        Err(ctx) << d->getFile() << ": " << printSymbol(d)
                 << " is offered as an overridable default, but its object "
                    "does not describe its references to it, which therefore "
                    "cannot reach the program's choice";
      preemptedOffers[d] = imp;
      file->live = true;
      // The DLL's branches to the definition reach the import's thunk.
      if (file->thunkSym && !file->thunkSym->isGCRoot) {
        file->thunkSym->isGCRoot = true;
        ctx.config.gcroot.push_back(file->thunkSym);
      }
      continue;
    }
    // What the image takes from a DLL that offers it keeps the import: a
    // shared vague definition, which the DLL's instance preempts, or whose
    // address it gives.
    DefinedRegular *leader = d->getChunk()->sym;
    if (leader && leader->isCOMDAT && isShareableVagueName(leader->getName()) &&
        isDefaultVisibility(leader) && !ignoresOffersOf(imp))
      continue;
    // A thread-local variable's import binds to the record the image writes.
    Defined *bound = d;
    if (d->getName() != target && !(bound = getThreadLocalRecord(d)))
      continue;
    Log(ctx) << "Binding " << name << " to " << printSymbol(bound) << " in "
             << d->getFile();
    // The import keeps a symbol of its own, which nothing in the link names.
    file->impSym = make<DefinedImportData>(name, file, file->location);
    if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(file->thunkSym))
      thunk->wrappedSym = file->impSym;
    replaceSymbol<DefinedLocalImport>(imp, ctx, name, bound);
    localImportChunks.push_back(cast<DefinedLocalImport>(imp)->getChunk());
  }
}

void SymbolTable::bindForwarders() {
  for (auto [file, sym, pointer] : forwarders) {
    auto *f = dyn_cast<DefinedRegular>(sym);
    auto *li = dyn_cast<DefinedLocalImport>(pointer);
    if (!f || f->getFile() != file || !li)
      continue;
    auto *target = dyn_cast<DefinedRegular>(li->getTarget());
    if (!target || target == f)
      continue;
    Log(ctx) << "Replacing forwarder " << printSymbol(f) << " in " << file
             << " by " << printSymbol(target);
    f->replaceKeepingName(target, sizeof(DefinedRegular));
  }
}

DefinedRegular *SymbolTable::makeCopy(DefinedRegular *d) {
  auto *copy = make<DefinedRegular>(*d);
  copy->isExternal = false;
  copy->isCOMDAT = false;
  return copy;
}

void SymbolTable::preemptWeakData() {
  if (!ctx.config.importSlots || ctx.hybridSymtab || isEC() || weakData.empty())
    return;
  llvm::TimeTraceScope timeScope("Preempt weak data");
  DenseSet<Symbol *> seen;
  SmallVector<SectionChunk *, 4> chunks;
  SmallVector<DefinedRegular *, 4> members;
  SmallVector<DefinedImportData *, 4> imports;
  for (Symbol *sym : weakData) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !seen.insert(d).second || !isa<ObjFile>(d->getFile()) ||
        !d->getChunk()->live || !isDefaultVisibility(d))
      continue;
    auto *imp =
        dyn_cast_or_null<DefinedImportData>(impSymbol(getSharedName(d)));
    if (!imp)
      continue;

    collectWeakDataGroup(d, chunks, members, /*liveOnly=*/true);

    // The group moves as a whole: an image that kept any of it would run its
    // own initialization of the DLL's instance, or keep a second guard.
    imports.clear();
    for (DefinedRegular *m : members) {
      auto *mi =
          dyn_cast_or_null<DefinedImportData>(impSymbol(getSharedName(m)));
      if (mi && mi->getDLLName().equals_insensitive(imp->getDLLName())) {
        imports.push_back(mi);
        continue;
      }
      Err(ctx) << m->getFile() << ": " << printSymbol(d) << " is provided by "
               << imp->getDLLName() << ", but " << printSymbol(m)
               << ", which belongs with it, is "
               << (mi ? "provided by " + mi->getDLLName().str()
                      : "not provided by " + imp->getDLLName().str());
    }
    if (imports.size() != members.size())
      continue;

    // A copy that never changes may still serve the loads that only read
    // values through its address, under a symbol of its own for each member
    // of a group that lies in one section.
    SectionChunk *sc = d->getChunk();
    if (chunks.size() == 1 && !(sc->header->Characteristics &
                                (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_CODE)))
      for (auto [m, mi] : llvm::zip_equal(members, imports))
        immutableCopies[mi] = makeCopy(m);
    for (auto [m, mi] : llvm::zip_equal(members, imports))
      preemptSymbol(m, mi);
    for (SectionChunk *c : chunks) {
      c->live = false;
      for (const coff_relocation &rel : c->getRelocs())
        if (auto *imp = dyn_cast_or_null<DefinedImportData>(
                c->file->getSymbol(rel.SymbolTableIndex));
            imp && !imp->isRuntimePseudoReloc)
          preemptedGroupImports.insert(imp->file);
    }
  }
  if (preemptedData.empty())
    return;

  // An export of preempted data forwards to the DLL's instance.
  forwardExports([&](Symbol *s) -> DefinedImportData * {
    return preemptedData.contains(s) ? cast<DefinedImportData>(s) : nullptr;
  });
}

bool SymbolTable::isCanonicalCopy(SectionChunk *sc) {
  if (sc->header->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_CODE))
    return false;
  for (const coff_relocation &rel : sc->getRelocs()) {
    // An address inside an object, or in the image alone, such as that of a
    // string literal, differs from the instance's, unless the DLL that
    // provides the object exports that address exactly.
    Symbol *s = sc->file->getSymbol(rel.SymbolTableIndex);
    std::optional<int64_t> addend = getAddressWordAddend(sc, rel);
    if (!sc->isAddressWord(rel) || !addend)
      return false;
    if (isa_and_nonnull<DefinedAbsolute>(s))
      continue;
    if (DefinedImportData *imp = getInPlaceImport(s);
        imp && (!*addend || findInteriorImport(imp, *addend)))
      continue;
    // A copy kept for its reads must not name what liveness discarded.
    if (*addend || (!offeredFunctions.contains(s) &&
                    !(isPreemptibleVagueEntity(s) && s->isLive())))
      return false;
  }
  return true;
}

void SymbolTable::readImmutableDataLocally() {
  if (!ctx.config.importSlots || ctx.hybridSymtab || isEC())
    return;
  llvm::TimeTraceScope timeScope("Read immutable data locally");
  // By the symbol that value-only loads' relocations name, the copy they read
  // instead, if there is one: that of data a DLL's instance preempts, named
  // by its import, and the instance of data the DLL keeps preemptible, named
  // by its cell.
  struct Copy {
    DefinedRegular *copy = nullptr;
    // If liveness discarded the copy, the definitions of its group, each with
    // the import of the DLL's instance that preempts it once the copy is kept.
    SmallVector<std::pair<DefinedRegular *, DefinedImportData *>, 1> discarded;
    DefinedLocalImport *pointer = nullptr;
    size_t sites = 0;
  };
  MapVector<Symbol *, Copy *> copies;
  SmallVector<SectionChunk *, 4> chunks;
  SmallVector<DefinedRegular *, 4> members;
  auto findCopy = [&](Symbol *s, Copy &c) {
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(s)) {
      if (DefinedRegular *copy = immutableCopies.lookup(imp)) {
        if (isCanonicalCopy(copy->getChunk()))
          c.copy = copy;
        return;
      }
      // Import-form references never name the copy, which liveness then
      // discards unless something else does.
      auto *d = dyn_cast_or_null<DefinedRegular>(
          find(imp->getName().drop_front(strlen("__imp_"))));
      if (!d || !isa<ObjFile>(d->getFile()) || !d->isCOMDAT ||
          d->getChunk()->live || isThreadLocal(d) || !isDefaultVisibility(d) ||
          ignoresOffersOf(imp))
        return;
      // The group moves to the DLL whole, so it must lie in the copy's
      // section, and the DLL must provide each member.
      collectWeakDataGroup(d, chunks, members, /*liveOnly=*/false);
      if (chunks.size() != 1 || !isCanonicalCopy(d->getChunk()))
        return;
      for (DefinedRegular *m : members) {
        auto *mi =
            dyn_cast_or_null<DefinedImportData>(impSymbol(getSharedName(m)));
        if (!mi || !mi->getDLLName().equals_insensitive(imp->getDLLName())) {
          c.discarded.clear();
          return;
        }
        c.discarded.push_back({m, mi});
      }
      c.copy = makeCopy(d);
    } else if (auto *cell = dyn_cast_or_null<DefinedLocalImport>(s);
               cell && !cell->getChunk()->vagueEntity.empty()) {
      auto *d = dyn_cast<DefinedRegular>(cell->getChunk()->getTarget());
      if (d && d->isLive() && !isThreadLocal(d) &&
          isCanonicalCopy(d->getChunk()))
        c.copy = d;
    }
  };

  // The loads of a function may lead to addresses that it compares, so the
  // value-only loads of a symbol in an object read the copy together, or not
  // at all where any of them is not the load its object describes.
  struct Site {
    SectionChunk *sc;
    size_t i, page;
    Symbol *sym;
    Copy *copy;
  };
  SmallVector<Site, 0> sites;
  SmallPtrSet<Copy *, 4> mismatched;
  auto getCopy = [&](Symbol *s) -> Copy & {
    auto [it, inserted] = copies.try_emplace(s, nullptr);
    if (inserted) {
      it->second = make<Copy>();
      findCopy(s, *it->second);
    }
    return *it->second;
  };
  // The index of the relocation at offset in sc, or the number of its
  // relocations if none is there.
  auto findReloc = [](SectionChunk *sc, uint32_t offset) {
    ArrayRef<coff_relocation> relocs = sc->getRelocs();
    size_t i = llvm::partition_point(relocs,
                                     [&](const coff_relocation &r) {
                                       return r.VirtualAddress < offset;
                                     }) -
               relocs.begin();
    return i != relocs.size() && relocs[i].VirtualAddress == offset
               ? i
               : relocs.size();
  };
  for (ObjFile *file : ctx.objFileInstances) {
    ArrayRef<ObjFile::LinkValueOnly> loads = file->getValueOnlyLoads();
    if (&file->symtab != this || loads.empty())
      continue;
    sites.clear();
    mismatched.clear();
    for (const ObjFile::LinkValueOnly &load : loads) {
      SectionChunk *sc = file->getSectionChunk(load.section);
      if (!sc || !sc->live ||
          !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE))
        continue;
      // An access that only reads, through the register of a load that
      // stays, may read the copy directly: on x86-64 where its code may be
      // resized, since a RIP-relative operand is longer than one through a
      // register, and on ARM64, where nothing changes size, anywhere.
      if (load.form == LinkSiteOther) {
        const ObjFile::LinkFusion *f = file->getLinkFusion(sc, load.offset);
        if (!f || f->use != ObjFile::LinkFusion::Access || !f->valueRead ||
            (!file->isCovered(sc) && sc->getArch() != Triple::aarch64))
          continue;
        size_t i = findReloc(sc, f->load);
        if (i == sc->getRelocs().size())
          continue;
        Symbol *s = file->getSymbol(sc->getRelocs()[i].SymbolTableIndex);
        Copy &copy = getCopy(s);
        if (!copy.copy)
          continue;
        immutableReads.try_emplace(s, copy.copy);
        ++copy.sites;
        continue;
      }
      size_t i = findReloc(sc, load.offset);
      if (i == sc->getRelocs().size())
        continue;
      Symbol *s = file->getSymbol(sc->getRelocs()[i].SymbolTableIndex);
      Copy &copy = getCopy(s);
      if (!copy.copy)
        continue;
      size_t page;
      if (isValueOnlyLoad(sc, i, load.form, load.page, page))
        sites.push_back({sc, i, page, s, &copy});
      else
        mismatched.insert(&copy);
    }

    DenseMap<Copy *, uint32_t> indices;
    for (auto [sc, i, page, sym, copy] : sites) {
      if (mismatched.contains(copy))
        continue;
      if (!copy->pointer) {
        copy->pointer =
            make<DefinedLocalImport>(ctx, sym->getName(), copy->copy);
        copy->pointer->getChunk()->copy = true;
        localImportChunks.push_back(copy->pointer->getChunk());
      }
      auto [index, added] = indices.try_emplace(copy, 0);
      if (added)
        index->second = file->addRangeThunkSymbol(copy->pointer);
      // The relocations are made writable as range extension thunks make
      // them.
      ArrayRef<coff_relocation> relocs = sc->getRelocs();
      if (relocs.data() ==
          file->getCOFFObj()->getRelocations(sc->header).data()) {
        auto *newRelocs = bAlloc().Allocate<coff_relocation>(relocs.size());
        llvm::copy(relocs, newRelocs);
        sc->setRelocs(ArrayRef(newRelocs, relocs.size()));
        relocs = sc->getRelocs();
      }
      auto *writable = const_cast<coff_relocation *>(relocs.data());
      writable[i].SymbolTableIndex = index->second;
      writable[page].SymbolTableIndex = index->second;
      ++copy->sites;
    }
  }

  // A discarded copy is kept, with no symbol, while loads or accesses read
  // it, and the imports whose addresses its words hold are kept with it.
  for (auto [s, copy] : copies) {
    if (!copy->sites)
      continue;
    SectionChunk *sc = copy->copy->getChunk();
    if (ctx.config.verbose) {
      // Each word that holds an address in another image is a slot the
      // loader binds by name.
      size_t inPlace = llvm::count_if(sc->getRelocs(), [&](auto &rel) {
        return getInPlaceImport(sc->file->getSymbol(rel.SymbolTableIndex));
      });
      Log(ctx) << "Keeping the copy of " << printSymbol(copy->copy) << " in "
               << copy->copy->getFile() << " for " << copy->sites
               << (copy->sites == 1 ? " value read" : " value reads")
               << (inPlace ? ", binding " + Twine(inPlace) + " of its words"
                           : Twine(""));
    }
    if (sc->live)
      continue;
    sc->sym = nullptr;
    auto keep = [&](SectionChunk *c) {
      c->live = true;
      for (const coff_relocation &rel : c->getRelocs())
        if (DefinedImportData *imp =
                getAddressedImport(c->file->getSymbol(rel.SymbolTableIndex)))
          imp->file->live = true;
    };
    keep(sc);
    for (SectionChunk &child : sc->children())
      keep(&child);
    for (auto [m, mi] : copy->discarded)
      preemptSymbol(m, mi);
  }
}

static StringRef getForwardTarget(DefinedImportData *imp);

void SymbolTable::forwardExports(
    function_ref<DefinedImportData *(Symbol *)> getInstance) {
  // Exports that an LTO object's directives add are matched by name.
  for (Export &e : exports) {
    Symbol *s = e.sym;
    if (!s && e.forwardTo.empty())
      s = find(e.name);
    DefinedImportData *imp = s ? getInstance(s) : nullptr;
    if (!imp)
      continue;
    if (!e.extName.empty())
      e.name = e.extName;
    e.extName = {};
    e.forwardTo = getForwardTarget(imp);
    e.sym = nullptr;
    forwardedImports[e.name] = imp;
  }
}

// The forwarder of an export to the instance that imp imports: the DLL's name
// without its extension, and the export's name or ordinal.
static StringRef getForwardTarget(DefinedImportData *imp) {
  StringRef dll = imp->getDLLName();
  std::string target = imp->file->hdr->getNameType() == IMPORT_ORDINAL
                           ? "#" + std::to_string(imp->getOrdinal())
                           : imp->getExternalName().str();
  return saver().save(dll.take_front(dll.rfind('.')) + "." + target);
}

void SymbolTable::addOffers() {
  if (!ctx.config.importSlots || ctx.hybridSymtab || isEC() ||
      (!ctx.config.dll && ctx.config.implib.empty()))
    return;
  // The names exported already, which an offer does not export again. An
  // offer is generated by the linker, and gives way to any other export.
  DenseSet<StringRef> exported;
  for (Export &e : exports)
    exported.insert(e.extName.empty() ? e.name : e.extName);
  auto offer = [&](Symbol *s, bool data, DefinedImportData *forward) {
    // A forwarded offer has the name the DLL offers it under, which is a
    // thread-local variable's record's.
    StringRef name =
        forward ? forward->file->impSym->getName().drop_front(strlen("__imp_"))
                : s->getName();
    if (!exported.insert(name).second)
      return;
    Export e;
    e.name = name;
    e.data = data;
    e.source = ExportSource::ExportAll;
    if (forward) {
      e.forwardTo = getForwardTarget(forward);
      forwardedImports[e.name] = forward;
    } else {
      e.sym = s;
      e.symbolName = e.name;
    }
    exports.push_back(e);
  };

  // The data with default visibility in a COMDAT that the image keeps, with
  // the other external symbols of its group and its guard; a thread-local
  // variable's record, which the image then keeps too.
  SmallVector<SectionChunk *, 4> chunks;
  SmallVector<DefinedRegular *, 4> members;
  for (Symbol *sym : weakData) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !isa<ObjFile>(d->getFile()) || !d->getChunk()->live ||
        !isDefaultVisibility(d))
      continue;
    collectWeakDataGroup(d, chunks, members, /*liveOnly=*/true);
    for (DefinedRegular *m : members) {
      if (!isThreadLocal(m)) {
        offer(m, /*data=*/true, nullptr);
      } else if (Defined *record = getThreadLocalRecord(m)) {
        keepThreadLocalRecord(record);
        offer(record, /*data=*/true, nullptr);
      }
    }
  }
  // The functions whose addresses are significant.
  for (Symbol *sym : weakFunctions)
    if (auto *d = dyn_cast<DefinedRegular>(sym);
        d && d->getChunk()->live && !offeredFunctions.contains(d))
      offer(d, /*data=*/false, nullptr);
  // What the image imports from a DLL that offers it, forwarded to that DLL,
  // so that a path through this image reaches the instance, whether or not
  // the image kept a copy.
  auto getOffer = [&](StringRef name) -> DefinedImportData * {
    auto *imp = dyn_cast_or_null<DefinedImportData>(impSymbol(name));
    if (!imp || !imp->file->live || ignoresOffersOf(imp))
      return nullptr;
    return imp;
  };
  for (Symbol *s : preemptedData)
    offer(s, /*data=*/true, cast<DefinedImportData>(s));
  for (Symbol *sym : weakData) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !isa<ObjFile>(d->getFile()) || !isDefaultVisibility(d))
      continue;
    collectWeakDataGroup(d, chunks, members, /*liveOnly=*/false);
    for (DefinedRegular *m : members)
      if (DefinedImportData *imp = getOffer(getSharedName(m)))
        offer(m, /*data=*/true, imp);
  }
  for (auto &[f, imp] : offeredFunctions)
    offer(f, /*data=*/false, imp);
  for (Symbol *f : weakFunctions)
    if (DefinedImportData *imp = getOffer(f->getName());
        imp && imp->file->thunkSym)
      offer(f, /*data=*/false, imp);
}

bool SymbolTable::isPreemptibleVagueEntity(Symbol *s) {
  if (!preemptibleVagueEntities) {
    preemptibleVagueEntities.emplace();
    Configuration &config = ctx.config;
    using B = Configuration::Bsymbolic;
    // The cells are written by the image's fill, which its C initializer
    // table runs.
    if (!config.importSlots || !config.dll || ctx.hybridSymtab || isEC() ||
        config.bsymbolic == B::All ||
        !isa_and_nonnull<Defined>(findUnderscore("__xi_a")))
      return false;
    // Asked before liveness is decided, which a cell's reference keeps.
    SmallVector<SectionChunk *, 4> chunks;
    SmallVector<DefinedRegular *, 4> members;
    for (Symbol *sym : weakData) {
      auto *d = dyn_cast<DefinedRegular>(sym);
      // A type_info object stays the DLL's own, which compares by name with
      // that of an image no link joins.
      if (!d || !isa<ObjFile>(d->getFile()) || !isDefaultVisibility(d) ||
          isTypeInfoName(d->getName()))
        continue;
      collectWeakDataGroup(d, chunks, members, /*liveOnly=*/false);
      // Immutable data is preemptible too, since its address is the program's
      // one; its value reads go through the cell with its address-takes,
      // which nothing yet tells apart. A thread-local variable is preempted
      // with its record.
      preemptibleVagueEntities->insert_range(members);
    }
    if (config.bsymbolic != B::Functions)
      for (Symbol *sym : weakFunctions)
        if (auto *d = dyn_cast<DefinedRegular>(sym);
            d && !offeredFunctions.contains(d))
          preemptibleVagueEntities->insert(d);
  }
  if (DefinedRegular *var = getRecordVariable(s))
    s = var;
  return preemptibleVagueEntities->contains(s);
}

void SymbolTable::keepThreadLocalRecord(Defined *record) {
  auto *c = dyn_cast_or_null<ThreadLocalRecordChunk>(record->getChunk());
  if (!c)
    return;
  c->live = true;
  // A guard that liveness discarded is set by nothing live, and so is never
  // tested.
  if (auto *g = dyn_cast<DefinedRegular>(c->guard); g && !g->isLive())
    c->guard = c->var;
}

void SymbolTable::bindOfferedFunctions() {
  if (!ctx.config.importSlots || ctx.hybridSymtab || isEC() ||
      weakFunctions.empty())
    return;
  for (Symbol *sym : weakFunctions) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !d->getChunk()->live || offeredFunctions.contains(d))
      continue;
    auto *imp = dyn_cast_or_null<DefinedImportData>(impSymbol(d->getName()));
    if (!imp || !imp->file->thunkSym || ignoresOffersOf(imp))
      continue;
    Log(ctx) << "Binding the address of " << printSymbol(d) << " in "
             << d->getFile() << " to its instance in " << imp->getDLLName();
    imp->file->live = true;
    offeredFunctions[d] = imp;
  }
  // An export of the function forwards to the DLL's instance.
  if (!offeredFunctions.empty())
    forwardExports([&](Symbol *s) { return offeredFunctions.lookup(s); });
}

Defined *SymbolTable::getOverrideOffer(const Export &e) {
  if (!ctx.config.dll || !e.sym || !e.extName.empty() || !e.exportAs.empty() ||
      (!overridables.contains(e.sym->getName()) &&
       !defaultsByName.contains(e.sym->getName())))
    return nullptr;
  // The runtime's default is a weak definition, the fallback that the
  // executable's replacement supersedes, so exporting it is what makes it
  // the default of every image. Its definition is the weak external's
  // .weak.<name>.* alias, or a weak definition from bitcode.
  Symbol *def = e.sym;
  if (auto *u = dyn_cast<Undefined>(def))
    def = u->getWeakAlias();
  auto *d = dyn_cast_or_null<Defined>(def);
  if (!d || d->isWeak || d->getName().starts_with(".weak."))
    return nullptr;
  return d;
}

void SymbolTable::offerOverridableDefaults() {
  if (overridables.empty() && defaultsByName.empty())
    return;
  // Only the executable replaces an overridable default for every image of
  // the program. A DLL's strong definition is an offer the executable's link
  // may select instead, so the DLL exports the body under the offer name
  // rather than its own: no importer binds to it as the ABI symbol, which
  // would split the heap, and an executable that links the DLL discovers the
  // offer and picks the winner for each slot.
  for (Export &e : exports)
    if (getOverrideOffer(e))
      e.extName = saver().save(overrideOfferPrefix + e.sym->getName());
}

bool SymbolTable::loadOverrideOfferImports() {
  if (!ctx.config.importSlots || !ctx.config.dll || ctx.hybridSymtab || isEC())
    return false;
  readOverrideRecords();
  if (overridables.empty() && defaultsByName.empty())
    return false;
  // The DLL's own references to a definition it offers go through the
  // default's import, which nothing else may have loaded, since the DLL
  // defines the name. A link without the runtime's import library has no
  // import, and the DLL keeps its definition to itself.
  bool loaded = false;
  for (const Export &e : exports) {
    if (!getOverrideOffer(e))
      continue;
    Symbol *l = find(("__imp_" + e.sym->getName()).str());
    if (l && l->isLazy() && !l->pendingArchiveLoad && isLazyImport(l)) {
      forceLazy(l);
      loaded = true;
    }
  }
  return loaded;
}

// A strong definition in a bitcode file counts too: the check runs before LTO,
// whose object then defines the replacement.
bool SymbolTable::isLocalOverrideReplacement(StringRef name) {
  auto *d = dyn_cast_or_null<DefinedRegular>(find(name));
  return d && !d->isWeak && isa<ObjFile, BitcodeFile>(d->getFile()) &&
         !d->getName().starts_with(".weak.");
}

bool SymbolTable::loadOverrideOffers() {
  if (!ctx.config.importSlots || ctx.config.dll || ctx.hybridSymtab ||
      isEC() || overrideOffersLoaded)
    return false;
  overrideOffersLoaded = true;
  // A DLL that defines its own overridable default exports it under the offer
  // name, so its import library carries an offer symbol. Materialise the offer
  // symbols the linked import libraries hold, so this link can see them by
  // name; each names an offered default's ABI symbol after the prefix.
  addIndexedLazies(overrideOfferPrefix);
  SmallVector<StringRef, 8> offers;
  for (auto &pair : symMap)
    if (pair.second->getName().starts_with(overrideOfferPrefix))
      offers.push_back(pair.second->getName());
  bool pulled = false;
  for (StringRef offer : offers) {
    StringRef name = offer.drop_front(overrideOfferPrefix.size());
    overrideOffers.insert(name);
    // The executable's own replacement wins its slot, so an offer of a default
    // the executable replaces itself is left unpulled. The first offer in link
    // order, which find resolves, wins among several, as ELF selects the first
    // in breadth-first order.
    if (isLocalOverrideReplacement(name))
      continue;
    if (Symbol *s = find(offer); s && s->isLazy()) {
      addGCRoot(offer);
      pulled = true;
    }
  }
  // After the DLLs whose import libraries the link names come the DLLs they
  // depend on, in ELF's breadth-first order, which the records of those
  // import libraries describe. The executable imports the first such offer
  // of a default that nothing before it offers or replaces from its DLL by
  // name, as it would through that DLL's import library.
  for (auto &[dll, name] :
       getDependentOverrideOffers(ctx, getImportLibraryRecords())) {
    if (overrideOffers.contains(name) || isLocalOverrideReplacement(name))
      continue;
    StringRef offer = saver().save(overrideOfferPrefix + name);
    overrideOffers.insert(offer.drop_front(overrideOfferPrefix.size()));
    Log(ctx) << "Importing " << offer << " from " << dll
             << ", which a DLL the image imports depends on";
    ctx.driver.addFile(
        make<ImportFile>(ctx, makeShortImport(machine, saver().save(dll), offer,
                                              IMPORT_CODE, IMPORT_NAME)));
    addGCRoot(offer);
    pulled = true;
  }
  return pulled;
}

SmallVector<MemoryBufferRef, 4> SymbolTable::getImportLibraryRecords() {
  SmallVector<MemoryBufferRef, 4> records;
  for (ArchiveFile *f : indexedArchives)
    for (StringRef name : f->getSymbolNames(vagueRecordPrefix))
      if (auto *lazy = dyn_cast_or_null<LazyArchive>(find(name)))
        records.push_back(lazy->getMemberBuffer());
  return records;
}

void SymbolTable::readOverrideRecords() {
  // The records come only with the import libraries, so they are read again
  // only once more libraries have been read.
  if (!ctx.config.importSlots || ctx.hybridSymtab || isEC() ||
      indexedArchives.size() == overrideRecordsArchives)
    return;
  overrideRecordsArchives = indexedArchives.size();
  OverrideRecords records = readImportedOverrideRecords(
      ctx, getImportLibraryRecords(), overrideTableVersion);
  for (std::string &name : records.byName)
    defaultsByName.insert(saver().save(name));
  for (OverrideRecords::Slot &s : records.slots)
    addOverrideSlotFacts(saver().save(s.name),
                         {s.slot, s.forward, s.dropsSecond, /*own=*/false},
                         "an import library");
}

void SymbolTable::addOverrideSlotFacts(StringRef name, OverrideSlotFacts facts,
                                       const Twine &source) {
  auto [it, inserted] = overrideSlotFacts.try_emplace(name, facts);
  if (inserted)
    return;
  OverrideSlotFacts &old = it->second;
  if (old.slot != facts.slot || old.forward != facts.forward ||
      old.dropsSecond != facts.dropsSecond) {
    Err(ctx) << source << " records overridable default " << name
             << " with slot " << facts.slot << ", but another input with slot "
             << old.slot << " or another forwarding";
    return;
  }
  old.own |= facts.own;
}

std::optional<uint32_t> SymbolTable::overrideImportSlot(StringRef name) {
  auto it = overrideSlotFacts.find(name);
  if (it == overrideSlotFacts.end())
    return std::nullopt;
  return it->second.slot;
}

void SymbolTable::resolveOverridableReplacements() {
  if (!ctx.config.importSlots || ctx.config.dll || ctx.hybridSymtab || isEC())
    return;
  readOverrideRecords();
  if (overridables.empty() && overrideOffers.empty() &&
      defaultsByName.empty() && vagueOwners.empty())
    return;
  // An overridable default this executable replaces is one an input defines
  // with a strong definition, the body B, superseding the runtime's weak
  // default for every image. A default a DLL defines with no fixed slot is
  // replaced by name, through the table's block, by any such definition,
  // whether or not its object marks it, as an executable's definition
  // preempts a shared object's on ELF.
  SmallVector<std::pair<StringRef, Defined *>, 8> replaced;
  auto add = [&](StringRef name, Defined *body) {
    if (defaultsByName.contains(name))
      overrideReplacementsByName.emplace_back(name, body);
    else
      replaced.emplace_back(name, body);
  };
  for (StringRef name : overridables)
    if (!defaultsByName.contains(name) && isLocalOverrideReplacement(name))
      add(name, cast<Defined>(find(name)));
  for (StringRef name : defaultsByName)
    if (isLocalOverrideReplacement(name))
      add(name, cast<Defined>(find(name)));
  // A default the executable does not replace itself but a linked DLL offers is
  // replaced for every image by that offer, reached through an import thunk to
  // the DLL's instance that loadOverrideOffers pulled.
  for (StringRef name : overrideOffers) {
    if (isLocalOverrideReplacement(name))
      continue;
    auto *thunk =
        dyn_cast_or_null<Defined>(find((overrideOfferPrefix + name).str()));
    if (thunk && !thunk->isLazy())
      add(name, thunk);
  }
  llvm::sort(overrideReplacementsByName, llvm::less_first());
  if (replaced.empty()) {
    buildOverrideSlots();
    return;
  }
  // The slots, which the C++ runtime's definitions of its defaults record, in
  // its objects or in its import library. Without them there is no default to
  // override: a program that defines a replaceable function with no such
  // runtime, as a freestanding one may, keeps its own definition and needs no
  // table.
  if (overrideSlotFacts.empty()) {
    buildOverrideSlots();
    return;
  }
  for (auto [name, body] : replaced) {
    std::optional<uint32_t> slot = overrideImportSlot(name);
    if (!slot) {
      Err(ctx) << body->getFile() << ": " << printSymbol(body)
               << " replaces an overridable default, but the C++ runtime "
                  "records no slot for it";
      continue;
    }
    overrideReplacements.push_back({body, name, *slot});
  }
  buildOverrideSlots();
}

bool SymbolTable::bindVagueSiblings() {
  Configuration &config = ctx.config;
  if (!config.importSlots || config.dll || ctx.hybridSymtab || isEC() ||
      !vagueOwners.empty())
    return false;
  // What the executable keeps: its default-visibility vague data, with the
  // other external symbols of its group and its guard, and functions whose
  // addresses are significant, that no DLL it imports from offers, a
  // delay-loaded one's offer not being taken.
  DenseSet<StringRef> kept;
  auto keeps = [&](DefinedRegular *d) {
    if (!d || !isa<ObjFile>(d->getFile()) || !isDefaultVisibility(d))
      return false;
    auto *imp =
        dyn_cast_or_null<DefinedImportData>(impSymbol(getSharedName(d)));
    return !imp || ignoresOffersOf(imp);
  };
  SmallVector<SectionChunk *, 4> chunks;
  SmallVector<DefinedRegular *, 4> members;
  for (Symbol *sym : weakData)
    if (auto *d = dyn_cast<DefinedRegular>(sym); keeps(d)) {
      collectWeakDataGroup(d, chunks, members, /*liveOnly=*/false);
      for (DefinedRegular *m : members)
        kept.insert(getSharedName(m));
    }
  for (Symbol *sym : weakFunctions)
    if (auto *d = dyn_cast<DefinedRegular>(sym); keeps(d))
      kept.insert(d->getName());

  for (auto &[name, dll] : chooseVagueOwners(ctx, kept)) {
    StringRef entity = saver().save(name);
    if (dll.empty()) {
      auto *d = dyn_cast_or_null<Defined>(findVagueEntity(entity));
      // A thread-local variable's instance is its record, which the pointer
      // keeps.
      if (d && isThreadLocal(d)) {
        d = getThreadLocalRecord(cast<DefinedRegular>(d));
        if (d)
          ctx.config.gcroot.push_back(d);
      }
      if (!d)
        continue;
      auto *pointer = make<LocalImportChunk>(ctx, d);
      localImportChunks.push_back(pointer);
      vagueOwners[entity] = make<DefinedSynthetic>(
          saver().save("__vague_owner$" + entity), pointer);
      continue;
    }
    // The import of the owner's instance, unless the executable imports the
    // entity from a DLL already, which is then the one it reaches.
    vagueOwners[entity] = nullptr;
    if (isa_and_nonnull<DefinedImportData>(find(("__imp_" + entity).str())))
      continue;
    Log(ctx) << "Importing " << entity << " from " << dll
             << ", which owns it for the program";
    ctx.driver.addFile(make<ImportFile>(
        ctx, makeShortImport(machine, saver().save(dll), entity, IMPORT_DATA,
                             IMPORT_NAME)));
  }
  if (vagueOwners.empty())
    return false;
  // The table's validation, as for a replacement.
  if (!overrideAnchor) {
    overrideAnchor = make<OverrideAnchorChunk>(ctx, *this);
    addSynthetic("__ovr_anchor", overrideAnchor);
    addGCRoot(mangle("__ovr_check"));
  }
  for (auto &[entity, word] : vagueOwners)
    if (!word)
      addGCRoot(saver().save("__imp_" + entity));
  return true;
}

void SymbolTable::buildOverrideSlots() {
  // The words of the vague entities' owners, once their imports are read.
  for (auto &[entity, word] : vagueOwners) {
    if (!word)
      word =
          dyn_cast_or_null<DefinedImportData>(find(("__imp_" + entity).str()));
    if (word)
      overrideReplacementsByName.emplace_back(entity, word);
  }
  llvm::sort(overrideReplacementsByName, llvm::less_first());
  if (overrideReplacements.empty() && overrideReplacementsByName.empty())
    return;
  auto root = [&](Defined *d) {
    if (!d->isGCRoot) {
      d->isGCRoot = true;
      ctx.config.gcroot.push_back(d);
    }
  };
  // The block's entries: each takes the first free bucket from its home
  // bucket, in the order of their names, so that the layout does not depend
  // on the order of the inputs, and the rest go to the overflow list.
  DenseMap<uint64_t, StringRef> keys;
  std::array<bool, overrideBucketCount> taken = {};
  std::vector<OverrideByName> overflow;
  for (auto [name, body] : overrideReplacementsByName) {
    uint64_t key = overrideNameKey(name);
    auto [it, inserted] = keys.try_emplace(key, name);
    if (!inserted || key == 0) {
      Err(ctx) << "overridable defaults " << it->second << " and " << name
               << " have the same key in the override table's block";
      continue;
    }
    root(body);
    OverrideByName entry{body, name, key, -1, vagueOwners.contains(name)};
    uint32_t home = overrideHomeBucket(key);
    for (uint32_t i = 0; i != overrideBucketCount; ++i) {
      uint32_t b = (home + i) % overrideBucketCount;
      if (!taken[b]) {
        taken[b] = true;
        entry.bucket = b;
        break;
      }
    }
    if (entry.bucket < 0)
      overflow.push_back(entry);
    else
      overrideBlock.push_back(entry);
  }
  llvm::sort(overrideBlock,
             [](const OverrideByName &a, const OverrideByName &b) {
               return a.bucket < b.bucket;
             });
  llvm::append_range(overrideBlock, overflow);
  // The slot each replacement names, its body.
  DenseMap<uint32_t, Defined *> replaced;
  for (const OverrideReplacement &r : overrideReplacements)
    replaced[r.slot] = r.body;

  // The forwarding graph the runtime's defaults record: the standard specifies
  // most defaults as calling another, and each such default records that
  // other's slot, and whether the other takes its arguments without the
  // second. A default forwarded to a replaced default runs the replacement's
  // body.
  struct Edge {
    uint32_t target;
    bool dropsSecond;
  };
  DenseMap<uint32_t, Edge> edges;
  for (auto &[name, facts] : overrideSlotFacts)
    if (facts.forward)
      edges[facts.slot] = {*facts.forward, facts.dropsSecond};
  // Each forwarding slot that is not replaced resolves to the nearest replaced
  // slot its chain reaches, through a thunk when an edge on the way drops the
  // second argument.
  DenseMap<uint32_t, std::pair<Defined *, bool>> forwarded;
  for (const auto &edge : edges) {
    uint32_t from = edge.first;
    if (replaced.count(from))
      continue;
    uint32_t cur = from;
    bool drops = false;
    DenseSet<uint32_t> seen;
    while (edges.count(cur) && seen.insert(cur).second) {
      drops |= edges[cur].dropsSecond;
      cur = edges[cur].target;
      auto it = replaced.find(cur);
      if (it != replaced.end()) {
        forwarded[from] = {it->second, drops};
        break;
      }
    }
  }

  // The table runs from slot zero to the highest slot it writes, and never
  // fewer than its first eight, the hot line, which is always present, so a
  // hot-line slot's check needs no bound. A table with no slot replaced still
  // holds them, beside its block.
  uint32_t count = 8;
  for (const auto &r : replaced)
    count = std::max(count, r.first + 1);
  for (const auto &f : forwarded)
    count = std::max(count, f.first + 1);
  overrideSlots.assign(count, {});
  // A replacement names its body and sets the anchor's mask bit for its slot.
  for (const auto &r : replaced) {
    overrideSlots[r.first] = {r.second, /*replaced=*/true};
    if (r.first < 32)
      overrideMask |= 1u << r.first;
    root(r.second);
  }
  // A forwarded slot names the replacement's body, or the thunk to it, and
  // sets no mask bit. Each body is a root already; a thunk is kept because the
  // code section holds it.
  for (const auto &f : forwarded) {
    Defined *target = f.second.first;
    if (f.second.second) {
      auto *thunk = make<OverrideThunkChunk>(ctx, target);
      overrideThunks.push_back(thunk);
      target = make<DefinedSynthetic>(saver().save("__ovr_thunk$" + Twine(f.first)),
                                      thunk);
    }
    overrideSlots[f.first] = {target, /*replaced=*/false};
  }
}

bool SymbolTable::nameOverrideCheck() {
  if (!ctx.config.importSlots || ctx.config.dll || ctx.hybridSymtab || isEC() ||
      overrideAnchor)
    return false;
  readOverrideRecords();
  if (overrideSlotFacts.empty() && defaultsByName.empty())
    return false;
  // Not until a replacement supersedes the runtime's weak default: an input's
  // strong definition, or a linked DLL's offer the executable selects for a
  // slot it does not replace itself. A link with no replacement writes no
  // table.
  bool replaces =
      (!overrideSlotFacts.empty() &&
       llvm::any_of(
           overridables,
           [&](StringRef name) { return isLocalOverrideReplacement(name); })) ||
      llvm::any_of(
          defaultsByName,
          [&](StringRef name) { return isLocalOverrideReplacement(name); }) ||
      llvm::any_of(overrideOffers, [&](StringRef name) {
        return (!overrideSlotFacts.empty() || defaultsByName.contains(name)) &&
               !isLocalOverrideReplacement(name) &&
               isa_and_nonnull<Defined>(
                   find((overrideOfferPrefix + name).str()));
      });
  if (!replaces)
    return false;
  // The executable's start-up validation, whose TLS callback the C++ runtime
  // provides runs before any of the executable's code. It reads the anchor the
  // writer fills beside the table, which the runtime reaches by this name.
  overrideAnchor = make<OverrideAnchorChunk>(ctx, *this);
  addSynthetic("__ovr_anchor", overrideAnchor);
  addGCRoot(mangle("__ovr_check"));
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

  bool arm64 = machine == ARM64 && !ctx.hybridSymtab;
  auto processFile = [&](InputFile *file, ArrayRef<Symbol *> symbols) {
    // References whose instructions may be rewritten are reported only if
    // they still read the pointer, by bindLocalImports.
    auto *obj = dyn_cast<ObjFile>(file);
    bool deferred = obj && defersLocalImportWarning(obj, arm64);
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
  // A lookup below can enter a lazy symbol, so the loop visits a copy.
  SmallVector<Symbol *, 0> syms(llvm::make_second_range(symMap));
  for (Symbol *sym : syms) {
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
    // The record of a thread-local variable that the link defines, which the
    // image writes once LTO is done, and which needs the variable and its
    // guard.
    if (ctx.config.importSlots) {
      StringRef record = name;
      record.consume_front("__imp_");
      Symbol *var = record.consume_back("$tls") ? find(record) : nullptr;
      if (Defined *def = var ? var->getDefined() : nullptr;
          def && isThreadLocal(def)) {
        def->isUsedInRegularObj = true;
        if (Symbol *guard = find(getGuardName(record)))
          guard->isUsedInRegularObj = true;
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

// Whether the linker may define the symbol u refers to: a plain reference, or
// a weak one whose alias is zero, as a weak declaration makes it. A weak alias
// to anything else is the definition the program chose.
static bool isDefinableReference(Undefined *u) {
  Symbol *alias = u->getWeakAlias();
  auto *abs = dyn_cast_or_null<DefinedAbsolute>(alias);
  return !alias || (abs && abs->getVA() == 0);
}

void SymbolTable::addStartStopSymbols() {
  // An ordered map keeps the diagnostics in a stable order.
  std::map<StringRef, SectionRun> runs;
  for (auto &i : symMap) {
    auto *undef = dyn_cast<Undefined>(i.second);
    if (!undef || !undef->isUsedInRegularObj || !isDefinableReference(undef))
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
    if (!undef || !undef->isUsedInRegularObj || !isDefinableReference(undef))
      continue;
    bool weak = undef->getWeakAlias();
    boundarySymbols.push_back({addSynthetic(undef->getName(), nullptr), weak});
  }
}

void SymbolTable::resolveRemainingUndefines(std::vector<Undefined *> &aliases) {
  llvm::TimeTraceScope timeScope("Resolve remaining undefined symbols");
  SmallPtrSet<Symbol *, 8> undefs;
  DenseMap<Symbol *, Symbol *> localImports;

  // A lookup below can enter a lazy symbol, so the loop visits a copy.
  SmallVector<Symbol *, 0> syms(llvm::make_second_range(symMap));
  for (Symbol *sym : syms) {
    auto *undef = dyn_cast<Undefined>(sym);
    if (!undef)
      continue;
    if (!sym->isUsedInRegularObj)
      continue;

    StringRef name = undef->getName();

    // A weak alias may have been resolved, so check for that. Under
    // -import-slots, an extern_weak X whose import an import library offered
    // is that import, as a shared object satisfies an ELF weak reference: a
    // function's import defines X, and imported data is reached as automatic
    // import reaches it, so that static data holding X's address holds the
    // address code reads through __imp_X.
    if (undef->getWeakAlias()) {
      if (ctx.config.importSlots) {
        auto *abs =
            dyn_cast_or_null<DefinedAbsolute>(undef->getDefinedWeakAlias());
        if (abs && abs->getVA() == 0 &&
            isa_and_nonnull<DefinedImportData>(impSymbol(name)) &&
            handleMinGWAutomaticImport(sym, name)) {
          continue;
        }
      }
      aliases.push_back(undef);
      continue;
    }

    // Under -import-slots, the record of a thread-local variable that the
    // image defines, which code reaches through its import pointer where
    // another image may define the variable, is the one the image writes.
    if (ctx.config.importSlots) {
      StringRef record = name;
      record.consume_front("__imp_");
      auto *var = record.ends_with("$tls")
                      ? dyn_cast_or_null<DefinedRegular>(
                            find(record.drop_back(strlen("$tls"))))
                      : nullptr;
      if (var && isThreadLocal(var) && getThreadLocalRecord(var) &&
          isa<Defined>(sym))
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
        // A DLL's own address-take of its default replaced by name reads a
        // cell, as every other image's does, which its fill writes.
        if (ctx.config.importSlots && ctx.config.dll &&
            (overridablesByName.contains(impName) || isOwnSlotDefault(impName)))
          cast<DefinedLocalImport>(sym)->getChunk()->overrideDefault = impName;
        // Its own address of a vague entity another image's instance may
        // preempt reads a cell too.
        else if (isPreemptibleVagueEntity(imp))
          cast<DefinedLocalImport>(sym)->getChunk()->vagueEntity = impName;
        // Under -import-slots, a pointer to the zero default of an absent
        // weak X imports nothing that the image defines.
        auto *abs = dyn_cast<DefinedAbsolute>(imp);
        if (!ctx.config.importSlots || !abs || abs->getVA() != 0 ||
            !isa<Undefined>(find(impName)))
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
  if (wasInserted && addIndexedLazy(s, name))
    wasInserted = false;
  // Only a weak external passes overrideLazy; any other reference is one a
  // weak fallback must not shadow (question 136).
  if (ctx.config.importSlots && !overrideLazy)
    stronglyReferenced.insert(s);
  if (wasInserted || (s->isLazy() && overrideLazy)) {
    // A weak external that consumes a lazy archive symbol keeps its member
    // reachable, so a strong reference can still pull it.
    if (ctx.config.importSlots && overrideLazy)
      if (auto *la = dyn_cast<LazyArchive>(s))
        weakFallbackLazy.try_emplace(s, la->file, la->sym);
    replaceSymbol<Undefined>(s, name);
    if (ctx.config.importSlots && name.starts_with("__imp_"))
      impUndefs.push_back(s);
    return s;
  }
  if (s->isLazy())
    forceLazy(s);
  if (ctx.config.importSlots && !overrideLazy)
    pullWeakFallbackLazy(s);
  return s;
}

// Loads the lazy archive member a weak external's consumption of s kept
// reachable, once a reference other than a weak external names s.
void SymbolTable::pullWeakFallbackLazy(Symbol *s) {
  auto it = weakFallbackLazy.find(s);
  if (it == weakFallbackLazy.end() || s->pendingArchiveLoad)
    return;
  ArchiveFile *f = it->second.first;
  Archive::Symbol sym = it->second.second;
  weakFallbackLazy.erase(it);
  s->pendingArchiveLoad = true;
  f->addMember(sym);
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
  if (wasInserted && addIndexedLazy(s, name))
    wasInserted = false;
  if (wasInserted) {
    replaceSymbol<LazyArchive>(s, f, sym);
    return;
  }
  auto *u = dyn_cast<Undefined>(s);
  if (!u || s->pendingArchiveLoad)
    return;
  if (u->weakAlias && !u->isECAlias(machine)) {
    // A weak external shadows the symbol. link.exe, and this linker without
    // -import-slots, drop the member, so a later strong reference resolves to
    // the weak fallback where ELF would pull the member. Under -import-slots
    // the member stays reachable: pull it now if a reference other than a weak
    // external has named the symbol, otherwise keep it for one that may
    // (question 136).
    if (!ctx.config.importSlots)
      return;
    if (!stronglyReferenced.count(s)) {
      weakFallbackLazy.try_emplace(s, f, sym);
      return;
    }
  }
  s->pendingArchiveLoad = true;
  f->addMember(sym);
}

void SymbolTable::addLazyObject(InputFile *f, StringRef n) {
  assert(f->lazy);
  if (isEC() && !checkLazyECPair<LazyObject>(this, n, f))
    return;
  auto [s, wasInserted] = insert(n, f);
  if (wasInserted && addIndexedLazy(s, n))
    wasInserted = false;
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
  if (wasInserted && addIndexedLazy(s, n))
    wasInserted = false;
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
  if (wasInserted || !isa<DefinedRegular>(s) || s->isWeak) {
    // Under -import-slots, the import whose thunk the definition replaces
    // keeps the thunk.
    if (auto *thunk = dyn_cast<DefinedImportThunk>(s);
        thunk && ctx.config.importSlots)
      thunk->wrappedSym->file->thunkSym = make<DefinedImportThunk>(
          ctx, thunk->getName(), thunk->wrappedSym, thunk->getChunk());
    replaceSymbol<DefinedRegular>(s, f, n, /*IsCOMDAT*/ false,
                                  /*IsExternal*/ true, sym, c, isWeak);
  } else if (!isWeak)
    reportDuplicate(s, f, c, sectionOffset);
  return s;
}

std::pair<DefinedRegular *, bool>
SymbolTable::addComdat(InputFile *f, StringRef n,
                       const coff_symbol_generic *sym) {
  auto [s, wasInserted] = insert(n, f);
  if (wasInserted || !isa<DefinedRegular>(s)) {
    // Under -import-slots, the import whose thunk the definition replaces
    // keeps the thunk, which serves only the function's address.
    if (auto *thunk = dyn_cast<DefinedImportThunk>(s);
        thunk && ctx.config.importSlots)
      thunk->wrappedSym->file->thunkSym = make<DefinedImportThunk>(
          ctx, thunk->getName(), thunk->wrappedSym, thunk->getChunk());
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
  if (Symbol *s = symMap.lookup(key))
    return s;
  auto [f, i] = findIndexed(key);
  if (!f)
    return nullptr;
  // The table keeps the name, which the archive holds for the link.
  Symbol *s = insert(f->getSymbolName(i)).first;
  replaceSymbol<LazyArchive>(s, f, f->getSymbol(i));
  return s;
}

std::pair<ArchiveFile *, uint32_t>
SymbolTable::findIndexed(CachedHashStringRef name) {
  for (ArchiveFile *f : indexedArchives)
    if (std::optional<uint32_t> i = f->findSymbol(name))
      return {f, *i};
  return {nullptr, 0};
}

bool SymbolTable::addIndexedLazy(Symbol *s, StringRef name) {
  if (indexedArchives.empty())
    return false;
  auto [f, i] = findIndexed(CachedHashStringRef(name));
  if (!f)
    return false;
  replaceSymbol<LazyArchive>(s, f, f->getSymbol(i));
  return true;
}

void SymbolTable::addIndexedArchive(ArchiveFile *f) {
  // An undefined symbol loads the member defining it, as addLazyArchive would
  // load it, and the members load in the order of the archive's symbols.
  auto isWaiting = [&](Symbol *s) {
    auto *u = dyn_cast<Undefined>(s);
    if (!u || s->pendingArchiveLoad)
      return false;
    // A weak external shadows the symbol. link.exe, and this linker without
    // -import-slots, leave it so and the member stays unloaded; under
    // -import-slots a reference other than the weak external still pulls the
    // member, as ELF pulls it for the strong reference (question 136).
    if (u->weakAlias && !u->isECAlias(machine))
      return ctx.config.importSlots && stronglyReferenced.contains(s);
    return true;
  };
  SmallVector<std::pair<uint32_t, Symbol *>, 0> loads;
  // Look the table's symbols up in the archive, or the archive's in the
  // table, whichever is fewer; a lookup in the table costs more.
  if (symMap.size() < f->getNumIndexed() * 4) {
    for (auto &[key, s] : symMap)
      if (isWaiting(s))
        if (std::optional<uint32_t> i = f->findSymbol(key))
          loads.push_back({*i, s});
  } else {
    for (uint32_t i : f->getIndexedSymbols())
      if (Symbol *s = symMap.lookup(CachedHashStringRef(f->getSymbolName(i)));
          s && isWaiting(s))
        loads.push_back({i, s});
  }
  llvm::sort(loads);
  for (auto [i, s] : loads) {
    s->pendingArchiveLoad = true;
    f->addMember(f->getSymbol(i));
  }
  indexedArchives.push_back(f);
}

std::vector<StringRef> SymbolTable::getArchiveSymbolNames(StringRef prefix) {
  std::vector<StringRef> names;
  for (ArchiveFile *f : indexedArchives)
    llvm::append_range(names, f->getSymbolNames(prefix));
  return names;
}

void SymbolTable::addIndexedLazies(StringRef prefix) {
  for (ArchiveFile *f : indexedArchives)
    for (StringRef name : f->getSymbolNames(prefix))
      find(name);
}

Symbol *SymbolTable::findUnderscore(StringRef name) {
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
  auto findByPrefix = [&](const Twine &t) -> Symbol * {
    std::string prefix = t.str();
    for (auto *s : syms)
      if (s->getName().starts_with(prefix))
        return s;
    // The table has an indexed archive's symbol once it is looked up.
    for (ArchiveFile *f : indexedArchives)
      for (StringRef name : f->getSymbolNames(prefix))
        if (Symbol *s = find(name))
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

  // The pattern can match the name of any archive's symbol.
  addIndexedLazies("");
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
    if (e == *existing)
      continue;
    if (e.name != existing->name) {
      // As link.exe does, an import library offers the symbol of an export
      // that takes another's name through EXPORTAS, importing that name. The
      // export table needs the name once, for the other export.
      if (!e.exportAs.empty()) {
        exportAsAliases.push_back(e);
      } else if (!existing->exportAs.empty()) {
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

void SymbolTable::assignExportOrdinals() {
  // Assign unique ordinals if default (= 0).
  uint32_t max = 0;
  for (Export &e : exports)
    max = std::max(max, (uint32_t)e.ordinal);
  // The ABI contract witnesses name one byte, through one ordinal.
  uint32_t witnessOrdinal = 0;
  for (Export &e : exports) {
    if (e.ordinal != 0)
      continue;
    if (!isContractWitness(ctx, dyn_cast_or_null<Defined>(e.sym)))
      e.ordinal = ++max;
    else if (witnessOrdinal)
      e.ordinal = witnessOrdinal;
    else
      e.ordinal = witnessOrdinal = ++max;
  }
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
  // The objects that LTO wrote list again the definitions of weak data that
  // the bitcode listed, which every later pass would visit twice.
  if (!weakData.empty()) {
    DenseSet<Symbol *> seen;
    llvm::erase_if(weakData, [&](Symbol *s) { return !seen.insert(s).second; });
  }
}

void SymbolTable::waitForLTOCleanup() {
  if (lto)
    lto->waitForLTOCleanup();
}

} // namespace lld::coff
