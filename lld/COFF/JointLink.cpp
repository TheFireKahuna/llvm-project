//===- JointLink.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A joint link links several images, its members, as one program, in three
// steps modelled on distributed ThinLTO. Each member's link line runs once with
// /lldjointscan, which resolves its symbols, writes a summary of them and stops
// before LTO. One /lldjointmerge reads the summaries, in the order given, and
// writes each member a fact file. Each member's final link is its own link line
// with /lldjoint naming its fact file, which LLD reads before any other input.
//
// The fact file is an archive of import members and one member of link-only
// records. A member that references what another member exports, or an
// observable vague entity another member keeps, imports it from that member.
// Of an entity that several members keep, the merge chooses one to keep it, a
// member the others already import from or else the first declared, never one
// that would start an initialization cycle; that member keeps it for the
// others, and they import it, as they would take a DLL's offer. The merge reads
// nothing but the summaries, so the final links depend on no order between
// them beyond that of the import libraries their link lines name, and a final
// link whose own inputs changed since its scan fails.
//
//===----------------------------------------------------------------------===//

#include "JointLink.h"
#include "COFFLinkerContext.h"
#include "Driver.h"
#include "InputFiles.h"
#include "Relax.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "VTableRuns.h"
#include "VagueEntities.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
#include <functional>
#include <list>
#include <map>
#include <numeric>
#include <set>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;

namespace lld::coff {
static constexpr StringLiteral identityPrefix = "__llvm_private$";

namespace {
// What a member that defines it gives up for another member's instance: an
// observable vague entity, as an importer gives it up for a DLL's offer; a
// strong vtable, type_info or type name that another member exports too, as
// one static library linked into both makes it; or the vtable of a class whose
// exact dynamic_casts compare vtable pointers, which only one copy makes decide
// for every member's objects.
enum EntityKind : int { VagueEntity, StrongEntity, ExactCastVTable };

// An entity a member defines: the names of its group, the entity's first, and
// the DLL, in lowercase, whose offer of it the member takes, if any.
struct Definition {
  std::vector<std::string> names;
  bool code = false;
  std::string offeredBy;
  int kind = VagueEntity;
};

// A vtable the member defines with default visibility, by its class and the
// name of its primary address point, where the class's vtable run starts.
struct Run {
  std::string cls, start;
};

struct NamedExport {
  std::string name;
  bool data = false;
};

struct Import {
  std::string name, dll;
};

// What the merge needs of a member's link, a function of its inputs alone.
struct Summary {
  std::string name;
  bool dll = false;
  int64_t machine = 0;
  std::vector<std::string> options;
  std::vector<NamedExport> exports;
  std::vector<Definition> definitions;
  std::vector<std::string> undefined;
  std::vector<Import> imports;
  std::vector<Run> runs;
  // What the member references through import entries it passes on, which the
  // loader grants as call targets where an export suppresses them.
  std::vector<std::string> addressTaken;
  // An executable's functions that an import library it links offers too,
  // each with that library's DLL; and the functions a DLL exports that such a
  // definition may take the place of.
  std::vector<Import> interposing;
  std::vector<std::string> interposable;
  // The witnesses of the contracts its inputs provide.
  std::vector<std::string> provides;
};

using Digest = std::array<uint8_t, 16>;

// A contract's witness name splits into the name of its identity, the prefix
// and locator, and its digest.
std::pair<StringRef, StringRef> splitWitness(StringRef w) {
  size_t pos = w.rfind('$');
  return {w.take_front(pos), w.drop_front(pos + 1)};
}

json::Value toJSON(const Definition &d) {
  return json::Object{{"names", d.names},
                      {"code", d.code},
                      {"offeredBy", d.offeredBy},
                      {"kind", d.kind}};
}
json::Value toJSON(const Run &r) {
  return json::Object{{"class", r.cls}, {"start", r.start}};
}
json::Value toJSON(const NamedExport &e) {
  return json::Object{{"name", e.name}, {"data", e.data}};
}
json::Value toJSON(const Import &i) {
  return json::Object{{"name", i.name}, {"dll", i.dll}};
}
json::Value toJSON(const Summary &s) {
  return json::Object{{"version", 1},
                      {"name", s.name},
                      {"dll", s.dll},
                      {"machine", s.machine},
                      {"options", s.options},
                      {"exports", s.exports},
                      {"definitions", s.definitions},
                      {"undefined", s.undefined},
                      {"imports", s.imports},
                      {"runs", s.runs},
                      {"addressTaken", s.addressTaken},
                      {"interposing", s.interposing},
                      {"interposable", s.interposable},
                      {"provides", s.provides}};
}

bool fromJSON(const json::Value &v, Definition &d, json::Path p) {
  json::ObjectMapper o(v, p);
  return o && o.map("names", d.names) && o.map("code", d.code) &&
         o.map("offeredBy", d.offeredBy) && o.mapOptional("kind", d.kind) &&
         d.kind >= VagueEntity && d.kind <= ExactCastVTable && !d.names.empty();
}
bool fromJSON(const json::Value &v, Run &r, json::Path p) {
  json::ObjectMapper o(v, p);
  return o && o.map("class", r.cls) && o.map("start", r.start);
}
bool fromJSON(const json::Value &v, NamedExport &e, json::Path p) {
  json::ObjectMapper o(v, p);
  return o && o.map("name", e.name) && o.map("data", e.data);
}
bool fromJSON(const json::Value &v, Import &i, json::Path p) {
  json::ObjectMapper o(v, p);
  return o && o.map("name", i.name) && o.map("dll", i.dll);
}
bool fromJSON(const json::Value &v, Summary &s, json::Path p) {
  json::ObjectMapper o(v, p);
  int64_t version = 0;
  if (!o || !o.map("version", version))
    return false;
  if (version != 1) {
    p.field("version").report("unknown version");
    return false;
  }
  return o.map("name", s.name) && o.map("dll", s.dll) &&
         o.map("machine", s.machine) && o.map("options", s.options) &&
         o.map("exports", s.exports) && o.map("definitions", s.definitions) &&
         o.map("undefined", s.undefined) && o.map("imports", s.imports) &&
         o.mapOptional("runs", s.runs) &&
         o.mapOptional("addressTaken", s.addressTaken) &&
         o.mapOptional("interposing", s.interposing) &&
         o.mapOptional("interposable", s.interposable) &&
         o.mapOptional("provides", s.provides);
}
} // namespace

// The options of a link line that the members of a joint link share: all but
// those that name the member's inputs and outputs or shape its image alone, and
// those that change nothing the link writes. Whether an image suppresses its
// exports as call targets is its own, as its exports are, so of /guard only the
// rest is shared.
static std::vector<std::string>
getLinkWideOptions(COFFLinkerContext &ctx, const opt::InputArgList &args) {
  static constexpr unsigned memberOptions[] = {
      OPT_INPUT, OPT_wholearchive_file, OPT_out, OPT_dll, OPT_entry,
      OPT_noentry, OPT_deffile, OPT_export, OPT_incl, OPT_pdb, OPT_implib,
      OPT_noimplib, OPT_map, OPT_map_file, OPT_lldmap, OPT_lldmap_file,
      OPT_base, OPT_stack, OPT_heap, OPT_version, OPT_subsystem, OPT_manifest,
      OPT_manifest_colon, OPT_manifestuac, OPT_manifestfile,
      OPT_manifestdependency, OPT_manifestinput, OPT_pdbaltpath, OPT_idlout,
      OPT_tlbout, OPT_lldjointscan, OPT_lldjoint, OPT_guard,
      // Options that change nothing written.
      OPT_nologo, OPT_verbose, OPT_verbose_all, OPT_errorlimit, OPT_threads,
      OPT_show_timing, OPT_summary, OPT_print_search_paths, OPT_time_trace_eq,
      OPT_time_trace_granularity_eq, OPT_color_diagnostics,
      OPT_no_color_diagnostics, OPT_color_diagnostics_eq, OPT_rsp_quoting,
      OPT_libpath, OPT_lldignoreenv, OPT_winsysroot, OPT_vctoolsdir,
      OPT_vctoolsversion, OPT_winsdkdir, OPT_winsdkversion, OPT_diasdkdir,
      OPT_vfsoverlay, OPT_lldltocache, OPT_lldltocachepolicy, OPT_reproduce,
      OPT_linkrepro, OPT_linkreprofullpathrsp, OPT_lldsavetemps,
      OPT_lldsavetemps_colon};
  std::vector<std::string> options;
  for (const opt::Arg *arg : args) {
    if (llvm::is_contained(memberOptions, arg->getOption().getID()))
      continue;
    std::string s = arg->getOption().getName().str();
    s += join(arg->getValues(), ",");
    options.push_back(StringRef(s).lower());
  }
  options.push_back(
      ("guard:" + Twine(ctx.config.guardCF & ~GuardCFLevel::ExportSuppress))
          .str());
  return options;
}

// Adds to names the shared names of what bitcode file f defines in the COMDAT
// of d and in that of its guard, as a native object's group would hold them.
static void addBitcodeGroup(SymbolTable &symtab, BitcodeFile *f, Symbol *d,
                            std::vector<std::string> &names) {
  ArrayRef<Symbol *> syms = f->getSymbols();
  Symbol *guard = symtab.find(getGuardName(d->getName()));
  SmallVector<int, 2> comdats;
  for (auto [i, objSym] : llvm::enumerate(f->obj->symbols()))
    if ((syms[i] == d || (guard && syms[i] == guard)) &&
        !objSym.isUndefined() && objSym.getComdatIndex() != -1)
      comdats.push_back(objSym.getComdatIndex());
  for (auto [i, objSym] : llvm::enumerate(f->obj->symbols()))
    if (syms[i] != d && !objSym.isUndefined() && syms[i]->getFile() == f &&
        llvm::is_contained(comdats, objSym.getComdatIndex()))
      names.push_back(symtab.getSharedName(syms[i]).str());
}

// What bitcode file f defines as d.
static const lto::InputFile::Symbol *getBitcodeSymbol(BitcodeFile *f,
                                                      Symbol *d) {
  ArrayRef<Symbol *> syms = f->getSymbols();
  ArrayRef<lto::InputFile::Symbol> objSyms = f->obj->symbols();
  for (size_t i = 0, e = objSyms.size(); i != e; ++i)
    if (syms[i] == d && !objSyms[i].isUndefined())
      return &objSyms[i];
  return nullptr;
}

// Whether bitcode file f's symbol o is a definition that no COMDAT copy
// replaces: one outside a COMDAT that deduplicates.
static bool isStrongDefinition(BitcodeFile *f,
                               const lto::InputFile::Symbol &o) {
  return !o.isUndefined() && !o.isWeak() &&
         (o.getComdatIndex() == -1 ||
          f->obj->getComdatTable()[o.getComdatIndex()].second ==
              Comdat::NoDeduplicate);
}

// Whether d is a definition that no COMDAT copy replaces: a section that admits
// no duplicate, or a bitcode definition outside a COMDAT that deduplicates.
static bool isStrongDefinition(DefinedRegular *d) {
  if (isa<ObjFile>(d->getFile()))
    return !d->isCOMDAT ||
           d->getChunk()->selection == IMAGE_COMDAT_SELECT_NODUPLICATES;
  auto *f = dyn_cast<BitcodeFile>(d->getFile());
  const lto::InputFile::Symbol *o = f ? getBitcodeSymbol(f, d) : nullptr;
  return o && isStrongDefinition(f, *o);
}

// Whether d is a function the image defines with default visibility, which no
// COMDAT copy replaces.
static bool isStrongFunction(DefinedRegular *d) {
  if (auto *f = dyn_cast<BitcodeFile>(d->getFile())) {
    const lto::InputFile::Symbol *o = getBitcodeSymbol(f, d);
    return o && o->isExecutable() &&
           o->getVisibility() == GlobalValue::DefaultVisibility &&
           isStrongDefinition(f, *o);
  }
  return isa<ObjFile>(d->getFile()) && d->getCOFFSymbol().isExternal() &&
         (d->getChunk()->header->Characteristics & IMAGE_SCN_CNT_CODE) &&
         isStrongDefinition(d) && isDefaultVisibility(d);
}

// The functions an executable defines with default visibility, which no COMDAT
// copy replaces, that an import library its link names offers too, each with
// the library's DLL: those an ELF executable exports because a shared object
// it links defines them, and whose definitions so take the shared object's
// place in its own references.
static std::vector<std::pair<DefinedRegular *, StringRef>>
getInterposingDefinitions(COFFLinkerContext &ctx) {
  SymbolTable &symtab = ctx.symtab;
  std::vector<std::pair<DefinedRegular *, StringRef>> defs;
  if (ctx.config.dll)
    return defs;
  std::vector<DefinedRegular *> functions;
  for (ObjFile *f : ctx.objFileInstances)
    if (&f->symtab == &symtab)
      for (Symbol *sym : f->getSymbols())
        if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
            d && d->getFile() == f && isStrongFunction(d))
          functions.push_back(d);
  for (BitcodeFile *f : symtab.bitcodeFileInstances) {
    ArrayRef<Symbol *> syms = f->getSymbols();
    for (auto [i, o] : llvm::enumerate(f->obj->symbols()))
      if (auto *d = dyn_cast_or_null<DefinedRegular>(syms[i]);
          d && d->getFile() == f && o.isExecutable() &&
          o.getVisibility() == GlobalValue::DefaultVisibility &&
          isStrongDefinition(f, o))
        functions.push_back(d);
  }
  for (DefinedRegular *d : functions) {
    Symbol *imp = symtab.find(("__imp_" + d->getName()).str());
    StringRef dll;
    if (auto *data = dyn_cast_or_null<DefinedImportData>(imp))
      dll = data->file->thunkSym ? data->getDLLName() : StringRef();
    else
      dll = getLazyImportDLL(imp, IMPORT_CODE);
    if (!dll.empty())
      defs.push_back({d, dll});
  }
  llvm::sort(defs, [](auto &a, auto &b) {
    return std::make_pair(a.first->getName(), a.second) <
           std::make_pair(b.first->getName(), b.second);
  });
  defs.erase(llvm::unique(defs), defs.end());
  return defs;
}

