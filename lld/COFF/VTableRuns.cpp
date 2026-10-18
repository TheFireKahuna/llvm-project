//===- VTableRuns.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The vtable records say where each vtable's primary address point is, which
// class it belongs to and that class's primary base. From them this file lays
// out the vtables of a class and of every class derived from it through primary
// bases as one run of equal slots, so that a vtable pointer is one of the
// class's exactly when its offset from the run's start is a multiple of the
// slot size below the run's extent. A guard thunk tests a virtual call's vtable
// pointer that way, and a checked downcast tests the target class's run.
//
// The slot size, a power of two of at least a cache line, is the run's and
// covers its largest vtable, so that between two slots' address points no other
// address point and no filler falls on a slot boundary. A vtable too large for
// any run's slot, or merged with others into one chunk, is left out, and the
// calls on its objects take the slow path.
//
//===----------------------------------------------------------------------===//

#include "VTableRuns.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Driver.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::COFF;
using namespace lld;
using namespace lld::coff;

// A vtable larger than this, with its address point, is left out of the runs:
// giving a run a slot this large to hold it would waste a page or more on every
// other class of the run. Vtables are far smaller in practice.
static const uint64_t maxSlot = 1 << 16;

namespace {
// A 4-byte read-only word holding a run's stride less one, which a guard thunk
// loads to mask a vtable pointer's offset, since a constant cannot be an
// immediate on COFF without a relocation that adds the image base.
class MaskChunk : public NonSectionChunk {
public:
  explicit MaskChunk(uint32_t mask) : mask(mask) { setAlignment(4); }
  size_t getSize() const override { return 4; }
  void writeTo(uint8_t *buf) const override {
    llvm::support::endian::write32le(buf, mask);
  }
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }
  StringRef getSectionName() const override { return ".rdata"; }

private:
  uint32_t mask;
};

// Three zero words in read-only data, the owner's run of a class whose owner
// the image does not bind: its extent is zero, so every pointer misses it.
class EmptyOwnerRunChunk : public NonSectionChunk {
public:
  EmptyOwnerRunChunk() { setAlignment(8); }
  size_t getSize() const override { return 24; }
  void writeTo(uint8_t *buf) const override { memset(buf, 0, 24); }
};
} // namespace

namespace {
// A class the records name, before the runs are formed: the kept chunk of its
// vtable, the address point's offset into it, and its primary base's name.
struct ClassNode {
  DefinedRegular *vtable;
  uint64_t apoff;
  std::string base;
};
} // namespace

// A tagged vtable, which a required pin places rather than a slot of its
// primary base's run.
struct TaggedVTable {
  SectionChunk *chunk;
  uint64_t apoff;
  StringRef cls;
  StringRef base;
};

// Gathers the classes the live records name, keeping one vtable per class. A
// chunk that holds more than one class's address point is a merged global,
// whose classes are left out, since its vtables do not each have a slot. A
// tagged vtable is gathered apart.
static void gatherClasses(COFFLinkerContext &ctx,
                          MapVector<StringRef, ClassNode> &classes,
                          std::vector<TaggedVTable> &tagged) {
  DenseMap<const Chunk *, unsigned> chunkUses;
  DenseSet<StringRef> merged;
  for (ObjFile *file : ctx.objFileInstances) {
    for (const ObjFile::LinkVTable &vt : file->getLinkVTables()) {
      if (!vt.vtable->getChunk() || !vt.vtable->getChunk()->live)
        continue;
      uint64_t apoff = vt.offset;
      if (!vt.onSection)
        apoff += vt.vtable->getValue();
      StringRef name = vt.cls;
      if (auto pin = ctx.chunkPins.find(vt.vtable->getChunk());
          pin != ctx.chunkPins.end() && pin->second.required) {
        tagged.push_back({vt.vtable->getChunk(), apoff, name, vt.base});
        continue;
      }
      auto [it, inserted] = classes.try_emplace(
          name, ClassNode{vt.vtable, apoff, vt.base});
      if (inserted)
        ++chunkUses[vt.vtable->getChunk()];
      else if (it->second.vtable->getChunk() != vt.vtable->getChunk())
        // Two kept chunks for one class: keep the first and do not count the
        // other against the chunk it names.
        merged.insert(name);
    }
  }
  // A chunk used by more than one class holds a merged global.
  for (auto &[name, node] : classes)
    if (chunkUses[node.vtable->getChunk()] > 1)
      merged.insert(name);
  for (StringRef name : merged)
    classes.erase(name);
}

