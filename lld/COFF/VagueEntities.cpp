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
#include "InputFiles.h"
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

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;

namespace lld::coff {
// The name by which an import library's record of the DLL dll is found.
static std::string getRecordSymbol(StringRef dll) {
  return (vagueRecordPrefix + dll.lower()).str();
}

namespace {
// One image of a load closure: the entities it keeps, the images it imports
// from, by index, the name of the epoch it exports, if any, and the
// overridable defaults it offers.
struct Image {
  std::string name;
  std::vector<std::string> entities;
  std::vector<uint32_t> imports;
  std::string epoch;
  std::vector<std::string> offers;
  // The entities it keeps preemptible, with the other external symbols of
  // their groups and their guards.
  std::vector<std::string> preemptible;
  // The images it delay-loads, by index.
  std::vector<uint32_t> delayLoads;
};

// The images of a load closure, by name.
struct Closure {
  std::vector<Image> images;
  StringMap<uint32_t> byName;

  uint32_t add(StringRef name) {
    auto [it, inserted] = byName.try_emplace(name.lower(), images.size());
    if (inserted) {
      images.emplace_back();
      images.back().name = name.lower();
    }
    return it->second;
  }
};
} // namespace

// Reads the record that mb, a member of an import library, holds into
// closure, and returns the index of its first image, the DLL itself.
static std::optional<uint32_t>
readRecord(COFFLinkerContext &ctx, MemoryBufferRef mb, Closure &closure) {
  Expected<std::unique_ptr<COFFObjectFile>> obj = COFFObjectFile::create(mb);
  if (!obj) {
    consumeError(obj.takeError());
    return std::nullopt;
  }
  for (const SectionRef &sec : (*obj)->sections()) {
    Expected<StringRef> name = sec.getName();
    Expected<StringRef> contents = sec.getContents();
    if (!name || !contents || *name != ".llvm_link_records") {
      consumeError(name.takeError());
      consumeError(contents.takeError());
      continue;
    }
    DataExtractor data(*contents, /*IsLittleEndian=*/true);
    DataExtractor::Cursor cur(0);
    if (data.getBytes(cur, sizeof(LinkRecordsMagic)) !=
            StringRef(LinkRecordsMagic, sizeof(LinkRecordsMagic)) ||
        data.getULEB128(cur) != LinkRecordsVersion) {
      consumeError(cur.takeError());
      return std::nullopt;
    }
    data.getULEB128(cur);
    SmallVector<uint32_t, 0> index;
    while (cur && !data.eof(cur)) {
      uint64_t kind = data.getULEB128(cur);
      uint64_t size = data.getULEB128(cur);
      uint64_t start = cur.tell();
      data.skip(cur, size);
      if (!cur || (kind != LinkRecordVagueEntities &&
                   kind != LinkRecordOverrideOffers &&
                   kind != LinkRecordPreemptibleEntities &&
                   kind != LinkRecordDelayLoads))
        continue;
      DataExtractor group(contents->substr(start, size), true);
      DataExtractor::Cursor c(0);
      auto getString = [&] {
        uint64_t n = group.getULEB128(c);
        return group.getBytes(c, n);
      };
      // The DLLs the images the closure group listed delay-load, by index.
      if (kind == LinkRecordDelayLoads) {
        while (c && !group.eof(c) && !index.empty()) {
          uint64_t image = group.getULEB128(c);
          uint64_t count = group.getULEB128(c);
          for (uint64_t j = 0; c && j != count; ++j) {
            uint64_t to = group.getULEB128(c);
            if (image < index.size() && to < index.size() &&
                !llvm::is_contained(closure.images[index[image]].delayLoads,
                                    index[to]))
              closure.images[index[image]].delayLoads.push_back(index[to]);
          }
        }
        consumeError(c.takeError());
        continue;
      }
      // The offers, or the preemptible entities, of the images the closure
      // group listed, by their index.
      if (kind == LinkRecordOverrideOffers ||
          kind == LinkRecordPreemptibleEntities) {
        auto field = kind == LinkRecordOverrideOffers ? &Image::offers
                                                      : &Image::preemptible;
        while (c && !group.eof(c) && !index.empty()) {
          uint64_t image = group.getULEB128(c);
          uint64_t count = group.getULEB128(c);
          std::vector<std::string> *offers =
              image < index.size() ? &(closure.images[index[image]].*field)
                                   : nullptr;
          bool known = offers && !offers->empty();
          for (uint64_t j = 0; c && j != count; ++j) {
            StringRef offer = getString();
            if (offers && !known)
              offers->push_back(offer.str());
          }
        }
        consumeError(c.takeError());
        continue;
      }
      uint64_t count = group.getULEB128(c);
      SmallVector<SmallVector<uint64_t, 0>, 0> imports;
      for (uint64_t i = 0; c && i != count; ++i) {
        uint32_t image = closure.add(getString());
        index.push_back(image);
        imports.emplace_back();
        uint64_t numImports = group.getULEB128(c);
        for (uint64_t j = 0; c && j != numImports; ++j)
          imports.back().push_back(group.getULEB128(c));
        uint64_t numEntities = group.getULEB128(c);
        // An image that several records hold is described alike by each.
        bool known = !closure.images[image].entities.empty();
        for (uint64_t j = 0; c && j != numEntities; ++j) {
          StringRef entity = getString();
          if (!known)
            closure.images[image].entities.push_back(entity.str());
        }
        StringRef epoch = getString();
        if (!known)
          closure.images[image].epoch = epoch.str();
      }
      bool ok = c && group.eof(c) && !index.empty();
      for (size_t i = 0; ok && i != index.size(); ++i)
        for (uint64_t j : imports[i]) {
          if (j >= index.size()) {
            ok = false;
            break;
          }
          std::vector<uint32_t> &to = closure.images[index[i]].imports;
          if (!llvm::is_contained(to, index[j]))
            to.push_back(index[j]);
        }
      consumeError(c.takeError());
      consumeError(cur.takeError());
      if (!ok) {
        Warn(ctx) << mb.getBufferIdentifier()
                  << ": the record of vague entities is malformed";
        return std::nullopt;
      }
    }
    consumeError(cur.takeError());
    if (!index.empty())
      return index[0];
  }
  return std::nullopt;
}

// The entities that the image keeps and may share: data with default
// visibility in a COMDAT that it keeps and no DLL's instance preempted, under
// its record's name for a thread-local variable, and functions whose
// addresses are significant and not a DLL's, from objects that list what has
// default visibility. Names that MSVC mangles keep the per-image copies their
// code expects.
static std::vector<std::string> getKeptEntities(SymbolTable &symtab) {
  std::vector<std::string> kept;
  auto add = [&](Symbol *s) {
    auto *d = dyn_cast<DefinedRegular>(s);
    auto *file = d ? dyn_cast<ObjFile>(d->getFile()) : nullptr;
    if (!file || !d->getChunk()->live || !file->listsBindings() ||
        !file->hasDefaultVisibility(d) || !isShareableVagueName(d->getName()))
      return;
    kept.push_back(symtab.getSharedName(d).str());
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
                                                 Closure &closure,
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
  Closure closure;
  for (uint32_t i : readImportedRecords(ctx, closure, liveOnly)) {
    const Image &image = closure.images[i];
    if (!image.preemptible.empty())
      offers[image.name].insert_range(image.preemptible);
  }
  return offers;
}

OverrideRecords readImportedOverrideRecords(COFFLinkerContext &ctx,
                                            ArrayRef<MemoryBufferRef> records,
                                            uint32_t version) {
  OverrideRecords result;
  for (MemoryBufferRef mb : records) {
    Expected<std::unique_ptr<COFFObjectFile>> obj = COFFObjectFile::create(mb);
    if (!obj) {
      consumeError(obj.takeError());
      continue;
    }
    for (const SectionRef &sec : (*obj)->sections()) {
      Expected<StringRef> name = sec.getName();
      Expected<StringRef> contents = sec.getContents();
      if (!name || !contents || *name != ".llvm_link_records") {
        consumeError(name.takeError());
        consumeError(contents.takeError());
        continue;
      }
      DataExtractor data(*contents, /*IsLittleEndian=*/true);
      DataExtractor::Cursor cur(0);
      if (data.getBytes(cur, sizeof(LinkRecordsMagic)) !=
              StringRef(LinkRecordsMagic, sizeof(LinkRecordsMagic)) ||
          data.getULEB128(cur) != LinkRecordsVersion) {
        consumeError(cur.takeError());
        continue;
      }
      data.getULEB128(cur);
      while (cur && !data.eof(cur)) {
        uint64_t kind = data.getULEB128(cur);
        uint64_t size = data.getULEB128(cur);
        uint64_t start = cur.tell();
        data.skip(cur, size);
        if (!cur || (kind != LinkRecordOverridableByName &&
                     kind != LinkRecordOverrideSlots))
          continue;
        DataExtractor group(contents->substr(start, size), true);
        DataExtractor::Cursor c(0);
        auto getString = [&] {
          uint64_t n = group.getULEB128(c);
          return group.getBytes(c, n);
        };
        if (kind == LinkRecordOverrideSlots) {
          uint64_t format = group.getULEB128(c);
          if (c && format != version) {
            Err(ctx) << mb.getBufferIdentifier()
                     << ": the C++ runtime's override table format (version "
                     << format << ") is not the one this linker writes "
                     << "(version " << version << ")";
            consumeError(c.takeError());
            continue;
          }
        }
        while (c && !group.eof(c)) {
          StringRef s = getString();
          if (kind == LinkRecordOverridableByName) {
            if (c)
              result.byName.push_back(s.str());
            continue;
          }
          uint64_t slot = group.getULEB128(c);
          uint64_t forward = group.getULEB128(c);
          uint64_t flags = group.getULEB128(c);
          if (c)
            result.slots.push_back(
                {s.str(), uint32_t(slot),
                 forward ? std::optional<uint32_t>(forward - 1) : std::nullopt,
                 (flags & 1) != 0});
        }
        consumeError(c.takeError());
      }
      consumeError(cur.takeError());
    }
  }
  return result;
}

std::vector<std::pair<std::string, std::string>>
getDependentOverrideOffers(COFFLinkerContext &ctx,
                           ArrayRef<MemoryBufferRef> records) {
  Closure closure;
  std::vector<uint32_t> queue;
  for (MemoryBufferRef mb : records)
    if (std::optional<uint32_t> i = readRecord(ctx, mb, closure);
        i && !llvm::is_contained(queue, *i))
      queue.push_back(*i);
  // Breadth first from the DLLs whose records the import libraries hold,
  // which make their own offers through their offer symbols.
  BitVector seen(closure.images.size());
  for (uint32_t i : queue)
    seen.set(i);
  size_t roots = queue.size();
  std::vector<std::pair<std::string, std::string>> offers;
  for (size_t next = 0; next != queue.size(); ++next) {
    const Image &image = closure.images[queue[next]];
    if (next >= roots)
      for (const std::string &offer : image.offers)
        offers.emplace_back(image.name, offer);
    for (uint32_t i : image.imports)
      if (!seen.test(i)) {
        seen.set(i);
        queue.push_back(i);
      }
  }
  return offers;
}

std::vector<std::pair<std::string, std::string>>
chooseVagueOwners(COFFLinkerContext &ctx, const DenseSet<StringRef> &kept) {
  Closure closure;
  std::vector<uint32_t> direct =
      readImportedRecords(ctx, closure, /*liveOnly=*/false);
  // The images in ELF's breadth-first load order from the executable: those
  // loaded eagerly, then those delay-loaded, which join their importer's scope
  // when they load, so that lazy loading never moves an instance.
  std::vector<uint32_t> order;
  BitVector seen(closure.images.size());
  auto visit = [&](uint32_t i) {
    if (!seen.test(i)) {
      seen.set(i);
      order.push_back(i);
    }
  };
  std::vector<uint32_t> delayed;
  DenseSet<uint32_t> lazy;
  for (uint32_t i : direct) {
    if (ctx.config.delayLoads.contains(closure.images[i].name))
      delayed.push_back(i);
    else
      visit(i);
  }
  for (size_t next = 0;;) {
    for (; next != order.size(); ++next) {
      const Image &image = closure.images[order[next]];
      for (uint32_t j : image.imports)
        visit(j);
      llvm::append_range(delayed, image.delayLoads);
    }
    if (delayed.empty())
      break;
    for (uint32_t i : delayed) {
      visit(i);
      lazy.insert(i);
    }
    delayed.clear();
  }
  // The first image to keep each entity preemptible, and how many do.
  StringMap<std::pair<uint32_t, uint32_t>> keepers;
  for (uint32_t i : order)
    for (const std::string &name : closure.images[i].preemptible) {
      auto [it, inserted] = keepers.try_emplace(name, i, 0);
      ++it->second.second;
    }
  std::vector<std::pair<std::string, std::string>> owners;
  for (auto &[name, keeper] : keepers) {
    bool own = kept.contains(name);
    if (keeper.second + own < 2)
      continue;
    // An entity that only delay-loaded DLLs keep has no instance at start-up
    // for the executable to import; the first of them to load keeps its own,
    // and the others' searches find it.
    if (!own && lazy.contains(keeper.first))
      continue;
    owners.emplace_back(name.str(), own ? std::string()
                                        : closure.images[keeper.first].name);
  }
  llvm::sort(owners);
  return owners;
}

void checkVagueEntities(COFFLinkerContext &ctx, StringRef imageName) {
  Configuration &config = ctx.config;
  if (!config.importSlots || ctx.hybridSymtab || isArm64EC(config.machine))
    return;
  Closure closure;
  std::vector<uint32_t> direct = readImportedRecords(ctx, closure);
  if (direct.empty())
    return;
  uint32_t self = closure.add(imageName);
  closure.images[self].entities = getKeptEntities(ctx.symtab);
  closure.images[self].imports = direct;
  // An executable owns what it keeps, through the table it writes for the
  // entities two images keep.
  for (const std::string &entity : closure.images[self].entities)
    if (!config.dll ||
        ctx.symtab.isPreemptibleVagueEntity(ctx.symtab.findVagueEntity(entity)))
      closure.images[self].preemptible.push_back(entity);

  // Which images each image reaches through its imports.
  size_t n = closure.images.size();
  std::vector<BitVector> reaches(n, BitVector(n));
  for (size_t i = 0; i != n; ++i) {
    SmallVector<uint32_t, 8> work(closure.images[i].imports.begin(),
                                  closure.images[i].imports.end());
    while (!work.empty()) {
      uint32_t j = work.pop_back_val();
      if (reaches[i][j])
        continue;
      reaches[i].set(j);
      llvm::append_range(work, closure.images[j].imports);
    }
  }

  StringMap<SmallVector<uint32_t, 2>> keepers;
  for (auto [i, image] : llvm::enumerate(closure.images))
    for (const std::string &entity : image.entities)
      keepers[entity].push_back(i);

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
        // run time, the one the executable's table names.
        if (llvm::is_contained(x.preemptible, entity) &&
            llvm::is_contained(y.preemptible, entity))
          continue;
        // An image that keeps its copy while importing from one that keeps
        // the entity too predates the other's definition, or does not take
        // its offer.
        const Image *importer = reaches[images[a]][images[b]]   ? &x
                                : reaches[images[b]][images[a]] ? &y
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
                 << (importer == &closure.images[self]
                         ? "take " + owner.name + "'s offer"
                         : "relink " + importer->name);
      }
  }
}