// Whether d is a default-visibility COMDAT copy, of which a link keeps any.
static bool isDefaultVisibilityCopy(DefinedRegular *d) {
  if (isa<ObjFile>(d->getFile()))
    return d->isCOMDAT &&
           d->getChunk()->selection != IMAGE_COMDAT_SELECT_NODUPLICATES &&
           isDefaultVisibility(d);
  auto *f = dyn_cast<BitcodeFile>(d->getFile());
  const lto::InputFile::Symbol *o = f ? getBitcodeSymbol(f, d) : nullptr;
  return o && o->getComdatIndex() != -1 &&
         f->obj->getComdatTable()[o->getComdatIndex()].second !=
             Comdat::NoDeduplicate &&
         o->getVisibility() == GlobalValue::DefaultVisibility;
}

// Adds the names of d's group: d's first, then those of the rest of its COMDAT
// and its guard's, or for a vtable outside a COMDAT, its address points'.
static bool addGroupNames(SymbolTable &symtab, DefinedRegular *d,
                          std::vector<std::string> &names) {
  names.push_back(symtab.getSharedName(d).str());
  if (isa<ObjFile>(d->getFile())) {
    SmallVector<SectionChunk *, 4> chunks;
    SmallVector<DefinedRegular *, 4> members;
    symtab.collectWeakDataGroup(d, chunks, members, /*liveOnly=*/false);
    for (DefinedRegular *m : ArrayRef(members).drop_front())
      names.push_back(symtab.getSharedName(m).str());
  } else if (auto *f = dyn_cast<BitcodeFile>(d->getFile())) {
    addBitcodeGroup(symtab, f, d, names);
    std::string prefix = (d->getName() + "$ap").str();
    if (names.size() == 1 && d->getName().starts_with("_ZTV"))
      for (Symbol *sym : f->getSymbols())
        if (sym && sym->getFile() == f && isa<DefinedRegular>(sym) &&
            sym->getName().starts_with(prefix))
          names.push_back(sym->getName().str());
  } else {
    return false;
  }
  llvm::sort(std::next(names.begin()), names.end());
  names.erase(llvm::unique(names), names.end());
  return true;
}

// The entities the image defines that another image's may replace: the
// observable vague entities it defines with default visibility, as an
// importer's link matches them to a DLL's offers (COMDAT data with its group
// and guard, the record's name for a thread-local, and functions whose address
// is significant); the strong vtables, type_info and type names it exports;
// and the vtables of classes whose exact casts compare vtable pointers.
static std::vector<Definition> getDefinitions(SymbolTable &symtab) {
  auto getOfferer = [&](StringRef name, bool code) -> std::string {
    auto *imp = dyn_cast_or_null<DefinedImportData>(
        symtab.find(("__imp_" + name).str()));
    if (!imp || symtab.ignoresOffersOf(imp) || (code && !imp->file->thunkSym))
      return {};
    return imp->getDLLName().lower();
  };
  std::vector<Definition> defs;
  DenseSet<Symbol *> seen;
  for (Symbol *sym : symtab.weakData) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !seen.insert(d).second || !isDefaultVisibility(d))
      continue;
    Definition def;
    if (!addGroupNames(symtab, d, def.names))
      continue;
    def.offeredBy = getOfferer(def.names.front(), /*code=*/false);
    defs.push_back(std::move(def));
  }
  for (Symbol *sym : symtab.weakFunctions) {
    auto *d = dyn_cast<DefinedRegular>(sym);
    if (!d || !seen.insert(d).second)
      continue;
    Definition &def = defs.emplace_back();
    def.names.push_back(d->getName().str());
    def.code = true;
    def.offeredBy = getOfferer(d->getName(), /*code=*/true);
  }
  auto add = [&](DefinedRegular *d, int kind) {
    Definition def;
    def.kind = kind;
    if (seen.insert(d).second && addGroupNames(symtab, d, def.names))
      defs.push_back(std::move(def));
  };
  for (const Export &e : symtab.exports) {
    StringRef name = e.name;
    if (e.isPrivate || !e.forwardTo.empty() || name.contains('$') ||
        !(name.starts_with("_ZTV") || name.starts_with("_ZTI") ||
          name.starts_with("_ZTS")))
      continue;
    if (auto *d = dyn_cast_or_null<DefinedRegular>(symtab.find(name));
        d && isStrongDefinition(d))
      add(d, StrongEntity);
  }
  for (StringRef name : symtab.exactCastVTables)
    if (auto *d = dyn_cast_or_null<DefinedRegular>(symtab.find(name));
        d && isDefaultVisibilityCopy(d))
      add(d, ExactCastVTable);
  llvm::sort(defs, [](const Definition &a, const Definition &b) {
    return a.names.front() < b.names.front();
  });
  return defs;
}