// Visits name and its primary-base descendants in pre-order, appending a slot
// for each to run and recording, for each class, its slot and its subtree's
// size, which is contiguous in pre-order. A class with no vtable here, a base
// that only its derived classes name or one left out, has no slot of its own,
// and its run is its descendants'.
static void walk(StringRef name, MapVector<StringRef, ClassNode> &classes,
                 const DenseSet<StringRef> &excluded,
                 DenseMap<StringRef, std::vector<StringRef>> &children,
                 DenseSet<StringRef> &visited, VTableRun &run) {
  if (!visited.insert(name).second)
    return;
  unsigned first = run.slots.size();
  run.classes.push_back({name.str(), first, 0});
  unsigned classIndex = run.classes.size() - 1;
  if (auto it = classes.find(name);
      it != classes.end() && !excluded.contains(name))
    run.slots.push_back({it->second.vtable->getChunk(), it->second.apoff});
  std::vector<StringRef> &kids = children[name];
  llvm::sort(kids);
  for (StringRef kid : kids)
    walk(kid, classes, excluded, children, visited, run);
  run.classes[classIndex].count = run.slots.size() - first;
}

void lld::coff::registerVTableRunSymbols(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Register vtable run symbols");
  ctx.symtab.forEachSymbol([&](Symbol *s) {
    if (!isa<Undefined>(s))
      return;
    StringRef name = s->getName();
    if (name.starts_with(VTableRangeStartPrefix) ||
        name.starts_with(VTableRangeEndPrefix) ||
        name.starts_with(VTableRangeMaskPrefix) ||
        name.starts_with(VTableOwnerRunPrefix))
      replaceSymbol<DefinedSynthetic>(s, name, nullptr);
  });
}

// Two tagged vtables of one hierarchy whose address points share their page
// offset share a tag, so that a failing cast between them takes the type_info
// compare. Tags follow from the classes alone and are never moved, so a
// program can only rename or restructure; the pairs are listed under /verbose
// rather than warned about.
static void reportTagCollisions(COFFLinkerContext &ctx,
                                MapVector<StringRef, ClassNode> &classes,
                                std::vector<TaggedVTable> &tagged) {
  if (!ctx.config.verbose || tagged.size() < 2)
    return;
  // The root of a class's primary-base chain, as far as the records go.
  auto rootOf = [&](StringRef name) {
    for (unsigned depth = 0; depth != classes.size(); ++depth) {
      auto it = classes.find(name);
      if (it == classes.end() || it->second.base.empty())
        break;
      name = it->second.base;
    }
    return name;
  };
  std::map<std::pair<StringRef, uint64_t>, StringRef> seen;
  for (const TaggedVTable &t : tagged) {
    const ChunkPin &pin = ctx.chunkPins.find(t.chunk)->second;
    uint64_t tag = (pin.residue + t.apoff) & 4095;
    StringRef root = rootOf(t.base.empty() ? t.cls : t.base);
    auto [it, inserted] = seen.try_emplace({root, tag}, t.cls);
    if (!inserted && it->second != t.cls)
      Log(ctx) << "tagged vtables of " << it->second << " and " << t.cls
               << ", derived from " << root << ", share the tag " << tag;
  }
}

