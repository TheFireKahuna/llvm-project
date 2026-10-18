//===- VagueEntities.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Under -import-slots, a DLL's import library records the observable vague
// entities with default visibility that the DLL keeps, the DLLs it imports
// from, and their records in turn, so that a link sees the whole static load
// closure of what it imports. A link reports two images of that closure that
// both keep an entity: ELF, which gives a program one instance of each such
// entity among the shared objects it needs, would have unified them.
//
//===----------------------------------------------------------------------===//

#include "VagueEntities.h"
#include "COFFLinkerContext.h"
#include "Driver.h"
#include "ImportSlots.h"
#include "InputFiles.h"
#include "LocalImports.h"
#include "OverrideTable.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;

namespace lld::coff {
// The name by which an import library's record of the DLL dll is found.
static std::string getRecordSymbol(StringRef dll) {
  return (vagueRecordPrefix + dll.lower()).str();
}

// One image of a load closure: its name in lower case, the entities it keeps
// and the name of the epoch it exports, if any, the entities it keeps
// preemptible, with the other external symbols of their groups and their
// guards, the overridable defaults it offers, and the images it imports from
// and delay-loads, by index. The names point into the import libraries and
// the link's strings, which outlive every closure.
struct Image {
  StringRef name, epoch;
  std::vector<StringRef> entities, preemptible, offers;
  SmallVector<uint32_t, 0> imports, delayLoads;
};

// The images of a load closure, by name.
struct LoadClosure {
  std::vector<Image> images;
  StringMap<uint32_t> byName;

  uint32_t add(StringRef name) {
    auto [it, inserted] = byName.try_emplace(name.lower(), images.size());
    if (inserted)
      images.push_back({saver().save(it->first())});
    return it->second;
  }

  // Adds what src, a description of the image i whose image indices idx maps
  // to these, says of it. An image that several records describe keeps the
  // first description of its entities and epoch, of its offers and of its
  // preemptible entities, each independently, and the union of every
  // description's imports and delay-loads. src describes the entities only
  // where it names the image.
  void merge(uint32_t i, const Image &src, ArrayRef<uint32_t> idx) {
    Image &image = images[i];
    if (!src.name.empty() && image.entities.empty()) {
      image.entities = src.entities;
      image.epoch = src.epoch;
    }
    if (image.offers.empty())
      image.offers = src.offers;
    if (image.preemptible.empty())
      image.preemptible = src.preemptible;
    for (auto [to, from] : {std::pair(&image.imports, &src.imports),
                            std::pair(&image.delayLoads, &src.delayLoads)})
      for (uint32_t j : *from)
        if (!llvm::is_contained(*to, idx[j]))
          to->push_back(idx[j]);
  }