void checkOfferAdditions(COFFLinkerContext &ctx, StringRef dllName) {
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
  Expected<std::unique_ptr<Archive>> archive =
      Archive::create((*mb)->getMemBufferRef());
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
    if (name.starts_with(getRecordSymbol(""))) {
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
  Closure closure;
  std::optional<uint32_t> previous;
  if (record)
    previous = readRecord(ctx, *record, closure);
  if (!previous) {
    Err(ctx) << config.offersBaseline << " holds no record of the vague "
             << "entities " << dllName << " keeps";
    return;
  }
  StringSet<> before;
  for (const std::string &entity : closure.images[*previous].entities)
    before.insert(entity);
  // A DLL linked against the previous release keeps its own copy of an entity
  // that this release starts to define until it is relinked.
  for (const std::string &entity : getKeptEntities(ctx.symtab))
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
  std::vector<std::string> kept = getKeptEntities(ctx.symtab);
  SHA256 hash;
  for (const std::string &entity : kept) {
    hash.update(entity);
    hash.update(ArrayRef<uint8_t>{0});
  }
  std::array<uint8_t, 32> digest = hash.final();
  return saver().save("__llvm_offers$" + toHex(ArrayRef(digest).take_front(8),
                                               /*LowerCase=*/true));
}

void addOffersEpoch(COFFLinkerContext &ctx) {
  Configuration &config = ctx.config;
  SymbolTable &symtab = ctx.symtab;
  if (!config.importSlots || !config.offersEpoch || !config.dll ||
      ctx.hybridSymtab || isArm64EC(config.machine))
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
  Configuration &config = ctx.config;
  // A DLL that offers nothing, of its own or forwarded, needs no epoch.
  if (!config.importSlots || !config.dll || ctx.hybridSymtab ||
      isArm64EC(config.machine) ||
      (ctx.symtab.forwardedImports.empty() &&
       getKeptEntities(ctx.symtab).empty()))
    return;
  Closure closure;
  for (uint32_t i : readImportedRecords(ctx, closure)) {
    const Image &owner = closure.images[i];
    if (owner.epoch.empty())
      continue;
    // The import refuses this DLL's load beside a release of the owner whose
    // offers have grown.
    auto *lazy = dyn_cast_or_null<LazyArchive>(
        ctx.symtab.find(("__imp_" + owner.epoch).c_str()));
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
  if (!config.importSlots || ctx.hybridSymtab || isArm64EC(config.machine))
    return;
  Closure closure;
  uint32_t self = closure.add(dllName);
  std::vector<uint32_t> direct = readImportedRecords(ctx, closure);
  // The DLLs the image imports without a record are listed with no entities,
  // so that the closure keeps the path through them.
  for (const std::string &dll : getImportedDLLs(ctx)) {
    uint32_t i = closure.add(dll);
    if (!llvm::is_contained(direct, i))
      direct.push_back(i);
  }
  // A DLL the image delay-loads is listed apart: it is no binding path.
  for (uint32_t i : direct)
    if (config.delayLoads.contains(closure.images[i].name))
      closure.images[self].delayLoads.push_back(i);
  llvm::erase_if(direct, [&](uint32_t i) {
    return config.delayLoads.contains(closure.images[i].name);
  });
  closure.images[self].entities = getKeptEntities(ctx.symtab);
  closure.images[self].imports = direct;
  closure.images[self].epoch = ctx.symtab.offersEpoch.str();
  for (const Export &e : ctx.symtab.exports)
    if (StringRef name = e.extName; name.consume_front(overrideOfferPrefix))
      closure.images[self].offers.push_back(name.str());
  for (const Export &e : ctx.symtab.exports)
    if (e.sym && e.forwardTo.empty() &&
        ctx.symtab.isPreemptibleVagueEntity(e.sym))
      closure.images[self].preemptible.push_back(e.sym->getName().str());
  llvm::sort(closure.images[self].preemptible);

  std::string group;
  raw_string_ostream g(group);
  auto putString = [&](StringRef s) {
    encodeULEB128(s.size(), g);
    g << s;
  };
  encodeULEB128(closure.images.size(), g);
  for (const Image &image : closure.images) {
    putString(image.name);
    encodeULEB128(image.imports.size(), g);
    for (uint32_t i : image.imports)
      encodeULEB128(i, g);
    encodeULEB128(image.entities.size(), g);
    for (const std::string &entity : image.entities)
      putString(entity);
    putString(image.epoch);
  }
  std::string records;
  raw_string_ostream r(records);
  r.write(LinkRecordsMagic, sizeof(LinkRecordsMagic));
  encodeULEB128(LinkRecordsVersion, r);
  encodeULEB128(0, r);
  encodeULEB128(LinkRecordVagueEntities, r);
  encodeULEB128(group.size(), r);
  r << group;
  // The overridable defaults each image of the closure offers, so that an
  // executable that does not link the import library of a DLL its imports
  // depend on still selects that DLL's offer in load order.
  std::string offers;
  raw_string_ostream o(offers);
  for (auto [i, image] : llvm::enumerate(closure.images)) {
    if (image.offers.empty())
      continue;
    encodeULEB128(i, o);
    encodeULEB128(image.offers.size(), o);
    for (const std::string &offer : image.offers) {
      encodeULEB128(offer.size(), o);
      o << offer;
    }
  }
  if (!offers.empty()) {
    encodeULEB128(LinkRecordOverrideOffers, r);
    encodeULEB128(offers.size(), r);
    r << offers;
  }
  // The entities each image of the closure keeps preemptible, so that an
  // executable that sees two images keeping one picks its owner.
  std::string preemptible;
  raw_string_ostream p(preemptible);
  for (auto [i, image] : llvm::enumerate(closure.images)) {
    if (image.preemptible.empty())
      continue;
    encodeULEB128(i, p);
    encodeULEB128(image.preemptible.size(), p);
    for (const std::string &name : image.preemptible) {
      encodeULEB128(name.size(), p);
      p << name;
    }
  }
  if (!preemptible.empty()) {
    encodeULEB128(LinkRecordPreemptibleEntities, r);
    encodeULEB128(preemptible.size(), r);
    r << preemptible;
  }
  // The DLLs each image of the closure delay-loads.
  std::string delayLoads;
  raw_string_ostream d(delayLoads);
  for (auto [i, image] : llvm::enumerate(closure.images)) {
    if (image.delayLoads.empty())
      continue;
    encodeULEB128(i, d);
    encodeULEB128(image.delayLoads.size(), d);
    for (uint32_t j : image.delayLoads)
      encodeULEB128(j, d);
  }
  if (!delayLoads.empty()) {
    encodeULEB128(LinkRecordDelayLoads, r);
    encodeULEB128(delayLoads.size(), r);
    r << delayLoads;
  }
  // The defaults the DLL defines and exports with no fixed slot, which an
  // image that links its import library replaces, offers or reaches by name.
  std::vector<StringRef> byName;
  for (const Export &e : ctx.symtab.exports)
    if (e.sym && e.extName.empty() &&
        ctx.symtab.overridablesByName.contains(e.sym->getName()))
      byName.push_back(e.sym->getName());
  llvm::sort(byName);
  byName.erase(llvm::unique(byName), byName.end());
  if (!byName.empty()) {
    std::string names;
    raw_string_ostream n(names);
    for (StringRef name : byName) {
      encodeULEB128(name.size(), n);
      n << name;
    }
    encodeULEB128(LinkRecordOverridableByName, r);
    encodeULEB128(names.size(), r);
    r << names;
  }
  // The defaults the DLL defines and exports with a fixed slot, as their
  // definitions record it, which an image that links its import library
  // replaces or reaches through that slot.
  std::vector<std::pair<StringRef, SymbolTable::OverrideSlotFacts>> bySlot;
  for (const Export &e : ctx.symtab.exports)
    if (e.sym && isa<Defined>(e.sym) && !isa<DefinedImportData>(e.sym))
      if (auto it = ctx.symtab.overrideSlotFacts.find(e.sym->getName());
          it != ctx.symtab.overrideSlotFacts.end() && it->second.own)
        bySlot.emplace_back(it->first, it->second);
  llvm::sort(bySlot, llvm::less_first());
  bySlot.erase(
      llvm::unique(bySlot, [](const auto &a,
                              const auto &b) { return a.first == b.first; }),
      bySlot.end());
  if (!bySlot.empty()) {
    std::string slots;
    raw_string_ostream o(slots);
    encodeULEB128(overrideTableVersion, o);
    for (auto &[name, facts] : bySlot) {
      encodeULEB128(name.size(), o);
      o << name;
      encodeULEB128(facts.slot, o);
      encodeULEB128(facts.forward ? *facts.forward + 1 : 0, o);
      encodeULEB128(facts.dropsSecond, o);
    }
    encodeULEB128(LinkRecordOverrideSlots, r);
    encodeULEB128(slots.size(), r);
    r << slots;
  }
  members.push_back(createLinkerFacts(
      dllName, config.machine, ".llvm_link_records",
      arrayRefFromStringRef(records), getRecordSymbol(dllName), buffer));
}
} // namespace lld::coff