void lld::coff::resolveVTableRuns(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Resolve vtable runs");
  MapVector<StringRef, ClassNode> classes;
  std::vector<TaggedVTable> tagged;
  gatherClasses(ctx, classes, tagged);
  reportTagCollisions(ctx, classes, tagged);
  if (classes.empty())
    return;

  // A class whose vtable is too large for a slot is left out; its children
  // then head their own runs. The exclusion repeats until every run's slot
  // fits, which the first pass almost always reaches.
  DenseSet<StringRef> excluded;
  for (;;) {
    // Each class is a child of its primary base, and one with none heads a
    // run. A base with no vtable here heads its derived classes' run, so that
    // a check of it still finds them, as for a class the image derives from
    // another image's; it, and a class left out, has no slot of its own.
    DenseMap<StringRef, std::vector<StringRef>> children;
    std::vector<StringRef> roots;
    DenseSet<StringRef> absentRoots;
    for (auto &[name, node] : classes) {
      if (node.base.empty()) {
        roots.push_back(name);
        continue;
      }
      children[node.base].push_back(name);
      if (!classes.count(node.base) && absentRoots.insert(node.base).second)
        roots.push_back(node.base);
    }
    llvm::sort(roots);

    ctx.vtableRuns.clear();
    ctx.runChunkRun.clear();
    DenseSet<StringRef> visited;
    bool dropped = false;
    for (StringRef root : roots) {
      VTableRun run;
      walk(root, classes, excluded, children, visited, run);
      if (run.slots.empty())
        continue;
      // The slot covers the largest vtable, with its address point, so that no
      // slot boundary but an address point falls in the run.
      uint64_t widest = 0;
      StringRef widestClass;
      for (const VTableRun::Class &c : run.classes) {
        if (!classes.count(c.name) || excluded.contains(c.name))
          continue;
        const VTableRun::Slot &slot = run.slots[c.first];
        uint64_t span = slot.chunk->getSize() + slot.apoff;
        if (span > widest) {
          widest = span;
          widestClass = c.name;
        }
      }
      if (widest > maxSlot) {
        excluded.insert(widestClass);
        dropped = true;
        break;
      }
      run.stride = std::max<uint64_t>(64, llvm::bit_ceil(widest));
      run.maskChunk = make<MaskChunk>(uint32_t(run.stride - 1));
      unsigned runIndex = ctx.vtableRuns.size();
      for (unsigned i = 0; i != run.slots.size(); ++i)
        ctx.runChunkRun[run.slots[i].chunk] = {runIndex, i};
      ctx.vtableRuns.push_back(std::move(run));
    }
    if (!dropped)
      break;
  }

  // A tagged vtable is offered the gaps of its primary base's run first, so
  // that a hierarchy's vtables share their pages.
  StringMap<unsigned> runOf;
  for (auto [i, run] : llvm::enumerate(ctx.vtableRuns))
    for (const VTableRun::Class &c : run.classes)
      runOf[c.name] = i;
  for (const TaggedVTable &t : tagged)
    if (auto it = runOf.find(t.base); it != runOf.end())
      ctx.taggedRun[t.chunk] = it->second;

}

void lld::coff::placeVTableRuns(
    COFFLinkerContext &ctx, OutputSection *rdataSec,
    llvm::function_ref<bool(const SectionChunk *)> heldBack, bool packed) {
  llvm::TimeTraceScope timeScope("Place vtable runs");
  if (ctx.vtableOwnerEmpty)
    rdataSec->addChunk(ctx.vtableOwnerEmpty);
  if (ctx.vtableRuns.empty())
    return;
  for (const VTableRun &run : ctx.vtableRuns)
    rdataSec->addChunk(run.maskChunk);
  if (packed)
    return;
  std::vector<Chunk *> &chunks = rdataSec->chunks;
  DenseSet<const Chunk *> inRdata(chunks.begin(), chunks.end());
  // The run each vtable of a whole run belongs to, so one pass over the section
  // places every run, instead of a search and an erase over every chunk per
  // run.
  DenseMap<const Chunk *, VTableRun *> memberRun;
  for (VTableRun &run : ctx.vtableRuns) {
    // A run the linker places needs all its vtables in plain read-only data,
    // laid out as consecutive slots. A vtable held back into the import
    // address table span keeps its in-place import slots inside that table,
    // and one that a required pin places, as a tagged vtable is, sits at its
    // pin, so neither can move into a run. Such a vtable, or one otherwise not
    // here, is left in place, and the run becomes an empty range so that its
    // classes' calls take the slow path.
    bool whole = llvm::all_of(run.slots, [&](const VTableRun::Slot &s) {
      auto pin = ctx.chunkPins.find(s.chunk);
      return inRdata.contains(s.chunk) && !heldBack(s.chunk) &&
             (pin == ctx.chunkPins.end() || !pin->second.required);
    });
    if (!whole) {
      for (const VTableRun::Slot &slot : run.slots)
        ctx.runChunkRun.erase(slot.chunk);
      run.stride = 0;
      continue;
    }
    // The writer places these vtables itself, so their advisory line pins,
    // which would fix them one at a time, give way.
    for (const VTableRun::Slot &slot : run.slots) {
      ctx.chunkPins.erase(slot.chunk);
      memberRun[slot.chunk] = &run;
    }
  }
  if (memberRun.empty())
    return;
  // Lay each whole run's vtables out together where its first vtable sits in
  // the section, in slot order, leaving every other chunk in place.
  std::vector<Chunk *> placed;
  placed.reserve(chunks.size());
  DenseSet<VTableRun *> emitted;
  for (Chunk *c : chunks) {
    auto it = memberRun.find(c);
    if (it == memberRun.end()) {
      placed.push_back(c);
      continue;
    }
    if (!emitted.insert(it->second).second)
      continue;
    for (const VTableRun::Slot &slot : it->second->slots)
      placed.push_back(slot.chunk);
  }
  chunks = std::move(placed);
}

