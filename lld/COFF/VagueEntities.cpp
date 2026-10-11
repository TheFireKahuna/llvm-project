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
  return "__llvm_vague$" + dll.lower();
}

namespace {
// One image of a load closure: the entities it keeps, the images it imports
// from, by index, and the name of the epoch it exports, if any.
struct Image {
  std::string name;
  std::vector<std::string> entities;
  std::vector<uint32_t> imports;
  std::string epoch;
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
    while (cur && !data.eof(cur)) {
      uint64_t kind = data.getULEB128(cur);
      uint64_t size = data.getULEB128(cur);
      uint64_t start = cur.tell();
      data.skip(cur, size);
      if (!cur || kind != LinkRecordVagueEntities)
        continue;
      DataExtractor group(contents->substr(start, size), true);
      DataExtractor::Cursor c(0);
      auto getString = [&] {
        uint64_t n = group.getULEB128(c);
        return group.getBytes(c, n);
      };
      uint64_t count = group.getULEB128(c);
      SmallVector<uint32_t, 0> index;
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
      return index[0];
    }
  }
  return std::nullopt;
}

// The entities that the image keeps and may share: data with default
// visibility in a COMDAT that it keeps and no DLL's instance preempted, and
// functions whose addresses are significant and not a DLL's, from objects
// that list what has default visibility. Names that MSVC mangles keep the
// per-image copies their code expects.
static std::vector<std::string> getKeptEntities(SymbolTable &symtab) {
  std::vector<std::string> kept;
  auto add = [&](Symbol *s) {
    auto *d = dyn_cast<DefinedRegular>(s);
    auto *file = d ? dyn_cast<ObjFile>(d->getFile()) : nullptr;
    if (!file || !d->getChunk()->live || !file->listsBindings() ||
        !file->hasDefaultVisibility(d) || !isShareableVagueName(d->getName()))
      return;
    kept.push_back(d->getName().str());
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

// The DLLs the image imports statically, which its load closure holds.
static std::vector<std::string> getImportedDLLs(COFFLinkerContext &ctx) {
  std::vector<std::string> dlls;
  StringSet<> seen;
  for (ImportFile *file : ctx.importFileInstances) {
    std::string dll = StringRef(file->dllName).lower();
    if (file->live && seen.insert(dll).second)
      dlls.push_back(dll);
  }
  return dlls;
}

// Reads the records of the DLLs the image imports, which their import
// libraries hold, into closure, and returns the indices of those it read.
static std::vector<uint32_t> readImportedRecords(COFFLinkerContext &ctx,
                                                 Closure &closure) {
  std::vector<uint32_t> direct;
  for (const std::string &dll : getImportedDLLs(ctx))
    if (auto *lazy = dyn_cast_or_null<LazyArchive>(
            ctx.symtab.find(getRecordSymbol(dll))))
      if (std::optional<uint32_t> i =
              readRecord(ctx, lazy->getMemberBuffer(), closure))
        direct.push_back(*i);
  return direct;
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
  closure.images[self].entities = getKeptEntities(ctx.symtab);
  closure.images[self].imports = direct;
  closure.images[self].epoch = ctx.symtab.offersEpoch.str();

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
  members.push_back(createLinkerFacts(
      dllName, config.machine, ".llvm_link_records",
      arrayRefFromStringRef(records), getRecordSymbol(dllName), buffer));
}
} // namespace lld::coff