// Adds the names of the functions whose import entries file passes on, which
// the loader grants as call targets: those its guard metadata lists, every
// import an object without that metadata references, and those its static
// data holds, which become in-place slots.
static void addAddressTakenImports(ObjFile *file, StringSet<> &names) {
  ArrayRef<Symbol *> syms = file->getSymbols();
  for (SectionChunk *c : file->getGuardIATChunks()) {
    ArrayRef<uint8_t> data = c->getContents();
    for (size_t i = 0; i + 4 <= data.size(); i += 4) {
      uint32_t index = support::endian::read32le(data.data() + i);
      StringRef name = index < syms.size() && syms[index]
                           ? syms[index]->getName()
                           : StringRef();
      if (name.consume_front("__imp_"))
        names.insert(name);
    }
  }
  uint16_t addr64 = file->getMachineType() == ARM64 ? IMAGE_REL_ARM64_ADDR64
                                                    : IMAGE_REL_AMD64_ADDR64;
  for (Chunk *chunk : file->getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(chunk);
    if (!sc || sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
      continue;
    bool code = sc->header->Characteristics & IMAGE_SCN_MEM_EXECUTE;
    for (const coff_relocation &rel : sc->getRelocs()) {
      Symbol *sym = file->getSymbol(rel.SymbolTableIndex);
      if (!sym)
        continue;
      StringRef name = sym->getName();
      if (name.consume_front("__imp_")) {
        if (!file->hasGuardCF())
          names.insert(name);
      } else if (!code && rel.Type == addr64 &&
                 (isa<Undefined>(sym) || isa<DefinedImportThunk>(sym))) {
        names.insert(name);
      }
    }
  }
}

// Adds what bitcode file f's definitions reference other than by a call, as
// its module summary records it; nothing for a module without a summary.
static void addSummaryRefs(BitcodeFile *f, DenseSet<GlobalValue::GUID> &refs) {
  Expected<std::unique_ptr<ModuleSummaryIndex>> index =
      getModuleSummaryIndex(f->mb);
  if (!index) {
    consumeError(index.takeError());
    return;
  }
  for (const auto &[guid, info] : **index)
    for (const std::unique_ptr<GlobalValueSummary> &summary :
         info.getSummaryList())
      for (ValueInfo vi : summary->refs())
        refs.insert(vi.getGUID());
}

// The name other images import from the member by, as its import library's.
static std::string getMemberName(COFFLinkerContext &ctx) {
  if (ctx.config.importName.empty())
    return sys::path::filename(ctx.config.outputFile).str();
  SmallString<128> name(ctx.config.importName);
  if (!sys::path::has_extension(name))
    sys::path::replace_extension(name, ctx.config.dll ? ".dll" : ".exe");
  return std::string(name);
}

static Summary getSummary(COFFLinkerContext &ctx,
                          const opt::InputArgList &args) {
  SymbolTable &symtab = ctx.symtab;
  Summary s;
  s.name = getMemberName(ctx);
  s.dll = ctx.config.dll;
  s.machine = ctx.config.machine;
  s.options = getLinkWideOptions(ctx, args);
  for (const Export &e : symtab.exports)
    if (!e.isPrivate && !e.noname)
      s.exports.push_back({(!e.exportAs.empty() ? e.exportAs
                            : e.extName.empty() ? e.name
                                                : e.extName)
                               .str(),
                           e.data});
  llvm::sort(s.exports, [](const NamedExport &a, const NamedExport &b) {
    return a.name < b.name;
  });
  s.definitions = getDefinitions(symtab);
  // What nothing defines yet; an import-form reference to a definition is the
  // image's own.
  symtab.forEachSymbol([&](Symbol *sym) {
    auto *u = dyn_cast<Undefined>(sym);
    if (!u || u->getWeakAlias())
      return;
    StringRef name = u->getName();
    if (name.consume_front("__imp_") &&
        isa_and_nonnull<Defined>(symtab.findVagueEntity(name)))
      return;
    s.undefined.push_back(name.str());
  });
  llvm::sort(s.undefined);
  s.undefined.erase(llvm::unique(s.undefined), s.undefined.end());
  // Imports but those of offers of what the image defines, which name their
  // DLLs.
  StringSet<> defined;
  for (const Definition &d : s.definitions)
    defined.insert_range(d.names);
  for (ImportFile *f : ctx.importFileInstances) {
    if (!f->impSym || &f->symtab != &symtab)
      continue;
    StringRef name = f->impSym->getName().drop_front(strlen("__imp_"));
    if (!defined.contains(name))
      s.imports.push_back({name.str(), StringRef(f->dllName).lower()});
  }
  llvm::sort(s.imports, [](const Import &a, const Import &b) {
    return std::tie(a.name, a.dll) < std::tie(b.name, b.dll);
  });
  // Only a vtable with default visibility has address-point names; its first
  // starts its class's run.
  std::map<std::string, uint64_t> starts;
  symtab.forEachSymbol([&](Symbol *sym) {
    StringRef name = sym->getName();
    size_t pos = name.rfind("$ap");
    uint64_t offset;
    if (!isa<DefinedRegular>(sym) || !name.starts_with("_ZTV") ||
        pos == StringRef::npos ||
        name.substr(pos + strlen("$ap")).getAsInteger(10, offset))
      return;
    auto [it, inserted] =
        starts.try_emplace(name.slice(strlen("_ZTV"), pos).str(), offset);
    it->second = std::min(it->second, offset);
  });
  for (auto &[cls, offset] : starts)
    s.runs.push_back({cls, ("_ZTV" + cls + "$ap" + Twine(offset)).str()});
  // An executable's functions that a DLL it links exports too, and the
  // functions a DLL defines and exports that such a definition may take the
  // place of: all but overridable defaults, which the override table replaces.
  for (auto [d, dll] : getInterposingDefinitions(ctx))
    s.interposing.push_back({d->getName().str(), dll.lower()});
  if (s.dll)
    for (const Export &e : symtab.exports)
      if (auto *d = dyn_cast_or_null<DefinedRegular>(e.sym);
          d && !e.isPrivate && !e.noname && !e.data && e.forwardTo.empty() &&
          e.extName.empty() && e.exportAs.empty() &&
          !symtab.getOverrideOffer(e) && isStrongFunction(d))
        s.interposable.push_back(e.name.str());
  llvm::sort(s.interposable);
  s.interposable.erase(llvm::unique(s.interposable), s.interposable.end());
  for (auto &[file, witnesses] : ctx.contracts.provides)
    for (StringRef w : witnesses)
      s.provides.push_back(w.str());
  llvm::sort(s.provides);
  s.provides.erase(llvm::unique(s.provides), s.provides.end());
  // Of what the member imports or has yet to resolve, the functions whose
  // addresses it takes: an object says so in its guard metadata and data, and
  // bitcode in its module summary's references, where it has one. Without
  // CFG, nothing is suppressed or granted.
  if (!(ctx.config.guardCF & GuardCFLevel::CF))
    return s;
  StringSet<> taken;
  for (ObjFile *f : ctx.objFileInstances)
    if (&f->symtab == &symtab)
      addAddressTakenImports(f, taken);
  DenseSet<GlobalValue::GUID> refs;
  for (BitcodeFile *f : symtab.bitcodeFileInstances)
    addSummaryRefs(f, refs);
  auto addTaken = [&](StringRef name) {
    if (taken.contains(name) ||
        refs.contains(GlobalValue::getGUIDAssumingExternalLinkage(name)))
      s.addressTaken.push_back(name.str());
  };
  for (const std::string &name : s.undefined)
    addTaken(name);
  for (const Import &i : s.imports)
    addTaken(i.name);
  llvm::sort(s.addressTaken);
  s.addressTaken.erase(llvm::unique(s.addressTaken), s.addressTaken.end());
  return s;
}

// What a member resolved through another member's import library depends on
// when that member's link wrote it, so it is taken as unresolved: the merge
// resolves it again, and the member's final link imports it through its fact
// file, read first. An offer taken from a member is the merge's to choose too.
static void canonicalize(Summary &s, const StringSet<> &members) {
  for (const Import &i : s.imports)
    if (members.contains(i.dll))
      s.undefined.push_back(i.name);
  llvm::erase_if(s.imports,
                 [&](const Import &i) { return members.contains(i.dll); });
  for (Definition &d : s.definitions)
    if (members.contains(d.offeredBy))
      d.offeredBy.clear();
  llvm::sort(s.undefined);
  s.undefined.erase(llvm::unique(s.undefined), s.undefined.end());
}

static Digest getDigest(const Summary &s) {
  std::string buf;
  raw_string_ostream os(buf);
  os << toJSON(s);
  XXH128_hash_t hash = xxh3_128bits(arrayRefFromStringRef(buf));
  Digest d;
  support::endian::write64le(d.data(), hash.low64);
  support::endian::write64le(d.data() + 8, hash.high64);
  return d;
}

static std::string getFactsSymbol(StringRef member) {
  return "__llvm_joint$" + member.lower();
}

void writeJointSummary(COFFLinkerContext &ctx, const opt::InputArgList &args) {
  llvm::TimeTraceScope timeScope("Write joint-link summary");
  std::error_code ec;
  raw_fd_ostream os(ctx.config.jointScan, ec, sys::fs::OF_None);
  if (ec) {
    Err(ctx) << "cannot open " << ctx.config.jointScan << ": " << ec.message();
    return;
  }
  os << formatv("{0:2}", toJSON(getSummary(ctx, args))) << "\n";
}

static constexpr StringLiteral scanMarkPrefix = "__llvm_jointscan$";

bool mayWriteJointScanImportLibrary(StringRef path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> mb = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!mb)
    return true;
  Expected<std::unique_ptr<Archive>> archive = Archive::create(**mb);
  if (!archive) {
    consumeError(archive.takeError());
    return false;
  }
  for (const Archive::Symbol &sym : (*archive)->symbols())
    if (sym.getName().starts_with(scanMarkPrefix))
      return true;
  return false;
}