void lld::coff::defineVTableRuns(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Define vtable runs");

  // Where the end and the start plus the stride less one of each placed
  // class's run are, which a DLL exports.
  struct PlacedRun {
    Chunk *chunk;
    uint64_t end;
    uint64_t last;
  };
  StringMap<PlacedRun> placedRuns;
  for (const VTableRun &run : ctx.vtableRuns) {
    // The run holds if each slot's address point is the stride past the one
    // before, and the first is 16 bytes into a line; assignAddresses places it
    // so, but a packing it could not meet leaves it unplaced.
    bool placed = run.stride != 0;
    uint64_t prev = 0;
    for (unsigned i = 0; placed && i != run.slots.size(); ++i) {
      uint64_t point = run.slots[i].chunk->getRVA() + run.slots[i].apoff;
      if (i == 0)
        placed = point % 64 == 16;
      else if (point != prev + run.stride)
        placed = false;
      prev = point;
    }
    for (const VTableRun::Class &cls : run.classes) {
      // A class with no vtable here whose derived classes have none either
      // has an empty range.
      if (cls.count == 0)
        continue;
      const VTableRun::Slot &slot = run.slots[cls.first];
      uint64_t bytes = uint64_t(cls.count) * run.stride;
      auto define = [&](StringRef prefix, Chunk *c, uint64_t off) {
        if (Symbol *s = ctx.symtab.find((prefix + cls.name).str());
            s && isa<Defined>(s))
          replaceSymbol<DefinedSynthetic>(s, s->getName(), c, off);
      };
      // The guard thunk tests a vtable pointer through addresses: the start is
      // the class's own address point, the end one past its subtree's last
      // slot, and the mask the read-only word holding the stride less one. An
      // unplaced run gives an empty range at the image base, so a pointer
      // misses it.
      if (placed) {
        define(VTableRangeStartPrefix, slot.chunk, slot.apoff);
        define(VTableRangeEndPrefix, slot.chunk, slot.apoff + bytes);
        define(VTableRangeMaskPrefix, run.maskChunk, 0);
        if (!ctx.vtableRunExports.empty())
          placedRuns[cls.name] = {slot.chunk, slot.apoff + bytes,
                                  slot.apoff + run.stride - 1};
      } else {
        define(VTableRangeStartPrefix, nullptr, 0);
        define(VTableRangeEndPrefix, nullptr, 0);
        define(VTableRangeMaskPrefix, nullptr, 0);
      }
    }
  }

  // A range symbol whose class has no run keeps the empty range that
  // registerVTableRunSymbols gave it, so every vtable pointer misses it.

  // An exported run that the layout did not place has its end and its last
  // word at its start, an extent of zero.
  for (COFFLinkerContext::VTableRunExport &e : ctx.vtableRunExports) {
    auto it = placedRuns.find(e.cls);
    for (int w : {1, 2}) {
      if (it == placedRuns.end())
        replaceSymbol<DefinedSynthetic>(e.words[w], e.names[w], e.chunk,
                                        e.apoff);
      else
        replaceSymbol<DefinedSynthetic>(
            e.words[w], e.names[w], it->second.chunk,
            w == 1 ? it->second.end : it->second.last);
    }
  }

  // A class whose owner the image binds reads the three words from the
  // import address table.
  for (COFFLinkerContext::VTableOwnerRun &r : ctx.vtableOwnerRuns)
    replaceSymbol<DefinedSynthetic>(r.sym, r.sym->getName(),
                                    r.words[0]->getChunk(),
                                    r.words[0]->getRVA() -
                                        r.words[0]->getChunk()->getRVA());
}