  // The images that roots reach through their imports, roots first, in
  // breadth-first order.
  std::vector<uint32_t> walk(ArrayRef<uint32_t> roots) const {
    std::vector<uint32_t> order;
    BitVector seen(images.size());
    auto visit = [&](uint32_t i) {
      if (!seen.test(i)) {
        seen.set(i);
        order.push_back(i);
      }
    };
    for (uint32_t i : roots)
      visit(i);
    for (size_t next = 0; next != order.size(); ++next)
      for (uint32_t j : images[order[next]].imports)
        visit(j);
    return order;
  }
};

// Reads a string, its length and then its bytes.
static StringRef getString(DataExtractor &data, DataExtractor::Cursor &cur) {
  return data.getBytes(cur, data.getULEB128(cur));
}

// Writes s as getString reads it.
static void putString(raw_ostream &os, StringRef s) {
  encodeULEB128(s.size(), os);
  os << s;
}

// Parses the record that mb, a member of an import library, holds into a
// closure of its own, whose first image is the DLL, or returns null if it
// holds none or is malformed.
static LoadClosure *parseRecord(COFFLinkerContext &ctx, MemoryBufferRef mb) {
  auto *member = make<LoadClosure>();
  // The member's image of each image the record lists.
  SmallVector<uint32_t, 0> index;
  bool malformed = false;
  forEachLinkRecordGroup(
      mb, IMAGE_FILE_MACHINE_UNKNOWN,
      [&](uint64_t kind, ArrayRef<uint8_t> payload) {
        if (malformed || (kind != LinkRecordVagueEntities &&
                          kind != LinkRecordOverrideOffers &&
                          kind != LinkRecordPreemptibleEntities &&
                          kind != LinkRecordDelayLoads))
          return;
        DataExtractor group(payload, /*IsLittleEndian=*/true);
        DataExtractor::Cursor c(0);
        // The offers, preemptible entities or delay-loads of the images that
        // the closure group listed, by their index.
        if (kind != LinkRecordVagueEntities) {
          while (c && !group.eof(c) && !index.empty()) {
            uint64_t i = group.getULEB128(c);
            Image src;
            for (uint64_t j = 0, n = group.getULEB128(c); c && j != n; ++j) {
              if (kind == LinkRecordDelayLoads)
                src.delayLoads.push_back(group.getULEB128(c));
              else
                (kind == LinkRecordOverrideOffers ? src.offers
                                                  : src.preemptible)
                    .push_back(getString(group, c));
            }
            if (c && i < index.size() &&
                llvm::all_of(src.delayLoads,
                             [&](uint32_t j) { return j < index.size(); }))
              member->merge(index[i], src, index);
          }
          consumeError(c.takeError());
          return;
        }
        std::vector<Image> listed;
        for (uint64_t n = group.getULEB128(c); c && listed.size() != n;) {
          Image &image = listed.emplace_back();
          image.name = getString(group, c);
          for (uint64_t j = 0, m = group.getULEB128(c); c && j != m; ++j)
            image.imports.push_back(group.getULEB128(c));
          for (uint64_t j = 0, m = group.getULEB128(c); c && j != m; ++j)
            image.entities.push_back(getString(group, c));
          image.epoch = getString(group, c);
        }
        bool ok = c && group.eof(c) && !listed.empty() && index.empty();
        consumeError(c.takeError());
        for (const Image &image : listed)
          index.push_back(member->add(image.name));
        for (const Image &image : listed)
          ok &= llvm::all_of(image.imports,
                             [&](uint32_t j) { return j < index.size(); });
        if (!ok) {
          Warn(ctx) << mb.getBufferIdentifier()
                    << ": the record of vague entities is malformed";
          malformed = true;
          return;
        }
        for (auto [i, image] : llvm::enumerate(listed))
          member->merge(index[i], image, index);
      });
  return malformed || index.empty() ? nullptr : member;
}

// Adds the images of mb's record, which is parsed on the first request and
// kept on the context, to closure, and returns the index of the DLL itself.
static std::optional<uint32_t>
readRecord(COFFLinkerContext &ctx, MemoryBufferRef mb, LoadClosure &closure) {
  auto [it, inserted] = ctx.vagueRecordCache.try_emplace(mb.getBufferStart());
  if (inserted)
    it->second = parseRecord(ctx, mb);
  LoadClosure *member = it->second;
  if (!member)
    return std::nullopt;
  SmallVector<uint32_t, 0> idx;
  for (const Image &image : member->images)
    idx.push_back(closure.add(image.name));
  for (auto [i, image] : llvm::enumerate(member->images))
    closure.merge(idx[i], image, idx);
  return idx[0];
}

// The entities that the image keeps and may share: data with default
// visibility in a COMDAT that it keeps and no DLL's instance preempted, under
// its record's name for a thread-local variable, and functions whose
// addresses are significant and not a DLL's, from objects that list what has
// default visibility. Names that MSVC mangles keep the per-image copies their
// code expects. A type_info object is left out: two images that keep one
// compare it by name, which is correct.
static std::vector<StringRef> getKeptEntities(SymbolTable &symtab) {
  std::vector<StringRef> kept;
  auto add = [&](Symbol *s) {
    auto *d = dyn_cast<DefinedRegular>(s);
    auto *file = d ? dyn_cast<ObjFile>(d->getFile()) : nullptr;
    if (file && d->getChunk()->live && file->listsBindings() &&
        file->hasDefaultVisibility(d) && isShareableVagueName(d->getName()) &&
        !isTypeInfoName(d->getName()))
      kept.push_back(symtab.getSharedName(d));
  };
  for (Symbol *s : symtab.weakData)
    add(s);
  for (Symbol *s : symtab.weakFunctions)
    if (!symtab.offeredFunctions.contains(s))
      add(s);
  llvm::sort(kept);
  kept.erase(llvm::unique(kept), kept.end());
  return kept;
}

// The DLLs the image imports statically, which its load closure holds; before
// liveness is decided, every DLL an import was read from.
static std::vector<std::string> getImportedDLLs(COFFLinkerContext &ctx,
                                                bool liveOnly = true) {
  std::vector<std::string> dlls;
  StringSet<> seen;
  for (ImportFile *file : ctx.importFileInstances) {
    std::string dll = StringRef(file->dllName).lower();
    if ((file->live || !liveOnly) && seen.insert(dll).second)
      dlls.push_back(dll);
  }
  return dlls;
}

// Reads the records of the DLLs the image imports, which their import
// libraries hold, into closure, and returns the indices of those it read.
static std::vector<uint32_t> readImportedRecords(COFFLinkerContext &ctx,
                                                 LoadClosure &closure,
                                                 bool liveOnly = true) {
  std::vector<uint32_t> direct;
  for (const std::string &dll : getImportedDLLs(ctx, liveOnly))
    if (auto *lazy = dyn_cast_or_null<LazyArchive>(
            ctx.symtab.find(getRecordSymbol(dll))))
      if (std::optional<uint32_t> i =
              readRecord(ctx, lazy->getMemberBuffer(), closure))
        direct.push_back(*i);
  return direct;
}

StringMap<StringSet<>> getPreemptibleOffers(COFFLinkerContext &ctx,
                                            bool liveOnly) {
  StringMap<StringSet<>> offers;
  LoadClosure closure;
  for (uint32_t i : readImportedRecords(ctx, closure, liveOnly)) {
    const Image &image = closure.images[i];
    if (!image.preemptible.empty())
      offers[image.name].insert_range(image.preemptible);
  }
  return offers;
}

void SymbolTable::readOverrideRecords() {
  // The records come only with the import libraries, so they are read again
  // only once more libraries have been read.
  if (!ctx.config.importSlots ||
      indexedArchives.size() == overrideRecordsArchives)
    return;
  overrideRecordsArchives = indexedArchives.size();
  for (MemoryBufferRef mb : getImportLibraryRecords())
    forEachLinkRecordGroup(
        mb, IMAGE_FILE_MACHINE_UNKNOWN,
        [&](uint64_t kind, ArrayRef<uint8_t> payload) {
          if (kind != LinkRecordOverridableByName &&
              kind != LinkRecordOverrideSlots)
            return;
          DataExtractor group(payload, /*IsLittleEndian=*/true);
          DataExtractor::Cursor c(0);
          if (kind == LinkRecordOverrideSlots) {
            uint64_t format = group.getULEB128(c);
            if (c && format != overrideTableVersion) {
              Err(ctx) << mb.getBufferIdentifier()
                       << ": the C++ runtime's override table format (version "
                       << format << ") is not the one this linker writes "
                       << "(version " << overrideTableVersion << ")";
              return;
            }
          }
          while (c && !group.eof(c)) {
            StringRef name = getString(group, c);
            if (kind == LinkRecordOverridableByName) {
              if (c)
                defaultsByName.insert(name);
              continue;
            }
            uint64_t slot = group.getULEB128(c);
            uint64_t forward = group.getULEB128(c);
            uint64_t flags = group.getULEB128(c);
            if (c)
              addOverrideSlotFacts(
                  name,
                  {uint32_t(slot),
                   forward ? std::optional<uint32_t>(forward - 1)
                           : std::nullopt,
                   (flags & 1) != 0, /*own=*/false},
                  "an import library");
          }
          consumeError(c.takeError());
        });
}

std::vector<std::pair<StringRef, StringRef>>
getDependentOverrideOffers(COFFLinkerContext &ctx,
                           ArrayRef<MemoryBufferRef> records) {
  LoadClosure closure;
  std::vector<uint32_t> roots;
  for (MemoryBufferRef mb : records)
    if (std::optional<uint32_t> i = readRecord(ctx, mb, closure))
      roots.push_back(*i);
  // Breadth first from the DLLs whose records the import libraries hold,
  // which make their own offers through their offer symbols.
  std::vector<std::pair<StringRef, StringRef>> offers;
  for (uint32_t i : closure.walk(roots))
    if (!llvm::is_contained(roots, i))
      for (StringRef offer : closure.images[i].offers)
        offers.emplace_back(closure.images[i].name, offer);
  return offers;
}

std::vector<std::pair<StringRef, StringRef>>
chooseVagueOwners(COFFLinkerContext &ctx, const DenseSet<StringRef> &kept) {
  LoadClosure closure;
  std::vector<uint32_t> direct =
      readImportedRecords(ctx, closure, /*liveOnly=*/false);
  // The images in ELF's breadth-first load order from the executable: those
  // loaded eagerly, then those delay-loaded, which join their importer's scope
  // when they load, so that lazy loading never moves an instance.
  std::vector<uint32_t> order, delayed;
  for (uint32_t i : direct)
    (ctx.config.delayLoads.contains(closure.images[i].name) ? delayed : order)
        .push_back(i);
  order = closure.walk(order);
  DenseSet<uint32_t> lazy;
  for (size_t done = 0;;) {
    for (; done != order.size(); ++done)
      llvm::append_range(delayed, closure.images[order[done]].delayLoads);
    if (delayed.empty())
      break;
    lazy.insert_range(delayed);
    llvm::append_range(order, delayed);
    order = closure.walk(order);
    delayed.clear();
  }
  // The first image to keep each entity preemptible, and how many do.
  DenseMap<StringRef, std::pair<uint32_t, uint32_t>> keepers;
  for (uint32_t i : order)
    for (StringRef name : closure.images[i].preemptible)
      ++keepers.try_emplace(name, i, 0).first->second.second;
  std::vector<std::pair<StringRef, StringRef>> owners;
  for (auto &[name, keeper] : keepers) {
    bool own = kept.contains(name);
    // An entity that only delay-loaded DLLs keep has no instance at start-up
    // for the executable to import; the first of them to load keeps its own,
    // and the others' searches find it.
    if (keeper.second + own < 2 || (!own && lazy.contains(keeper.first)))
      continue;
    owners.emplace_back(name, own ? StringRef()
                                  : closure.images[keeper.first].name);
  }
  llvm::sort(owners);
  return owners;
}

void checkVagueEntities(COFFLinkerContext &ctx, StringRef imageName) {
  llvm::TimeTraceScope timeScope("Check vague entities");
  Configuration &config = ctx.config;
  if (!config.importSlots)
    return;
  LoadClosure closure;
  std::vector<uint32_t> direct = readImportedRecords(ctx, closure);
  if (direct.empty())
    return;
  uint32_t self = closure.add(imageName);
  Image &me = closure.images[self];
  me.entities = getKeptEntities(ctx.symtab);
  me.imports.assign(direct.begin(), direct.end());
  // An executable owns what it keeps, through the table it writes for the
  // entities two images keep.
  for (StringRef entity : me.entities)
    if (!config.dll ||
        ctx.symtab.isPreemptibleVagueEntity(ctx.symtab.findVagueEntity(entity)))
      me.preemptible.push_back(entity);

  StringMap<SmallVector<uint32_t, 2>> keepers;
  for (auto [i, image] : llvm::enumerate(closure.images))
    for (StringRef entity : image.entities)
      keepers[entity].push_back(i);
  auto reaches = [&](uint32_t from, uint32_t to) {
    return llvm::is_contained(llvm::drop_begin(closure.walk({from})), to);
  };
  for (auto &[entity, images] : keepers) {
    if (images.size() < 2 || config.allowedVagueDuplicates.contains(entity))
      continue;
    for (size_t a = 0; a != images.size(); ++a)
      for (size_t b = a + 1; b != images.size(); ++b) {
        const Image &x = closure.images[images[a]];
        const Image &y = closure.images[images[b]];
        if (config.allowedVagueDuplicateDLLs.contains(x.name) ||
            config.allowedVagueDuplicateDLLs.contains(y.name))
          continue;
        // Two images that keep the entity preemptible share one instance at
        // run time, the one the executable's table names. Both lists are
        // sorted, so a binary search answers each membership test.
        if (llvm::binary_search(x.preemptible, entity) &&
            llvm::binary_search(y.preemptible, entity))
          continue;
        // An image that keeps its copy while importing from one that keeps
        // the entity too predates the other's definition, or does not take
        // its offer.
        const Image *importer = reaches(images[a], images[b])   ? &x
                                : reaches(images[b], images[a]) ? &y
                                                                : nullptr;
        if (!importer) {
          Err(ctx) << entity << " is defined by both " << x.name << " and "
                   << y.name
                   << ", neither of which imports from the other; link one "
                      "against the other's import library, give it hidden "
                      "visibility, or link both jointly";
          continue;
        }
        const Image &owner = importer == &x ? y : x;
        Err(ctx) << importer->name << " keeps its own copy of " << entity
                 << ", which " << owner.name << ", which it imports from, "
                 << "defines; "
                 << (importer == &me
                         ? "take " + owner.name + "'s offer"
                         : "relink " + importer->name);
      }
  }
}

void checkOfferAdditions(COFFLinkerContext &ctx, StringRef dllName) {
  llvm::TimeTraceScope timeScope("Check offer additions");
  Configuration &config = ctx.config;
  if (!config.importSlots || config.offersBaseline.empty())
    return;
  ErrorOr<std::unique_ptr<MemoryBuffer>> mb =
      MemoryBuffer::getFile(config.offersBaseline, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (!mb) {
    Err(ctx) << "cannot open " << config.offersBaseline << ": "
             << mb.getError().message();
    return;
  }
  // The record's names point into the library, which the link keeps.
  Expected<std::unique_ptr<Archive>> archive =
      Archive::create(ctx.driver.takeBuffer(std::move(*mb)));
  if (!archive) {
    Err(ctx) << config.offersBaseline << ": " << archive.takeError();
    return;
  }
  // The previous release's record, which its import library holds whatever
  // the DLL was named, and the names it exported.
  StringSet<> exported;
  std::optional<MemoryBufferRef> record;
  for (const Archive::Symbol &sym : (*archive)->symbols()) {
    StringRef name = sym.getName();
    if (name.starts_with(vagueRecordPrefix)) {
      Expected<Archive::Child> child = sym.getMember();
      Expected<MemoryBufferRef> member =
          child ? child->getMemoryBufferRef()
                : Expected<MemoryBufferRef>(child.takeError());
      if (member)
        record = *member;
      else
        consumeError(member.takeError());
    } else if (name.consume_front("__imp_")) {
      exported.insert(name);
    }
  }
  LoadClosure closure;
  std::optional<uint32_t> previous;
  if (record)
    previous = readRecord(ctx, *record, closure);
  if (!previous) {
    Err(ctx) << config.offersBaseline << " holds no record of the vague "
             << "entities " << dllName << " keeps";
    return;
  }
  // A DLL linked against the previous release keeps its own copy of an entity
  // that this release starts to define until it is relinked.
  const std::vector<StringRef> &entities = closure.images[*previous].entities;
  DenseSet<StringRef> before(entities.begin(), entities.end());
  for (StringRef entity : getKeptEntities(ctx.symtab))
    if (!before.contains(entity))
      Warn(ctx) << dllName << " newly defines " << entity
                << (exported.contains(entity)
                        ? ", which its previous release exported as other "
                          "than a shared entity"
                        : "")
                << "; a DLL linked against its previous import library keeps "
                   "its own copy until relinked";
}

StringRef getOffersEpoch(COFFLinkerContext &ctx) {
  SHA256 hash;
  for (StringRef entity : getKeptEntities(ctx.symtab)) {
    hash.update(entity);
    hash.update(ArrayRef<uint8_t>{0});
  }
  std::array<uint8_t, 32> digest = hash.final();
  return saver().save("__llvm_offers$" + toHex(ArrayRef(digest).take_front(8),
                                               /*LowerCase=*/true));
}

void addOffersEpoch(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Add offers epoch");
  Configuration &config = ctx.config;
  SymbolTable &symtab = ctx.symtab;
  if (!config.importSlots || !config.offersEpoch || !config.dll)
    return;
  // The export names data the DLL offers; its address is not read.
  auto it = llvm::find_if(symtab.exports, [](const Export &e) {
    return e.data && isa_and_nonnull<DefinedRegular>(e.sym);
  });
  if (it == symtab.exports.end())
    return;
  Export e;
  e.name = getOffersEpoch(ctx);
  e.symbolName = e.name;
  e.sym = it->sym;
  e.data = true;
  e.source = ExportSource::ExportAll;
  symtab.offersEpoch = e.name;
  symtab.exports.push_back(e);
}

void importOffersEpochs(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Import offers epochs");
  Configuration &config = ctx.config;
  // A DLL that offers nothing, of its own or forwarded, needs no epoch.
  if (!config.importSlots || !config.dll ||
      (ctx.symtab.forwardedImports.empty() &&
       getKeptEntities(ctx.symtab).empty()))
    return;
  LoadClosure closure;
  for (uint32_t i : readImportedRecords(ctx, closure)) {
    // The import refuses this DLL's load beside a release of the owner whose
    // offers have grown.
    StringRef epoch = closure.images[i].epoch;
    auto *lazy = epoch.empty() ? nullptr
                               : dyn_cast_or_null<LazyArchive>(ctx.symtab.find(
                                     ("__imp_" + epoch).str()));
    if (!lazy)
      continue;
    auto *file = make<ImportFile>(ctx, lazy->getMemberBuffer());
    ctx.driver.addFile(file);
    file->live = true;
  }
}

void addVagueEntityRecord(COFFLinkerContext &ctx, StringRef dllName,
                          std::vector<NewArchiveMember> &members,
                          std::vector<uint8_t> &buffer) {
  Configuration &config = ctx.config;
  SymbolTable &symtab = ctx.symtab;
  if (!config.importSlots)
    return;
  LoadClosure closure;
  uint32_t self = closure.add(dllName);
  std::vector<uint32_t> direct = readImportedRecords(ctx, closure);
  // The DLLs the image imports without a record are listed with no entities,
  // so that the closure keeps the path through them.
  for (const std::string &dll : getImportedDLLs(ctx))
    if (uint32_t i = closure.add(dll); !llvm::is_contained(direct, i))
      direct.push_back(i);
  // A DLL the image delay-loads is listed apart: it is no binding path.
  Image &image = closure.images[self];
  image.imports.clear();
  for (uint32_t i : direct)
    (config.delayLoads.contains(closure.images[i].name) ? image.delayLoads
                                                        : image.imports)
        .push_back(i);
  image.entities = getKeptEntities(symtab);
  image.epoch = symtab.offersEpoch;
  for (const Export &e : symtab.exports) {
    if (StringRef name = e.extName; name.consume_front(overrideOfferPrefix))
      image.offers.push_back(name);
    if (e.sym && e.forwardTo.empty() && symtab.isPreemptibleVagueEntity(e.sym))
      image.preemptible.push_back(e.sym->getName());
  }
  llvm::sort(image.preemptible);

  std::vector<std::pair<uint64_t, std::string>> groups;
  // A group of kind holding what put writes, unless that is nothing.
  auto addGroup = [&](uint64_t kind, function_ref<void(raw_ostream &)> put) {
    std::string payload;
    raw_string_ostream os(payload);
    put(os);
    if (!payload.empty())
      groups.emplace_back(kind, std::move(payload));
  };
  addGroup(LinkRecordVagueEntities, [&](raw_ostream &os) {
    encodeULEB128(closure.images.size(), os);
    for (const Image &image : closure.images) {
      putString(os, image.name);
      encodeULEB128(image.imports.size(), os);
      for (uint32_t i : image.imports)
        encodeULEB128(i, os);
      encodeULEB128(image.entities.size(), os);
      for (StringRef entity : image.entities)
        putString(os, entity);
      putString(os, image.epoch);
    }
  });
  // A group listing field of each image of the closure that has one, by the
  // image's index: the overridable defaults it offers, so that an executable
  // that does not link the import library of a DLL its imports depend on
  // still selects that DLL's offer in load order; the entities it keeps
  // preemptible, so that an executable that sees two images keeping one picks
  // its owner; and the DLLs it delay-loads.
  auto addPerImage = [&](uint64_t kind, auto field, auto put) {
    addGroup(kind, [&](raw_ostream &os) {
      for (auto [i, image] : llvm::enumerate(closure.images)) {
        if ((image.*field).empty())
          continue;
        encodeULEB128(i, os);
        encodeULEB128((image.*field).size(), os);
        for (const auto &x : image.*field)
          put(os, x);
      }
    });
  };
  auto putIndex = [](raw_ostream &os, uint32_t i) { encodeULEB128(i, os); };
  addPerImage(LinkRecordOverrideOffers, &Image::offers, putString);
  addPerImage(LinkRecordPreemptibleEntities, &Image::preemptible, putString);
  addPerImage(LinkRecordDelayLoads, &Image::delayLoads, putIndex);
  // The defaults the DLL defines and exports with no fixed slot, which an
  // image that links its import library replaces, offers or reaches by name.
  std::vector<StringRef> byName;
  for (const Export &e : symtab.exports)
    if (e.sym && e.extName.empty() &&
        symtab.overridablesByName.contains(e.sym->getName()))
      byName.push_back(e.sym->getName());
  llvm::sort(byName);
  byName.erase(llvm::unique(byName), byName.end());
  addGroup(LinkRecordOverridableByName, [&](raw_ostream &os) {
    for (StringRef name : byName)
      putString(os, name);
  });
  // The defaults the DLL defines and exports with a fixed slot, as their
  // definitions record it, which an image that links its import library
  // replaces or reaches through that slot.
  std::vector<std::pair<StringRef, SymbolTable::OverrideSlotFacts>> bySlot;
  for (const Export &e : symtab.exports)
    if (e.sym && isa<Defined>(e.sym) && !isa<DefinedImportData>(e.sym))
      if (auto it = symtab.overrideSlotFacts.find(e.sym->getName());
          it != symtab.overrideSlotFacts.end() && it->second.own)
        bySlot.emplace_back(it->first, it->second);
  llvm::sort(bySlot, llvm::less_first());
  bySlot.erase(
      llvm::unique(bySlot, [](const auto &a,
                              const auto &b) { return a.first == b.first; }),
      bySlot.end());
  if (!bySlot.empty())
    addGroup(LinkRecordOverrideSlots, [&](raw_ostream &os) {
      encodeULEB128(overrideTableVersion, os);
      for (auto &[name, facts] : bySlot) {
        putString(os, name);
        encodeULEB128(facts.slot, os);
        encodeULEB128(facts.forward ? *facts.forward + 1 : 0, os);
        encodeULEB128(facts.dropsSecond, os);
      }
    });
  members.push_back(createLinkRecordMember(
      ctx, dllName, getRecordSymbol(dllName), groups, buffer));
}

// Whether a data definition in a COMDAT has default visibility, so that a
// DLL's instance may preempt it, as on ELF.
bool isDefaultVisibility(DefinedRegular *d) {
  auto *file = dyn_cast<ObjFile>(d->getFile());
  return !file || file->hasDefaultVisibility(d);
}

DefinedRegular *SymbolTable::makeCopy(DefinedRegular *d) {
  auto *copy = make<DefinedRegular>(*d);
  copy->isExternal = false;
  copy->isCOMDAT = false;
  return copy;
}

void SymbolTable::preemptWeakData() {
  if (!ctx.config.importSlots || weakData.empty())
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
  if (!ctx.config.importSlots)
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
  llvm::TimeTraceScope timeScope("Add offers");
  if (!ctx.config.importSlots || (!ctx.config.dll && ctx.config.implib.empty()))
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
} // namespace lld::coff