void addJointScanMark(COFFLinkerContext &ctx, StringRef dllName,
                      std::vector<NewArchiveMember> &members,
                      std::vector<uint8_t> &buffer) {
  members.push_back(createLinkRecordMember(
      ctx, dllName, (scanMarkPrefix + dllName.lower()).str(), {}, buffer));
}

void addJointScanOffers(COFFLinkerContext &ctx) {
  SymbolTable &symtab = ctx.symtab;
  DenseSet<StringRef> exported;
  for (Export &e : symtab.exports)
    exported.insert(e.extName.empty() ? e.name : e.extName);
  for (const Definition &d : getDefinitions(symtab)) {
    if (!d.offeredBy.empty())
      continue;
    for (const std::string &name : d.names) {
      if (!exported.insert(name).second)
        continue;
      Export e;
      e.name = saver().save(name);
      e.data = !d.code;
      e.source = ExportSource::ExportAll;
      symtab.exports.push_back(e);
    }
  }
}

void applyJointFacts(COFFLinkerContext &ctx, const opt::InputArgList &args) {
  llvm::TimeTraceScope timeScope("Apply joint-link facts");
  std::string name = getMemberName(ctx);
  auto *lazy =
      dyn_cast_or_null<LazyArchive>(ctx.symtab.find(getFactsSymbol(name)));
  if (!lazy) {
    Err(ctx) << ctx.config.jointFacts << ": holds no joint-link facts for "
             << name;
    return;
  }
  std::optional<Digest> digest;
  StringSet<> members;
  std::vector<StringRef> kept, privateExports, runs, unsuppressed;
  StringRef identity;
  std::vector<std::pair<StringRef, std::vector<StringRef>>> preempted;
  std::vector<std::pair<StringRef, std::vector<StringRef>>> interposed;
  std::vector<StringRef> interposing;
  struct Sibling {
    StringRef name, identity;
    bool needed;
    std::vector<StringRef> grants, provides;
  };
  std::vector<Sibling> siblings;
  forEachLinkRecordGroup(
      lazy->getMemberBuffer(), ctx.config.machine,
      [&](uint64_t kind, ArrayRef<uint8_t> payload) {
        if (kind != LinkRecordJointFacts || digest)
          return;
        DataExtractor data(payload, /*IsLittleEndian=*/true);
        DataExtractor::Cursor c(0);
        Digest d;
        data.getU8(c, d.data(), d.size());
        for (uint64_t i = 0, n = data.getULEB128(c); c && i != n; ++i)
          members.insert(data.getBytes(c, data.getULEB128(c)));
        auto getStrings = [&](std::vector<StringRef> &v) {
          for (uint64_t i = 0, n = data.getULEB128(c); c && i != n; ++i)
            v.push_back(data.getBytes(c, data.getULEB128(c)));
        };
        getStrings(kept);
        getStrings(privateExports);
        for (uint64_t i = 0, n = data.getULEB128(c); c && i != n; ++i) {
          auto &[from, group] = preempted.emplace_back();
          from = data.getBytes(c, data.getULEB128(c));
          getStrings(group);
        }
        getStrings(runs);
        getStrings(unsuppressed);
        identity = data.getBytes(c, data.getULEB128(c));
        for (uint64_t i = 0, n = data.getULEB128(c); c && i != n; ++i) {
          Sibling &sib = siblings.emplace_back();
          sib.name = data.getBytes(c, data.getULEB128(c));
          sib.identity = data.getBytes(c, data.getULEB128(c));
          sib.needed = data.getU8(c);
          getStrings(sib.grants);
          getStrings(sib.provides);
        }
        for (uint64_t i = 0, n = data.getULEB128(c); c && i != n; ++i) {
          auto &[from, names] = interposed.emplace_back();
          from = data.getBytes(c, data.getULEB128(c));
          getStrings(names);
        }
        getStrings(interposing);
        if (c && data.eof(c))
          digest = d;
        consumeError(c.takeError());
      });
  if (!digest) {
    Err(ctx) << ctx.config.jointFacts << ": the joint-link facts for " << name
             << " are malformed";
    return;
  }
  Summary s = getSummary(ctx, args);
  canonicalize(s, members);
  if (getDigest(s) != *digest) {
    Err(ctx) << name
             << ": joint-link facts are stale (inputs changed since the "
                "scan); rerun the scan and the merge";
    return;
  }
  for (const auto &m : members)
    ctx.jointMembers.insert(m.getKey());
  // What the member keeps for the others stays live, and so is offered or
  // exported, and LTO keeps it.
  SymbolTable &symtab = ctx.symtab;
  for (StringRef entity : kept)
    if (auto *d = dyn_cast_or_null<Defined>(symtab.findVagueEntity(entity))) {
      ctx.config.gcroot.push_back(d);
      d->isUsedInRegularObj = true;
    }
  for (StringRef name : privateExports) {
    Export e;
    e.name = name;
    e.data = true;
    e.isPrivate = true;
    e.source = ExportSource::Export;
    e.sym = symtab.addGCRoot(name);
    e.symbolName = name;
    symtab.exports.push_back(e);
  }
  // A group the member gives up for another's is preempted by its imports
  // once liveness is decided; LTO keeps it until then, as it keeps data a DLL
  // offers.
  for (auto &[from, group] : preempted) {
    for (StringRef name : group) {
      // The owner's import library, on the link line, may hold the import.
      Symbol *imp = symtab.find(("__imp_" + name).str());
      if (auto *data = dyn_cast_or_null<DefinedImportData>(imp)) {
        if (!data->getDLLName().equals_insensitive(from))
          Err(ctx) << name << " is imported from " << data->getDLLName()
                   << ", but the joint-link facts give it up for " << from
                   << "'s";
      } else if (!imp || isa<Undefined>(imp) || imp->isLazy()) {
        ctx.driver.addFile(make<ImportFile>(
            ctx, makeShortImport(ctx.config.machine, from, name, IMPORT_DATA,
                                 IMPORT_NAME)));
      }
      if (Symbol *sym = symtab.find(name))
        sym->isUsedInRegularObj = true;
    }
    if (Symbol *sym = group.empty() ? nullptr : symtab.find(group.front()))
      symtab.jointPreempted.insert(sym);
  }
  for (StringRef start : runs)
    ctx.jointRuns[start.slice(strlen("_ZTV"), start.rfind("$ap"))] = start;
  // The functions other members take the address of are not suppressed. A
  // member that takes them leaves their entries out of the loader's grants.
  ctx.jointUnsuppressed.insert_range(unsuppressed);
  // The image exports the identity the merge gave it, and imports that of
  // each member it imports from where it relies on what that identity covers:
  // a binding no import library holds, the functions a DLL does not suppress,
  // or that the member keeps none of the entities the image keeps its own
  // copies of. A member linked from other facts then fails to load beside it.
  // The image's code is checked against the contracts those members provide,
  // which their identities cover too, in place of their witnesses.
  ctx.jointPrivateIdentity = identity;
  for (Sibling &sib : siblings) {
    COFFLinkerContext::JointSibling &js = ctx.jointSiblings[sib.name.lower()];
    js.identity =
        saver().save("__imp_" + sib.identity + "$" + sib.name.lower());
    for (StringRef w : sib.provides) {
      auto [name, digest] = splitWitness(w);
      js.provides[name] = digest;
    }
    if (sib.needed)
      symtab.addGCRoot(js.identity);
    for (StringRef n : sib.grants)
      ctx.jointGrants.push_back({sib.name, n});
  }
  // A function the executables that load the member define too takes the
  // place of the member's own definition in its own references, which reach
  // it through an import from the executable, unless -Bsymbolic binds them
  // locally; the definition stays the member's export. The executables export
  // their definitions for those references.
  for (auto &[from, names] : interposed) {
    if (ctx.config.bsymbolic != Configuration::Bsymbolic::None)
      break;
    for (StringRef name : names) {
      auto *d = dyn_cast_or_null<DefinedRegular>(symtab.find(name));
      if (!d)
        continue;
      Symbol *imp = symtab.find(("__imp_" + name).str());
      if (auto *data = dyn_cast_or_null<DefinedImportData>(imp)) {
        if (!data->getDLLName().equals_insensitive(from))
          Err(ctx) << name << " is imported from " << data->getDLLName()
                   << ", but the joint-link facts give it up for " << from
                   << "'s";
      } else if (!imp || isa<Undefined>(imp) || imp->isLazy()) {
        ctx.driver.addFile(make<ImportFile>(
            ctx, makeShortImport(ctx.config.machine, from, name, IMPORT_CODE,
                                 IMPORT_NAME)));
      }
      symtab.jointInterposed.insert(d);
    }
  }
  for (StringRef name : interposing) {
    Export e;
    e.name = name;
    e.isPrivate = true;
    e.source = ExportSource::Export;
    e.sym = symtab.addGCRoot(name);
    e.symbolName = name;
    symtab.exports.push_back(e);
  }
}

void warnInterposingDefinitions(COFFLinkerContext &ctx) {
  SymbolTable &symtab = ctx.symtab;
  if (!ctx.config.importSlots || ctx.config.dll)
    return;
  symtab.readOverrideRecords();
  // A DLL that links under -import-slots, whose import library records its
  // entities, is linked to bind as ELF does; others keep PE's binding. A DLL
  // of the joint link the merge decided for, and a default the override table
  // replaces, are not reported.
  for (auto [d, dll] : getInterposingDefinitions(ctx)) {
    StringRef name = d->getName();
    if (ctx.jointMembers.contains(dll.lower()) ||
        symtab.overrideSlotFacts.contains(name) ||
        symtab.defaultsByName.contains(name) ||
        symtab.overridablesByName.contains(name) ||
        !symtab.find((vagueRecordPrefix + dll.lower()).str()))
      continue;
    Warn(ctx) << d->getFile() << ": " << name << " is also exported by " << dll
              << ", whose own references keep its definition; link them "
                 "jointly for this one to take its place";
  }
}