// The name a DLL exports a word of a class's run under, hashed from the
// class's name, which may be long, as the contract witnesses' are: the end
// ('e') or the start plus the stride less one ('m').
static std::string runWordExportName(StringRef cls, char word) {
  return (VTableRunExportPrefix + getABIContractLocator(cls) + Twine(word))
      .str();
}

// The name an export goes by, before fixupExports sets it.
static StringRef exportedName(const Export &e) {
  return !e.exportAs.empty()  ? e.exportAs
         : !e.extName.empty() ? e.extName
                              : e.name;
}

void lld::coff::addVTableRunExports(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Add vtable run exports");
  if (!ctx.config.dll || !ctx.config.importSlots)
    return;
  std::vector<Export> &exports = ctx.symtab.exports;
  llvm::erase_if(exports, [&](const Export &e) {
    if (!exportedName(e).starts_with(VTableRunExportPrefix))
      return false;
    if (e.source != ExportSource::ExportAll)
      Err(ctx) << "cannot export " << exportedName(e)
               << ": the name is reserved for the vtable runs";
    return true;
  });
  StringSet<> names;
  for (const Export &e : exports)
    names.insert(exportedName(e));

  // Each class of a live record whose vtable or type_info the image exports,
  // with its own vtable's address point, the run's first slot whether or not
  // the layout places the run.
  struct Class {
    std::string cls;
    Chunk *chunk;
    uint64_t apoff;
    std::string start;
  };
  std::vector<Class> classes;
  StringSet<> seen;
  StringMap<unsigned> locators;
  DenseSet<unsigned> clashed;
  for (ObjFile *file : ctx.objFileInstances) {
    for (const ObjFile::LinkVTable &vt : file->getLinkVTables()) {
      SectionChunk *chunk = vt.vtable->getChunk();
      if (!chunk || !chunk->live || !seen.insert(vt.cls).second)
        continue;
      if (!names.contains("_ZTV" + vt.cls) && !names.contains("_ZTI" + vt.cls))
        continue;
      uint64_t apoff = vt.offset + (vt.onSection ? 0 : vt.vtable->getValue());
      uint64_t point = apoff - vt.vtable->getValue();
      // Two classes whose hashed names clash export neither's, so that an
      // importer never reads one class's run for another's.
      auto [it, inserted] = locators.try_emplace(
          getABIContractLocator(vt.cls), classes.size());
      if (!inserted) {
        clashed.insert(it->second);
        clashed.insert(classes.size());
      }
      classes.push_back({vt.cls, chunk, apoff,
                         ("_ZTV" + vt.cls + "$ap" + Twine(point)).str()});
    }
  }

  for (auto [i, c] : llvm::enumerate(classes)) {
    if (clashed.contains(i))
      continue;
    COFFLinkerContext::VTableRunExport e{c.cls, c.chunk, c.apoff, {}, {}};
    e.names[0] = saver().save(c.start);
    e.names[1] = saver().save(runWordExportName(c.cls, 'e'));
    e.names[2] = saver().save(runWordExportName(c.cls, 'm'));
    // The start is the class's address point, which the image exports by
    // that name already when another image may refer to it.
    for (int w : {0, 1, 2}) {
      if (w == 0 && names.contains(e.names[0]))
        continue;
      e.words[w] = make<DefinedSynthetic>(e.names[w], c.chunk, c.apoff);
      Export x;
      x.name = e.names[w];
      x.sym = e.words[w];
      x.data = true;
      x.isPrivate = w != 0;
      x.source = ExportSource::Export;
      exports.push_back(x);
    }
    ctx.vtableRunExports.push_back(std::move(e));
  }
}