void collectJointGrants(COFFLinkerContext &ctx) {
  for (auto [dll, name] : ctx.jointGrants)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(
            ctx.symtab.find(("__imp_" + name).str()));
        imp && imp->file->thunkSym && !imp->file->isDelayLoaded() &&
        imp->getDLLName().equals_insensitive(dll))
      ctx.jointGranted.insert(imp);
}

namespace {
// What a member imports from another through its fact file.
struct FactImport {
  std::string name;
  unsigned from;
  bool code;
  bool operator<(const FactImport &o) const {
    return std::tie(from, name) < std::tie(o.from, o.name);
  }
  bool operator==(const FactImport &o) const {
    return from == o.from && name == o.name;
  }
};

// The vague entity a group name belongs to, and the members that keep it.
struct Entity {
  std::string name;
  bool code = false;
  int kind = VagueEntity;
  SmallVector<std::pair<unsigned, const Definition *>, 2> keepers;
  bool referenced = false;
};

class Merge {
public:
  Merge(COFFLinkerContext &ctx, std::vector<Summary> &summaries)
      : ctx(ctx), summaries(summaries), edges(summaries.size()),
        imports(summaries.size()), kept(summaries.size()),
        privateExports(summaries.size()), preempted(summaries.size()),
        runExports(summaries.size()), runImports(summaries.size()),
        unsuppressed(summaries.size()), grants(summaries.size()),
        interposed(summaries.size()), interposing(summaries.size()),
        identities(summaries.size()), needs(summaries.size()) {}
  void run(StringRef dir);

private:
  bool reaches(unsigned from, unsigned to) const;
  void addImport(unsigned to, StringRef name, unsigned from, bool code);
  int chooseOwner(const Entity &e);
  bool checkLinkLines(const std::vector<SmallVector<unsigned, 4>> &libs);
  void checkContracts();
  std::string getIdentity(unsigned m) const;
  void writeFacts(StringRef dir, unsigned m, const Digest &digest);

  COFFLinkerContext &ctx;
  std::vector<Summary> &summaries;
  StringMap<unsigned> byName;
  std::vector<SmallSetVector<unsigned, 4>> edges;
  std::vector<std::vector<FactImport>> imports;
  std::vector<std::vector<std::string>> kept;
  // What a member exports for the others under names no import library holds;
  // the groups it gives up for an owner's, which an importer's preemption of a
  // vague entity does not, each with the owner; and the classes whose vtable
  // runs it exports for the members that import the class from it.
  std::vector<std::vector<std::string>> privateExports;
  std::vector<std::vector<std::pair<unsigned, std::vector<std::string>>>>
      preempted;
  std::vector<std::set<std::string>> runExports;
  std::vector<std::set<std::pair<unsigned, const Run *>>> runImports;
  // The functions a DLL exports that other members take the address of, which
  // it does not suppress; and those of each DLL that a member takes, whose
  // entries it leaves out of the loader's grants.
  std::vector<std::set<std::string>> unsuppressed;
  std::vector<std::map<unsigned, std::vector<std::string>>> grants;
  // The functions a DLL exports whose definitions the executables that load
  // it define too, with the executable whose its own references import; and
  // the functions an executable exports for those references.
  std::vector<std::map<unsigned, std::vector<std::string>>> interposed;
  std::vector<std::set<std::string>> interposing;
  // The identity export of each member another imports from, and the members
  // whose identities each member relies on.
  std::vector<std::string> identities;
  std::vector<std::set<unsigned>> needs;
};
} // namespace

bool Merge::reaches(unsigned from, unsigned to) const {
  SmallVector<unsigned, 8> stack = {from};
  BitVector seen(summaries.size());
  seen.set(from);
  while (!stack.empty()) {
    unsigned m = stack.pop_back_val();
    if (m == to)
      return true;
    for (unsigned n : edges[m])
      if (!seen.test(n)) {
        seen.set(n);
        stack.push_back(n);
      }
  }
  return false;
}

void Merge::addImport(unsigned to, StringRef name, unsigned from, bool code) {
  imports[to].push_back({name.str(), from, code});
  edges[to].insert(from);
}

// The member to keep e for the others: one every other keeper already imports
// from, else the first declared, never one that would put a keeper and itself
// on a new cycle of imports; -1 if none can.
int Merge::chooseOwner(const Entity &e) {
  auto acyclic = [&](unsigned c) {
    return llvm::all_of(e.keepers, [&](auto &k) {
      return k.first == c || !reaches(c, k.first) || reaches(k.first, c);
    });
  };
  auto importedByAll = [&](unsigned c) {
    return llvm::all_of(e.keepers, [&](auto &k) {
      return k.first == c || reaches(k.first, c);
    });
  };
  for (const auto &k : e.keepers)
    if (summaries[k.first].dll && acyclic(k.first) && importedByAll(k.first))
      return k.first;
  for (const auto &k : e.keepers)
    if (summaries[k.first].dll && acyclic(k.first))
      return k.first;
  return -1;
}

// A member whose final link reads another's import library links after it, so
// two members whose link lines name each other's could link in either order.
bool Merge::checkLinkLines(const std::vector<SmallVector<unsigned, 4>> &libs) {
  enum : uint8_t { Unvisited, Active, Done };
  std::vector<uint8_t> state(summaries.size(), Unvisited);
  SmallVector<unsigned, 8> path;
  bool ok = true;
  std::function<void(unsigned)> visit = [&](unsigned m) {
    state[m] = Active;
    path.push_back(m);
    for (unsigned n : libs[m]) {
      if (state[n] == Active && ok) {
        auto cycle =
            ArrayRef(path).drop_front(llvm::find(path, n) - path.begin());
        std::string names;
        for (unsigned i : cycle)
          names += (names.empty() ? "" : ", ") + summaries[i].name;
        Err(ctx) << "the link lines of " << names
                 << " name each other's import libraries; name none of a "
                    "member's in another's, and the joint link binds them";
        ok = false;
      } else if (state[n] == Unvisited) {
        visit(n);
      }
    }
    path.pop_back();
    state[m] = Done;
  };
  for (unsigned m = 0; m != summaries.size(); ++m)
    if (state[m] == Unvisited)
      visit(m);
  return ok;
}

// The contracts of a member and of each member it imports from agree, as
// those of one image's objects must.
void Merge::checkContracts() {
  std::vector<StringMap<StringRef>> digests(summaries.size());
  for (unsigned m = 0; m != summaries.size(); ++m)
    for (StringRef w : summaries[m].provides) {
      auto [identity, digest] = splitWitness(w);
      digests[m][identity] = digest;
    }
  std::set<std::pair<unsigned, unsigned>> checked;
  for (unsigned m = 0; m != summaries.size(); ++m)
    for (unsigned from : edges[m]) {
      if (!checked.insert({std::min(m, from), std::max(m, from)}).second)
        continue;
      for (StringRef w : summaries[m].provides) {
        auto [identity, digest] = splitWitness(w);
        StringRef other = digests[from].lookup(identity);
        if (other.empty() || other == digest)
          continue;
        bool data = identity.starts_with("__abis$");
        Err(ctx) << "ABI contract mismatch for "
                 << (data ? "the type of the datum with locator "
                          : "the type with locator ")
                 << identity.substr(identity.find('$') + 1) << ": "
                 << summaries[m].name << " and " << summaries[from].name
                 << " were compiled with different layouts";
      }
    }
}

// The first 64 bits of the SHA-256 of the format, the machine and what a
// member offers the others beyond its import library, in lowercase
// hexadecimal: the contracts it provides, the entities it keeps for them, the
// names it exports for them alone, and the functions it does not suppress.
std::string Merge::getIdentity(unsigned m) const {
  SHA256 hash;
  hash.update(("1 " + Twine(summaries[m].machine) + "\n").str());
  auto add = [&](char kind, const auto &names) {
    std::vector<StringRef> sorted(names.begin(), names.end());
    llvm::sort(sorted);
    for (StringRef name : sorted)
      hash.update((Twine(kind) + " " + name + "\n").str());
  };
  add('c', summaries[m].provides);
  add('k', kept[m]);
  add('p', privateExports[m]);
  add('r', runExports[m]);
  add('u', unsuppressed[m]);
  std::array<uint8_t, 32> digest = hash.final();
  return (identityPrefix +
          toHex(ArrayRef(digest).take_front(8), /*LowerCase=*/true))
      .str();
}

void Merge::writeFacts(StringRef dir, unsigned m, const Digest &digest) {
  const Summary &s = summaries[m];
  std::list<std::vector<uint8_t>> buffers;
  std::vector<NewArchiveMember> members;
  llvm::sort(imports[m]);
  imports[m].erase(llvm::unique(imports[m]), imports[m].end());
  for (const FactImport &i : imports[m])
    members.push_back(createImportMember(
        summaries[i.from].name, ctx.config.machine, i.name,
        i.code ? IMPORT_CODE : IMPORT_DATA, buffers.emplace_back()));
  // The identity of each member the member imports from, under a name of that
  // member alone, imported as code so that a delay-load descriptor can hold
  // it.
  for (unsigned from : edges[m])
    members.push_back(createImportMember(
        summaries[from].name, ctx.config.machine,
        identities[from] + "$" + StringRef(summaries[from].name).lower(),
        IMPORT_CODE, buffers.emplace_back(), identities[from]));
  // The three words of each vtable run the member imports, under the names
  // the exporter's import library would give them.
  static const StringRef runPrefixes[] = {
      VTableRangeStartPrefix, VTableRangeEndPrefix, VTableRangeMaskPrefix};
  for (const auto &[from, run] : runImports[m]) {
    std::string dll = StringRef(summaries[from].name).lower();
    for (int w : {0, 1, 2})
      members.push_back(createImportMember(
          summaries[from].name, ctx.config.machine,
          (runPrefixes[w] + run->cls + "$" + dll).str(), IMPORT_DATA,
          buffers.emplace_back(),
          w == 0 ? StringRef(run->start)
                 : getVTableRunWordName(run->cls, w == 1 ? 'e' : 'm')));
  }

  std::string payload(digest.begin(), digest.end());
  raw_string_ostream os(payload);
  auto putString = [&](StringRef str) {
    encodeULEB128(str.size(), os);
    os << str;
  };
  encodeULEB128(summaries.size(), os);
  for (const Summary &t : summaries)
    putString(StringRef(t.name).lower());
  llvm::sort(kept[m]);
  kept[m].erase(llvm::unique(kept[m]), kept[m].end());
  encodeULEB128(kept[m].size(), os);
  for (const std::string &name : kept[m])
    putString(name);
  encodeULEB128(privateExports[m].size(), os);
  for (const std::string &name : privateExports[m])
    putString(name);
  encodeULEB128(preempted[m].size(), os);
  for (const auto &[from, group] : preempted[m]) {
    putString(summaries[from].name);
    encodeULEB128(group.size(), os);
    for (const std::string &name : group)
      putString(name);
  }
  encodeULEB128(runExports[m].size(), os);
  for (const std::string &start : runExports[m])
    putString(start);
  encodeULEB128(unsuppressed[m].size(), os);
  for (const std::string &name : unsuppressed[m])
    putString(name);
  putString(identities[m]);
  SmallVector<unsigned, 4> siblings(edges[m].begin(), edges[m].end());
  llvm::sort(siblings);
  encodeULEB128(siblings.size(), os);
  for (unsigned from : siblings) {
    putString(summaries[from].name);
    putString(identities[from]);
    os << char(needs[m].count(from));
    auto it = grants[m].find(from);
    encodeULEB128(it == grants[m].end() ? 0 : it->second.size(), os);
    if (it != grants[m].end())
      for (const std::string &name : it->second)
        putString(name);
    encodeULEB128(summaries[from].provides.size(), os);
    for (const std::string &w : summaries[from].provides)
      putString(w);
  }
  encodeULEB128(interposed[m].size(), os);
  for (const auto &[from, names] : interposed[m]) {
    putString(summaries[from].name);
    encodeULEB128(names.size(), os);
    for (const std::string &name : names)
      putString(name);
  }
  encodeULEB128(interposing[m].size(), os);
  for (const std::string &name : interposing[m])
    putString(name);
  members.push_back(createLinkRecordMember(ctx, s.name, getFactsSymbol(s.name),
                                           {{LinkRecordJointFacts, payload}},
                                           buffers.emplace_back()));

  Expected<std::unique_ptr<MemoryBuffer>> buf = writeArchiveToBuffer(
      members, SymtabWritingMode::NormalSymtab, Archive::K_COFF,
      /*Deterministic=*/true, /*Thin=*/false);
  if (!buf) {
    Err(ctx) << s.name << ": cannot write its joint-link facts: "
             << toString(buf.takeError());
    return;
  }
  // As an import library is, a fact file is replaced only when it changes, so
  // the final links of members it does not concern are not run again.
  SmallString<128> path(dir);
  sys::path::append(path, s.name + ".jfacts");
  ErrorOr<std::unique_ptr<MemoryBuffer>> old = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (old && (*old)->getBuffer() == (*buf)->getBuffer())
    return;
  if (old)
    old->reset();
  if (Error e = writeToOutput(path, [&](raw_ostream &out) {
        out << (*buf)->getBuffer();
        return Error::success();
      }))
    Err(ctx) << "cannot write " << path << ": " << toString(std::move(e));
}