void lld::coff::addVTableRunImports(
    COFFLinkerContext &ctx, StringRef dllName,
    std::vector<llvm::object::COFFShortExport> &exports) {
  if (ctx.vtableRunExports.empty())
    return;
  std::string dll = dllName.lower();
  StringMap<uint32_t> hints;
  for (const llvm::object::COFFShortExport &e : exports)
    hints.try_emplace(!e.ExportAs.empty() ? e.ExportAs
                      : !e.ExtName.empty() ? e.ExtName
                                           : e.Name,
                      e.Ordinal);
  static const StringRef prefixes[] = {VTableRangeStartPrefix,
                                       VTableRangeEndPrefix,
                                       VTableRangeMaskPrefix};
  for (const COFFLinkerContext::VTableRunExport &r : ctx.vtableRunExports) {
    for (int w : {0, 1, 2}) {
      auto it = hints.find(r.names[w]);
      if (it == hints.end())
        break;
      llvm::object::COFFShortExport e;
      e.Name = (prefixes[w] + r.cls + "$" + dll).str();
      e.ExportAs = r.names[w].str();
      e.Ordinal = it->second;
      e.Data = true;
      exports.push_back(std::move(e));
    }
  }
}

void lld::coff::bindVTableOwnerRuns(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Bind vtable owner runs");
  std::vector<Symbol *> owners;
  ctx.symtab.forEachSymbol([&](Symbol *s) {
    if (isa<DefinedSynthetic>(s) &&
        s->getName().starts_with(VTableOwnerRunPrefix))
      owners.push_back(s);
  });
  if (owners.empty())
    return;
  llvm::sort(owners, [](Symbol *a, Symbol *b) {
    return a->getName() < b->getName();
  });

  // The DLLs the image imports statically, in load order. A delay-loaded DLL
  // may be unloaded, and an image of mixed machines is not served.
  SmallVector<std::string, 0> dlls;
  if (ctx.config.importSlots) {
    StringSet<> seen;
    for (ImportFile *file : ctx.importFileInstances) {
      std::string dll = StringRef(file->dllName).lower();
      if (file->live && !file->isBoundAtRunTime() &&
          !ctx.config.delayLoads.contains(dll) && seen.insert(dll).second)
        dlls.push_back(dll);
    }
  }
  // Whether the image imports the class's vtable or type_info from dll, so
  // that the DLL's run of it adds no import that could fail to load.
  auto importsClassFrom = [&](StringRef cls, StringRef dll) {
    for (StringRef prefix : {"__imp__ZTV", "__imp__ZTI"}) {
      auto *imp =
          dyn_cast_or_null<DefinedImportData>(ctx.symtab.find((prefix + cls).str()));
      if (imp && imp->file->live && imp->getDLLName().equals_insensitive(dll))
        return true;
    }
    return false;
  };

  static const StringRef prefixes[] = {VTableRangeStartPrefix,
                                       VTableRangeEndPrefix,
                                       VTableRangeMaskPrefix};
  for (Symbol *sym : owners) {
    StringRef cls = sym->getName().drop_front(strlen(VTableOwnerRunPrefix));
    bool bound = false;
    for (StringRef dll : dlls) {
      LazyArchive *members[3];
      for (int w : {0, 1, 2})
        members[w] = dyn_cast_or_null<LazyArchive>(ctx.symtab.find(
            ("__imp_" + prefixes[w] + cls + "$" + dll).str()));
      if (!members[0] || !members[1] || !members[2] ||
          !importsClassFrom(cls, dll))
        continue;
      COFFLinkerContext::VTableOwnerRun r{sym, {}};
      for (int w : {0, 1, 2}) {
        auto *file = make<ImportFile>(ctx, members[w]->getMemberBuffer());
        ctx.driver.addFile(file);
        file->live = true;
        r.words[w] = file->impSym;
      }
      ctx.vtableOwnerRuns.push_back(r);
      bound = true;
      break;
    }
    if (!bound) {
      if (!ctx.vtableOwnerEmpty)
        ctx.vtableOwnerEmpty = make<EmptyOwnerRunChunk>();
      replaceSymbol<DefinedSynthetic>(sym, sym->getName(),
                                      ctx.vtableOwnerEmpty);
    }
  }
}