void Merge::run(StringRef dir) {
  unsigned n = summaries.size();
  for (unsigned m = 0; m != n; ++m)
    if (!byName.try_emplace(StringRef(summaries[m].name).lower(), m).second)
      Err(ctx) << summaries[m].name << " is a member of the joint link twice";
  // The members' link lines agree on every option of the program as a whole.
  for (unsigned m = 1; m != n; ++m) {
    const Summary &s = summaries[m], &first = summaries[0];
    if (s.machine != first.machine) {
      Err(ctx) << s.name << " is for another machine than " << first.name;
      continue;
    }
    auto [a, b] = std::mismatch(s.options.begin(), s.options.end(),
                                first.options.begin(), first.options.end());
    if (a != s.options.end() || b != first.options.end())
      Err(ctx) << s.name << "'s link line "
               << (a != s.options.end() ? "has /" + *a : "lacks /" + *b)
               << ", which " << first.name << "'s "
               << (a != s.options.end() ? "lacks" : "has")
               << "; a joint link's members share every option but those of "
                  "their own inputs and outputs";
  }
  if (errorCount())
    return;
  ctx.config.machine = static_cast<MachineTypes>(summaries[0].machine);

  StringSet<> memberNames;
  for (auto &[name, m] : byName)
    memberNames.insert(name);
  std::vector<SmallVector<unsigned, 4>> libs(n);
  for (unsigned m = 0; m != n; ++m)
    for (const Import &i : summaries[m].imports)
      if (auto it = byName.find(i.dll);
          it != byName.end() && !llvm::is_contained(libs[m], it->second))
        libs[m].push_back(it->second);
  if (!checkLinkLines(libs))
    return;
  std::vector<Digest> digests;
  for (Summary &s : summaries) {
    canonicalize(s, memberNames);
    digests.push_back(getDigest(s));
  }

  // The entities the members keep, by every name of their groups.
  std::vector<Entity> entities;
  StringMap<unsigned> entityOf;
  for (unsigned m = 0; m != n; ++m)
    for (const Definition &d : summaries[m].definitions) {
      if (!d.offeredBy.empty())
        continue;
      auto [it, inserted] =
          entityOf.try_emplace(d.names.front(), entities.size());
      unsigned e = it->second;
      if (inserted) {
        entities.push_back({d.names.front(), d.code, d.kind, {}, false});
        for (const std::string &name : ArrayRef(d.names).drop_front())
          entityOf.try_emplace(name, e);
      }
      entities[e].keepers.push_back({m, &d});
    }
  auto keeperDef = [](const Entity &e, unsigned m) -> const Definition * {
    for (auto &[k, d] : e.keepers)
      if (k == m)
        return d;
    return nullptr;
  };
  StringMap<std::pair<unsigned, bool>> exporters;
  for (unsigned m = 0; m != n; ++m)
    for (const NamedExport &e : summaries[m].exports)
      exporters.try_emplace(e.name, m, e.data);

  // References to what another member exports, an executable's only to a DLL.
  auto bindExport = [&](unsigned m, StringRef name) {
    auto it = exporters.find(name);
    if (it == exporters.end())
      return;
    auto [from, data] = it->second;
    if (from != m && (summaries[from].dll || summaries[m].dll))
      addImport(m, name, from, !data);
  };
  for (unsigned m = 0; m != n; ++m)
    for (const std::string &name : summaries[m].undefined) {
      auto it = entityOf.find(name);
      if (it == entityOf.end())
        bindExport(m, name);
      else if (!keeperDef(entities[it->second], m))
        entities[it->second].referenced = true;
    }

  // One member keeps each entity that several keep, or that another
  // references, for the others, in the order of the entities' names.
  std::vector<int> owners(entities.size(), -1);
  std::vector<unsigned> order(entities.size());
  std::iota(order.begin(), order.end(), 0);
  llvm::sort(order, [&](unsigned a, unsigned b) {
    return entities[a].name < entities[b].name;
  });
  for (unsigned i : order) {
    const Entity &e = entities[i];
    if (e.keepers.size() < 2 && !e.referenced)
      continue;
    if (llvm::none_of(e.keepers,
                      [&](auto &k) { return summaries[k.first].dll; }))
      continue;
    int owner = chooseOwner(e);
    if (owner < 0) {
      std::string names;
      for (auto &[k, d] : e.keepers)
        if (summaries[k].dll)
          names += (names.empty() ? "" : ", ") + summaries[k].name;
      Err(ctx) << "no member of the joint link can keep " << e.name
               << " for the others without an initialization cycle: " << names;
      continue;
    }
    owners[i] = owner;
    const Definition *od = keeperDef(e, owner);
    llvm::append_range(kept[owner], od->names);
    for (auto &[k, d] : e.keepers) {
      if (k == unsigned(owner))
        continue;
      // A vague entity meets the owner's instance as any importer meets a
      // DLL's offer. A vtable or a strong type_info or type name is given up
      // by name, its imports added once the member's link has resolved as its
      // scan did, since their archive members would satisfy references that
      // load the member's own definition from a library.
      if (e.kind != VagueEntity) {
        edges[k].insert(owner);
        preempted[k].push_back({unsigned(owner), {}});
      }
      if (e.kind == ExactCastVTable)
        needs[k].insert(owner);
      for (const std::string &name : d->names)
        if (llvm::is_contained(od->names, name)) {
          if (e.kind == VagueEntity)
            addImport(k, name, owner, e.code);
          else
            preempted[k].back().second.push_back(name);
        }
    }
    // A vtable copy's owner exports it for the others, but not for the images
    // that link its import library.
    if (e.kind == ExactCastVTable &&
        llvm::none_of(summaries[owner].exports,
                      [&](const NamedExport &x) { return x.name == e.name; }))
      llvm::append_range(privateExports[owner], od->names);
  }

  // References to an entity import it from its owner, or else from a member
  // that exports it.
  for (unsigned m = 0; m != n; ++m)
    for (const std::string &name : summaries[m].undefined) {
      auto it = entityOf.find(name);
      if (it == entityOf.end() || keeperDef(entities[it->second], m))
        continue;
      int owner = owners[it->second];
      if (owner >= 0 &&
          llvm::is_contained(keeperDef(entities[it->second], owner)->names,
                             name))
        addImport(m, name, owner, entities[it->second].code);
      else
        bindExport(m, name);
    }
  if (errorCount())
    return;

  // A function that an executable defines and that a DLL member, whose import
  // library its link line names, exports takes the DLL's definition's place in
  // every member's references, the DLL's own included, as an ELF executable's
  // definition preempts a shared object's. The DLL's references bind once for
  // every process that loads it, so only where every executable of the set
  // that loads it defines the function; otherwise the DLL keeps its own, as
  // where nothing is declared.
  std::map<std::pair<unsigned, StringRef>, SmallVector<unsigned, 2>> claims;
  for (unsigned e = 0; e != n; ++e)
    for (const Import &i : summaries[e].interposing)
      if (auto it = byName.find(i.dll);
          !summaries[e].dll && it != byName.end() &&
          std::binary_search(summaries[it->second].interposable.begin(),
                             summaries[it->second].interposable.end(),
                             i.name))
        claims[{it->second, i.name}].push_back(e);
  for (auto &claim : claims) {
    auto [d, name] = claim.first;
    ArrayRef<unsigned> exes = claim.second;
    SmallVector<unsigned, 2> loaders;
    for (unsigned e = 0; e != n; ++e)
      if (!summaries[e].dll && reaches(e, d))
        loaders.push_back(e);
    auto other = llvm::find_if(
        loaders, [&](unsigned e) { return !llvm::is_contained(exes, e); });
    if (other != loaders.end()) {
      Warn(ctx) << summaries[exes.front()].name << " defines " << name
                << ", which " << summaries[d].name << " exports, but "
                << summaries[*other].name << ", which loads "
                << summaries[d].name << " too, does not; every member keeps "
                << "binding " << name << " to " << summaries[d].name << "'s";
      continue;
    }
    if (loaders.empty())
      continue;
    interposed[d][loaders.front()].push_back(name.str());
    for (unsigned e : loaders)
      interposing[e].insert(name.str());
    for (unsigned m = 0; m != n; ++m)
      for (FactImport &i : imports[m])
        if (i.from == d && i.name == name) {
          i.from = loaders.front();
          edges[m].insert(loaders.front());
        }
  }

  // A member's vtable guard finds the objects another member builds where it
  // reads the other's vtable run: for each class whose vtable or type_info it
  // imports from a DLL member that has the class's vtable, it imports the run,
  // which that member exports.
  auto addRun = [&](unsigned m, StringRef cls, unsigned from) {
    if (!summaries[from].dll ||
        (!cls.consume_front("_ZTV") && !cls.consume_front("_ZTI")))
      return;
    cls = cls.take_front(cls.find("$ap"));
    if (cls.contains('$'))
      return;
    ArrayRef<Run> runs = summaries[from].runs;
    auto it = llvm::lower_bound(
        runs, cls, [](const Run &r, StringRef c) { return r.cls < c; });
    if (it != runs.end() && it->cls == cls) {
      runImports[m].insert({from, &*it});
      runExports[from].insert(it->start);
    }
  };
  for (unsigned m = 0; m != n; ++m) {
    for (const FactImport &i : imports[m])
      addRun(m, i.name, i.from);
    for (const auto &[from, group] : preempted[m])
      for (const std::string &name : group)
        addRun(m, name, from);
  }

  // A function that a member imports from a DLL member and takes the address
  // of need not wait for the loader to grant it as a call target: the DLL
  // does not suppress it, and the member leaves its entry out of the grants.
  // One that delay-loads the DLL still asks the loader.
  for (unsigned m = 0; m != n; ++m) {
    ArrayRef<std::string> taken = summaries[m].addressTaken;
    for (const FactImport &i : imports[m]) {
      std::string delayLoad =
          "delayload:" + StringRef(summaries[i.from].name).lower();
      if (!i.code || !summaries[i.from].dll ||
          llvm::is_contained(summaries[m].options, delayLoad) ||
          !std::binary_search(taken.begin(), taken.end(), i.name))
        continue;
      unsuppressed[i.from].insert(i.name);
      grants[m][i.from].push_back(i.name);
    }
  }
  for (auto &g : grants)
    for (auto &[from, names] : g) {
      llvm::sort(names);
      names.erase(llvm::unique(names), names.end());
    }

  checkContracts();
  if (errorCount())
    return;
  // A member relies on the identity of each member it imports from a binding
  // no import library holds (a private name, or the words of a vtable run its
  // checks may read) or functions that member does not suppress; and one that
  // keeps its own copy of an entity, on that of every member it imports from,
  // whose identity changes when it starts to keep the entity for the others.
  for (unsigned m = 0; m != n; ++m) {
    bool keepsOwn = llvm::any_of(summaries[m].definitions, [&](auto &d) {
      if (!d.offeredBy.empty())
        return false;
      int owner = owners[entityOf.lookup(d.names.front())];
      return owner < 0 || unsigned(owner) == m;
    });
    for (unsigned from : edges[m])
      if (keepsOwn || grants[m].count(from))
        needs[m].insert(from);
    for (const FactImport &i : imports[m])
      if (llvm::is_contained(privateExports[i.from], i.name))
        needs[m].insert(i.from);
    for (const auto &[from, run] : runImports[m])
      needs[m].insert(from);
  }
  for (unsigned m = 0; m != n; ++m)
    for (unsigned from : edges[m])
      if (identities[from].empty())
        identities[from] = getIdentity(from);

  if (std::error_code ec = sys::fs::create_directories(dir)) {
    Err(ctx) << "cannot create " << dir << ": " << ec.message();
    return;
  }
  for (unsigned m = 0; m != n; ++m)
    writeFacts(dir, m, digests[m]);
}

void mergeJointLink(COFFLinkerContext &ctx, const opt::InputArgList &args) {
  llvm::TimeTraceScope timeScope("Joint-link merge");
  StringRef dir = args.getLastArgValue(OPT_out);
  if (dir.empty())
    Err(ctx) << "/lldjointmerge needs /out, the directory of the fact files";
  std::vector<Summary> summaries;
  for (const opt::Arg *arg : args.filtered(OPT_INPUT)) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> mb = MemoryBuffer::getFile(
        arg->getValue(), /*IsText=*/true, /*RequiresNullTerminator=*/false);
    if (!mb) {
      Err(ctx) << "cannot open " << arg->getValue() << ": "
               << mb.getError().message();
      continue;
    }
    Expected<json::Value> v = json::parse((*mb)->getBuffer());
    json::Path::Root root(arg->getValue());
    Summary &s = summaries.emplace_back();
    if (!v || !fromJSON(*v, s, root))
      Err(ctx) << arg->getValue() << ": not a joint-link summary: "
               << (v ? toString(root.getError()) : toString(v.takeError()));
  }
  if (summaries.empty())
    Err(ctx) << "/lldjointmerge needs the summaries of the members";
  if (errorCount())
    return;
  Merge(ctx, summaries).run(dir);
}
} // namespace lld::coff

namespace lld::coff {
static std::string getLayoutSymbol(StringRef dll) {
  return "__llvm_layout$" + dll.lower();
}

// A DLL member whose data another member may reach through an anchor exports
// its identity, which names its layout: the addresses of its data exports. The
// name is decided once the layout is, so it takes the export's place in the
// export table now with as many characters.
bool addJointIdentityExport(COFFLinkerContext &ctx) {
  // The identity the merge gives a member that others import from names the
  // read-only byte the image's contract witnesses name, through their ordinal.
  if (!ctx.jointPrivateIdentity.empty()) {
    Export e;
    e.name = ctx.jointPrivateIdentity;
    e.sym = getContractWitnessByte(ctx);
    e.data = true;
    e.isPrivate = true;
    e.source = ExportSource::Export;
    ctx.symtab.exports.push_back(e);
  }
  if (ctx.config.jointFacts.empty() || !ctx.config.dll || ctx.config.noimplib)
    return false;
  Export *first = nullptr;
  for (Export &e : ctx.symtab.exports)
    if (e.data && e.forwardTo.empty() && isa_and_nonnull<DefinedRegular>(e.sym) &&
        (!first || e.name < first->name))
      first = &e;
  if (!first)
    return false;
  ctx.jointIdentity =
      make<std::string>((identityPrefix + std::string(16, '0')).str());
  Export e;
  e.name = *ctx.jointIdentity;
  e.sym = first->sym;
  e.data = true;
  e.isPrivate = true;
  e.source = ExportSource::Export;
  ctx.symtab.exports.push_back(e);
  return true;
}

void finalizeJointIdentity(COFFLinkerContext &ctx) {
  if (!ctx.jointIdentity)
    return;
  for (const Export &e : ctx.symtab.exports)
    if (e.data && !e.noname && e.forwardTo.empty() &&
        e.exportName != *ctx.jointIdentity)
      if (auto *d = dyn_cast_or_null<DefinedRegular>(e.sym))
        ctx.jointLayout.push_back({e.exportName, d->getRVA()});
  llvm::sort(ctx.jointLayout);
  // The first 64 bits of the SHA-256 of the format, the machine and the
  // exports with their addresses, in lowercase hexadecimal.
  SHA256 hash;
  hash.update(("1 " + Twine(ctx.config.machine) + "\n").str());
  for (auto &[name, rva] : ctx.jointLayout)
    hash.update((name + " " + Twine(rva) + "\n").str());
  std::array<uint8_t, 32> digest = hash.final();
  std::string hex = toHex(ArrayRef(digest).take_front(8), /*LowerCase=*/true);
  // The export table's string names this storage, whose length stays.
  ctx.jointIdentity->replace(identityPrefix.size(), 16, hex);
}

void addJointLayoutRecord(COFFLinkerContext &ctx, StringRef dllName,
                          std::vector<NewArchiveMember> &members,
                          std::vector<uint8_t> &buffer) {
  if (!ctx.jointIdentity)
    return;
  std::string payload;
  raw_string_ostream os(payload);
  auto putString = [&](StringRef s) {
    encodeULEB128(s.size(), os);
    os << s;
  };
  putString(*ctx.jointIdentity);
  encodeULEB128(ctx.jointLayout.size(), os);
  for (auto &[name, rva] : ctx.jointLayout) {
    putString(name);
    encodeULEB128(rva, os);
  }
  members.push_back(createLinkRecordMember(ctx, dllName,
                                           getLayoutSymbol(dllName),
                                           {{LinkRecordJointLayout, payload}},
                                           buffer));
}

namespace {
// A member DLL's layout, as its import library records it.
struct Layout {
  StringRef identity;
  StringMap<uint32_t> rvas;
};
} // namespace

static const Layout *getLayout(COFFLinkerContext &ctx, StringRef dll,
                               StringMap<std::optional<Layout>> &cache) {
  auto [it, inserted] = cache.try_emplace(dll.lower());
  if (!inserted)
    return it->second ? &*it->second : nullptr;
  auto *lazy =
      dyn_cast_or_null<LazyArchive>(ctx.symtab.find(getLayoutSymbol(dll)));
  if (!lazy)
    return nullptr;
  Layout layout;
  bool ok = false;
  forEachLinkRecordGroup(
      lazy->getMemberBuffer(), ctx.config.machine,
      [&](uint64_t kind, ArrayRef<uint8_t> payload) {
        if (kind != LinkRecordJointLayout || ok)
          return;
        DataExtractor data(payload, /*IsLittleEndian=*/true);
        DataExtractor::Cursor c(0);
        layout.identity = data.getBytes(c, data.getULEB128(c));
        for (uint64_t i = 0, n = data.getULEB128(c); c && i != n; ++i) {
          StringRef name = data.getBytes(c, data.getULEB128(c));
          layout.rvas[name] = data.getULEB128(c);
        }
        ok = c && data.eof(c) && layout.identity.starts_with(identityPrefix);
        consumeError(c.takeError());
      });
  if (!ok)
    return nullptr;
  it->second = std::move(layout);
  return &*it->second;
}

// The import of data from another image that the load of its pointer at off
// reads, with the register it fills: on x86-64 mov r64, [rip + __imp_X], whose
// displacement is at off, and on ARM64 ldr x, [x, :lo12:__imp_X] at off.
static DefinedImportData *getLoadedImport(SectionChunk *sc, uint32_t off,
                                          unsigned *reg) {
  ArrayRef<uint8_t> data = sc->getContents();
  bool arm64 = sc->getArch() == Triple::aarch64;
  if (arm64) {
    if (off % 4 || off + 4 > data.size() ||
        (support::endian::read32le(data.data() + off) & 0xFFC00000) != 0xF9400000)
      return nullptr;
    *reg = support::endian::read32le(data.data() + off) & 31;
  } else {
    if (off < 3 || off + 4 > data.size() || (data[off - 3] & 0xF8) != 0x48 ||
        data[off - 2] != 0x8B || (data[off - 1] & 0xC7) != 0x05)
      return nullptr;
    *reg = (data[off - 3] & 4) << 1 | (data[off - 1] >> 3 & 7);
  }
  uint16_t type =
      arm64 ? IMAGE_REL_ARM64_PAGEOFFSET_12L : IMAGE_REL_AMD64_REL32;
  for (const coff_relocation &rel : sc->getRelocs())
    if (rel.VirtualAddress == off && rel.Type == type) {
      auto *imp = dyn_cast_or_null<DefinedImportData>(
          sc->file->getSymbol(rel.SymbolTableIndex));
      if (imp && imp->file->hdr->getType() == IMPORT_DATA &&
          !imp->file->isDelayLoaded())
        return imp;
      return nullptr;
    }
  return nullptr;
}

bool bindJointAnchors(COFFLinkerContext &ctx) {
  if (ctx.jointMembers.empty() ||
      (ctx.config.machine != AMD64 && ctx.config.machine != ARM64))
    return false;
  llvm::TimeTraceScope timeScope("Bind joint-link anchors");
  StringMap<std::optional<Layout>> layouts;
  MapVector<StringRef, StringRef> identities;
  for (ObjFile *file : ctx.objFileInstances) {
    ArrayRef<ObjFile::LinkAnchor> anchors = file->getLinkAnchors();
    if (anchors.empty())
      continue;
    // A load whose pointer may lead to the image's own copy keeps its form.
    DenseSet<std::pair<uint32_t, uint32_t>> valueOnly;
    for (const ObjFile::LinkValueOnly &v : file->getValueOnlyLoads())
      valueOnly.insert({v.section, v.offset});
    uint32_t done = UINT32_MAX, doneSection = UINT32_MAX;
    for (const ObjFile::LinkAnchor &a : anchors) {
      if (a.section == doneSection && a.offset == done)
        continue;
      SectionChunk *sc = file->getSectionChunk(a.section);
      unsigned reg, anchorReg;
      DefinedImportData *imp, *anchor;
      if (!sc || valueOnly.contains({a.section, a.offset}) ||
          valueOnly.contains({a.section, a.load}) ||
          !(imp = getLoadedImport(sc, a.offset, &reg)) ||
          !(anchor = getLoadedImport(sc, a.load, &anchorReg)) ||
          anchorReg != a.reg || anchor == imp ||
          (ctx.config.machine == AMD64 && (a.reg & 7) == 4) ||
          !imp->getDLLName().equals_insensitive(anchor->getDLLName()) ||
          !ctx.jointMembers.contains(imp->getDLLName().lower()))
        continue;
      const Layout *layout = getLayout(ctx, imp->getDLLName(), layouts);
      if (!layout)
        continue;
      auto to = layout->rvas.find(imp->getExternalName());
      auto from = layout->rvas.find(anchor->getExternalName());
      if (to == layout->rvas.end() || from == layout->rvas.end())
        continue;
      // An ARM64 add or sub takes 12 bits, shifted or not.
      int64_t delta = int64_t(to->second) - from->second;
      uint64_t size = delta < 0 ? -delta : delta;
      if (ctx.config.machine == ARM64 && size > 0xFFF &&
          (size & 0xFFF || size > 0xFFF000))
        continue;
      ctx.jointAnchors.push_back({sc, a.offset, imp, int32_t(delta), a.reg});
      identities.try_emplace(imp->getDLLName(), layout->identity);
      done = a.offset;
      doneSection = a.section;
    }
  }
  // A member whose loads derive a DLL's pointers imports its identity, so that
  // a DLL linked with another layout fails to load beside it.
  bool added = false;
  for (auto &[dll, identity] : identities) {
    std::string name = ("__imp_" + identity).str();
    if (!ctx.symtab.find(name)) {
      ctx.driver.addFile(make<ImportFile>(
          ctx, makeShortImport(ctx.config.machine, dll, saver().save(identity),
                               IMPORT_DATA, IMPORT_NAME)));
      added = true;
    }
    ctx.symtab.addGCRoot(saver().save(name));
  }
  return added;
}

// An import that only anchored loads read is left out of the import tables.
void leaveOutAnchoredImports(COFFLinkerContext &ctx) {
  DenseMap<Symbol *, unsigned> anchored;
  for (const COFFLinkerContext::JointAnchor &a : ctx.jointAnchors)
    if (a.chunk->live)
      ++anchored[a.imp];
  if (anchored.empty())
    return;
  DenseMap<Symbol *, unsigned> reads;
  for (ObjFile *file : ctx.objFileInstances)
    for (Chunk *c : file->getChunks())
      if (auto *sc = dyn_cast<SectionChunk>(c); sc && sc->live)
        for (const coff_relocation &rel : sc->getRelocs())
          if (Symbol *s = file->getSymbol(rel.SymbolTableIndex);
              s && anchored.count(s))
            ++reads[s];
  for (auto &[s, n] : anchored)
    if (reads.lookup(s) == n)
      cast<DefinedImportData>(s)->file->live = false;
}

void addJointAnchorEdits(COFFLinkerContext &ctx) {
  for (const COFFLinkerContext::JointAnchor &a : ctx.jointAnchors) {
    if (!a.chunk->live)
      continue;
    // The shorter lea, with an 8-bit distance, where the chunk is resized;
    // on ARM64, an add or sub in the ldr's place.
    bool arm64 = a.chunk->getArch() == Triple::aarch64;
    uint16_t size = arm64 || (a.chunk->file->isCovered(a.chunk) &&
                              isInt<8>(a.delta))
                        ? 4
                        : 7;
    RelaxEdit e(arm64 ? a.offset : a.offset - 3, arm64 ? 4 : 7, size,
                RelaxEdit::Anchor);
    e.base = a.reg;
    e.delta = a.delta;
    addEdit(a.chunk, e);
  }
}
} // namespace lld::coff
