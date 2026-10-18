//===- ImportSlots.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ImportSlots.h"
#include "COFFLinkerContext.h"
#include "DLL.h"
#include "InputFiles.h"
#include "KCFI.h"
#include "MarkLive.h"
#include "OverrideTable.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "VagueEntities.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "Relax.h"
#include "lld/Common/AArch64Instructions.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/TinyPtrVector.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld::aarch64;

namespace lld::coff {
namespace {
// A NUL-terminated UTF-16 string, the form ntdll takes a module's name in.
class WideStringChunk : public NonSectionChunk {
public:
  explicit WideStringChunk(StringRef s) {
    convertUTF8ToUTF16String(s, str);
    setAlignment(2);
  }
  size_t getSize() const override { return 2 * (str.size() + 1); }
  void writeTo(uint8_t *buf) const override {
    for (size_t i = 0; i != str.size(); ++i)
      write16le(buf + 2 * i, str[i]);
    write16le(buf + 2 * str.size(), 0);
  }

private:
  SmallVector<UTF16, 0> str;
};

// A word through which the image reaches what the program may give another
// image's instance of: an overridable default or a preemptible vague entity.
// The fill writes the program's choice over the loader's value.
struct OverrideCell {
  // The word, or null for the import's own entry.
  Chunk *chunk;
  uint32_t offset;
  // The import, or null for the image's own default or entity.
  DefinedImportData *imp;
  // The override table's slot, or the name's key where the key is nonzero.
  uint32_t slot;
  uint64_t key;
  bool readOnly;
  bool threadLocal;
  // A vague entity, whose table entry names a word holding the instance, and
  // the name by which a DLL loaded at run time searches for it.
  bool indirect = false;
  StringRef name = {};

  uint64_t getRVA() const {
    return chunk ? chunk->getRVA() + offset : imp->getRVA();
  }
  // A slot's key is the offset of its RVA in the executable's header and the
  // slot, so that the fill's list sorts by key.
  uint64_t getKey() const {
    return key ? key
               : uint64_t(overrideSlotOffset(slot) + 4) << 32 | uint64_t(slot)
                                                                    << 1;
  }
};

// The template start and index that the TLS directory names.
struct TLSTemplate {
  Defined *start = nullptr;
  int64_t startAddend = 0;
  Defined *index = nullptr;
  int64_t indexAddend = 0;

  uint64_t getIndexRVA() const { return index->getRVA() + indexAddend; }
  uint64_t getStartRVA() const { return start->getRVA() + startAddend; }
};

// The list that the builtins' __llvm_thread_local_slot_copy reads, laid out as
// fill_windows.c describes.
class CopyListChunk : public NonSectionChunk {
public:
  CopyListChunk(std::vector<const ImportSlot *> slots, TLSTemplate tls)
      : slots(std::move(slots)), tls(tls) {
    setAlignment(8);
  }
  size_t getSize() const override { return 16 * (slots.size() + 1); }
  void writeTo(uint8_t *buf) const override {
    write32le(buf, slots.size());
    write32le(buf + 4, tls.getIndexRVA());
    write32le(buf + 8, tls.getStartRVA());
    for (const ImportSlot *s : slots) {
      buf += 16;
      write32le(buf, s->chunk->getRVA() + s->offset);
      s->lookup->writeTo(buf + 8);
    }
  }
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }

private:
  std::vector<const ImportSlot *> slots;
  TLSTemplate tls;
};

// The list that the builtins' __llvm_fill works through, laid out as
// fill_windows.c describes. It leaves room for a range of read-only cells for
// each output section their input sections name, which merging can only
// reduce.
enum : uint32_t {
  fillCellByName = 0,
  fillCellNoName = 1,
  fillCellSlot = 2,
  fillCellThreadLocal = 4,
};

class FillListChunk : public NonSectionChunk {
public:
  FillListChunk(COFFLinkerContext &ctx, std::vector<OverrideCell> cells)
      : cells(std::move(cells)), ctx(ctx) {
    setAlignment(16);
    llvm::stable_sort(this->cells,
                      [](const OverrideCell &a, const OverrideCell &b) {
                        return a.getKey() < b.getKey();
                      });
    // An import's own entry is in .rdata.
    StringSet<> names;
    for (const OverrideCell &c : this->cells)
      if (auto *sc = dyn_cast_or_null<SectionChunk>(c.chunk); c.readOnly)
        names.insert(sc ? getOutputSectionName(
                              getPartialSectionName(sc, ctx.config.mingw),
                              ctx.config.mingw)
                        : ".rdata");
    maxRanges = names.size();
  }
  static constexpr uint32_t headerSize = 48;
  size_t getSize() const override {
    return headerSize + 8 * maxRanges +
           16 * (cells.size() + runTimeCells.size() + words.size());
  }
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }
  // One range of read-only cells for each output section, which the fill makes
  // writable and read-only again: a range across sections would change those
  // between them.
  SmallVector<std::pair<uint64_t, uint64_t>, 0> getReadOnlyRanges() const;

  std::vector<OverrideCell> cells;
  std::vector<RunTimeCell> runTimeCells;
  std::vector<ResidualWord> words;
  size_t maxRanges;
  TLSTemplate tls;
  // Where a vague entity's name is: its export's, or its hint/name record's.
  DenseMap<StringRef, const Export *> exports;
  const DenseMap<DefinedImportData *, Chunk *> *hintOf = nullptr;
  // A chunk of the section the fill seals, or null.
  Chunk *sealed = nullptr;
  uint64_t selfKey = 0;
  bool callsCopy = false;

private:
  uint32_t getKind(const OverrideCell &c) const;
  COFFLinkerContext &ctx;
};

SmallVector<std::pair<uint64_t, uint64_t>, 0>
FillListChunk::getReadOnlyRanges() const {
  MapVector<OutputSection *, std::pair<uint64_t, uint64_t>> bySection;
  for (const OverrideCell &c : cells) {
    if (!c.readOnly)
      continue;
    Chunk *chunk = c.chunk ? c.chunk : c.imp->getChunk();
    uint64_t start = c.getRVA(), end = start + ctx.config.wordsize;
    auto [it, inserted] =
        bySection.try_emplace(ctx.getOutputSection(chunk), start, end);
    if (!inserted) {
      it->second.first = std::min(it->second.first, start);
      it->second.second = std::max(it->second.second, end);
    }
  }
  auto ranges = llvm::to_vector_of<std::pair<uint64_t, uint64_t>>(
      llvm::make_second_range(bySection));
  llvm::sort(ranges);
  return ranges;
}

uint32_t FillListChunk::getKind(const OverrideCell &c) const {
  if (!c.indirect)
    return (c.key ? fillCellByName : fillCellSlot) |
           (c.threadLocal ? fillCellThreadLocal : 0);
  // A hint/name record holds the name after its hint.
  if (c.imp) {
    Chunk *hint = hintOf->lookup(c.imp);
    return hint ? hint->getRVA() + 2 : fillCellNoName;
  }
  const Export *e = exports.lookup(c.name);
  return e && e->nameChunk ? e->nameChunk->getRVA() : fillCellNoName;
}

void FillListChunk::writeTo(uint8_t *buf) const {
  SmallVector<std::pair<uint64_t, uint64_t>, 0> ranges = getReadOnlyRanges();
  ranges.resize(std::min(ranges.size(), maxRanges));
  OutputSection *seal = sealed ? ctx.getOutputSection(sealed) : nullptr;
  write32le(buf, ranges.size());
  write32le(buf + 4, cells.size());
  write32le(buf + 8, runTimeCells.size());
  write32le(buf + 12, words.size());
  if (tls.index) {
    write32le(buf + 16, tls.getIndexRVA());
    write32le(buf + 20, tls.getStartRVA());
  }
  if (seal) {
    write32le(buf + 24, seal->getRVA());
    write32le(buf + 28, seal->getVirtualSize());
  }
  write64le(buf + 32, selfKey);
  write32le(buf + 40, callsCopy);
  buf += headerSize;
  for (auto [start, end] : ranges) {
    write32le(buf, start);
    write32le(buf + 4, end - start);
    buf += 8;
  }
  buf += 8 * (maxRanges - ranges.size());
  for (const OverrideCell &c : cells) {
    write64le(buf, c.getKey() | c.readOnly);
    write32le(buf + 8, c.getRVA());
    write32le(buf + 12, getKind(c));
    buf += 16;
  }
  for (const RunTimeCell &c : runTimeCells) {
    write32le(buf, c.cell->getRVA());
    write32le(buf + 4, c.name->getRVA());
    write32le(buf + 8, c.dllName ? c.dllName->getRVA()
                                 : uint32_t(c.imp->file->isWeakImport));
    write16le(buf + 12, c.name->getSize() - 1);
    write16le(buf + 14, c.dllName ? c.dllName->getSize() - 2 : 0);
    buf += 16;
  }
  for (const ResidualWord &w : words) {
    write32le(buf, w.chunk->getRVA() + w.offset);
    write32le(buf + 4, w.imp->getRVA());
    write64le(buf + 8, w.addend);
    buf += 16;
  }
}

// The table of the vague entities an image shares, which the fill of a DLL
// loaded at run time searches: a DLL's, which its MZ header names, holds the
// words of the entities it keeps preemptible, and the executable's the
// entities it exports. The header {magic, flags, buckets, 0, filter mask,
// delay loads, their offset, the buckets' offset} is followed by a filter of
// two bits per key, as in ELF's DT_GNU_HASH, the buckets {key's top half, RVA,
// name's RVA}, and the keys of the DLLs the image delay-loads. A quarter of the
// buckets are free, so a probe ends at one.
class VagueOfferTableChunk : public NonSectionChunk {
public:
  struct Offer {
    uint64_t key = 0;
    // The word that holds the instance, or the instance itself.
    Chunk *cell = nullptr;
    Defined *instance = nullptr;
    const Export *name = nullptr;
    uint64_t getRVA() const {
      return cell ? cell->getRVA() : instance->getRVA();
    }
  };
  VagueOfferTableChunk(std::vector<Offer> offers,
                       std::vector<uint64_t> delayLoads, bool direct)
      : delayLoads(std::move(delayLoads)), direct(direct) {
    setAlignment(16);
    // A filter word for each four entities.
    buckets.resize(offers.empty() ? 0 : offers.size() + offers.size() / 3 + 1);
    filter.resize(llvm::PowerOf2Ceil(std::max<size_t>(offers.size() / 4, 1)));
    llvm::sort(offers,
               [](const Offer &a, const Offer &b) { return a.key < b.key; });
    for (const Offer &o : offers) {
      filter[o.key >> 13 & (filter.size() - 1)] |=
          uint64_t(1) << (o.key >> 1 & 63) | uint64_t(1) << (o.key >> 7 & 63);
      for (uint64_t b = (o.key >> 32) * buckets.size() >> 32;;
           b = b + 1 == buckets.size() ? 0 : b + 1)
        if (!buckets[b].key) {
          buckets[b] = o;
          break;
        }
    }
  }
  static constexpr uint32_t headerSize = 32;
  uint32_t getBucketsOffset() const { return headerSize + 8 * filter.size(); }
  static constexpr uint32_t bucketSize = 12;
  size_t getSize() const override {
    return getBucketsOffset() + bucketSize * buckets.size() +
           8 * delayLoads.size();
  }
  void writeTo(uint8_t *buf) const override {
    uint32_t delayOffset = getBucketsOffset() + bucketSize * buckets.size();
    write32le(buf, vagueOfferTableMagic);
    write32le(buf + 4, direct);
    write32le(buf + 8, buckets.size());
    write32le(buf + 12, 0);
    write32le(buf + 16, filter.size() - 1);
    write32le(buf + 20, delayLoads.size());
    write32le(buf + 24, delayOffset);
    write32le(buf + 28, getBucketsOffset());
    uint8_t *p = buf + headerSize;
    for (uint64_t word : filter) {
      write64le(p, word);
      p += 8;
    }
    for (const Offer &o : buckets) {
      if (o.key) {
        write32le(p, o.key >> 32);
        write32le(p + 4, o.getRVA());
        write32le(p + 8, o.name->nameChunk->getRVA());
      }
      p += bucketSize;
    }
    for (uint64_t key : delayLoads) {
      write64le(p, key);
      p += 8;
    }
  }
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }

private:
  std::vector<Offer> buckets;
  std::vector<uint64_t> filter;
  std::vector<uint64_t> delayLoads;
  bool direct;
};
} // namespace

// Unwind and exception data, which hold RVAs of the image's own code.
static bool isExceptionData(SectionChunk *sc) {
  StringRef name = sc->getSectionName();
  return name == ".xdata" || name == ".pdata" || name.starts_with(".xdata$") ||
         name.starts_with(".pdata$");
}

DefinedImportData *getAddressedImport(Symbol *s) {
  if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s))
    return thunk->wrappedSym;
  // Data resolved to its import is a copy of the import's symbol.
  auto *data = dyn_cast_or_null<DefinedImportData>(s);
  if (!data || !data->isRuntimePseudoReloc)
    return nullptr;
  return cast<DefinedImportData>(data->file->impSym);
}

DefinedImportData *getInPlaceImport(Symbol *s) {
  DefinedImportData *imp = getAddressedImport(s);
  return imp && !imp->file->isDelayLoaded() ? imp : nullptr;
}

std::optional<int64_t> getAddressWordAddend(SectionChunk *sc,
                                            const coff_relocation &rel) {
  const Configuration &config = sc->file->symtab.ctx.config;
  if (uint64_t(rel.VirtualAddress) + config.wordsize > sc->getSize())
    return std::nullopt;
  const uint8_t *word = sc->getContents().data() + rel.VirtualAddress;
  return config.is64() ? int64_t(read64le(word)) : int32_t(read32le(word));
}

// Whether rel, a reference to an import's entry, reads the whole address:
// a described load, an ARM64 adrp or ldr, or a KCFI list entry.
static bool readsWholeEntry(SectionChunk *sc, const coff_relocation &rel,
                            bool code, bool arm64) {
  if (code && arm64)
    return isArm64PointerLoad(sc, rel);
  if (code) {
    std::optional<LinkSiteForm> form = getDescribedSite(sc, rel);
    return form && *form != LinkSiteAddress;
  }
  return isKCFIListEntry(sc, rel);
}

// What the scan of the image's relocations gathers for the decisions after it.
struct SlotScan {
  // A section's candidate slots.
  struct Pending {
    SectionChunk *chunk;
    std::vector<ImportSlot> slots;
    Symbol *firstRef = nullptr;
  };
  // An instruction rewritten for imp, unless its thunk becomes its address.
  struct Site {
    SectionChunk *chunk;
    RelaxEdit edit;
    DefinedImportData *imp;
  };
  // How references other than slots use an import's address.
  enum Use : uint8_t { Unused, Read, Other };

  void use(DefinedImportData *imp, Use u) {
    Use &v = uses[imp->file];
    v = std::max(v, u);
  }

  std::vector<Pending> pending;
  std::vector<Site> addressSites;
  // ARM64 loads of a delay-loaded function's entry, rewritten all or none,
  // since an adrp may serve several loads.
  MapVector<DefinedImportData *,
            std::vector<std::pair<SectionChunk *, RelaxEdit>>>
      delayLoads;
  DenseSet<DefinedImportData *> delayLoadsRead;
  // For each import thunk, its references and those that bypass it.
  MapVector<DefinedImportThunk *, std::pair<size_t, size_t>> thunkRefs;
  DenseMap<ImportFile *, Use> uses;
  // For each offered function, the references that keep the image's copy.
  DenseMap<Symbol *, size_t> localFunctionRefs;
  // Per object: the symbols reported, and the indices its guard metadata
  // lists as address-taken, once read.
  SmallPtrSet<Symbol *, 4> reported;
  std::optional<DenseSet<uint32_t>> guardFids;
};

// Under -import-slots, a word of static data holding an import's address is an
// in-place import slot, which the loader writes as an entry of an import
// address table made of the run the word belongs to. Code taking an imported
// function's address in a described instruction, or an ARM64 adrp and add, is
// rewritten to load it from the import's entry, so code and data agree; an
// object that may take it otherwise makes the thunk the function's address
// image-wide. A delay-loaded function keeps its thunk. Any other reference to
// data resolved to its import is an error. One scan gathers what the decisions
// need, as ELF's relocation scan does, and they are taken once it is known
// which functions keep their thunks as their addresses.
void ImportSlotContents::bind() {
  Configuration &config = ctx.config;
  if (!config.importSlots)
    return;
  llvm::TimeTraceScope timeScope("Import slots");

  // An export of data resolved to its import would publish its entry.
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym);
        imp && imp->isRuntimePseudoReloc)
      Err(ctx) << "cannot export " << ctx.symtab.printSymbol(imp)
               << ": it is imported from " << imp->getDLLName()
               << "; export a forwarder to it instead";

  // An import bound at run time has a cell in .sealed instead of an entry.
  // A DLL's KCFI code range bounds, which a thunk loads together, share a
  // 16-byte block.
  DenseMap<DefinedImportData *, DefinedImportData *> rangeEnds;
  DenseSet<DefinedImportData *> rangeEndSet;
  for (const KCFIState::ImportedRange &r : ctx.kcfi.importedRanges) {
    rangeEnds[r.start] = r.end;
    rangeEndSet.insert(r.end);
  }
  auto addCell = [&](DefinedImportData *imp, uint32_t align) {
    if (imp->getExternalName().empty()) {
      Err(ctx) << "cannot bind " << ctx.symtab.printSymbol(imp)
               << ", imported from " << imp->getDLLName()
               << " by ordinal, at run time";
      return;
    }
    RunTimeCell c{imp, make<NullChunk>(ctx),
                  make<StringChunk>(imp->getExternalName()),
                  imp->file->isHostImport
                      ? nullptr
                      : make<WideStringChunk>(imp->getDLLName())};
    c.cell->setAlignment(std::max<uint32_t>(align, c.cell->getAlignment()));
    imp->setLocation(c.cell);
    StringRef name = imp->getName().drop_front(strlen("__imp_"));
    if (Symbol *s = ctx.symtab.find(name))
      ctx.symtab.replaceRefPtr(s, name, imp);
    runTimeNames.push_back(c.name);
    if (c.dllName)
      runTimeNames.push_back(c.dllName);
    runTimeCells.push_back(c);
  };
  for (ImportFile *f : ctx.importFileInstances) {
    DefinedImportData *imp = f->impSym;
    if (&f->symtab != &ctx.symtab || !f->live || !imp ||
        !f->isBoundAtRunTime() || rangeEndSet.contains(imp))
      continue;
    DefinedImportData *end = rangeEnds.lookup(imp);
    addCell(imp, end ? 16 : 1);
    if (end)
      addCell(end, 1);
  }

  if (!ctx.symtab.offeredFunctions.empty())
    keepUndescribedFunctionAddresses();

  SlotScan scan;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab)
      continue;
    scan.reported.clear();
    scan.guardFids.reset();
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (sc && sc->live &&
          !(sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE))
        scanChunk(scan, sc);
    }
  }
  resolve(scan);
}

// In an object built with /guard:cf by cl.exe, which has no x86-64 inline
// assembly, a REL32 to a function-typed symbol its .gfids$y table does not
// list is a branch: a missing address-take would fail at run time.
static bool isGuardMetadataBranch(std::optional<DenseSet<uint32_t>> &fids,
                                  ObjFile *file, const coff_relocation &rel) {
  if (!file->hasGuardCF())
    return false;
  Expected<COFFSymbolRef> sym =
      file->getCOFFObj()->getSymbol(rel.SymbolTableIndex);
  if (!sym) {
    consumeError(sym.takeError());
    return false;
  }
  if (sym->getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION)
    return false;
  if (!fids) {
    fids.emplace();
    for (SectionChunk *c : file->getGuardFidChunks()) {
      ArrayRef<uint8_t> data = c->getContents();
      for (size_t i = 0; i + 4 <= data.size(); i += 4)
        fids->insert(support::endian::read32le(data.data() + i));
    }
  }
  return !fids->contains(rel.SymbolTableIndex);
}

// An offered function whose address an object may take in an undescribed
// instruction keeps the image's copy as its address, so the address is not
// split.
void ImportSlotContents::keepUndescribedFunctionAddresses() {
  MapVector<Symbol *, DefinedImportData *> &offered =
      ctx.symtab.offeredFunctions;
  bool arm64 = ctx.config.machine == ARM64;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab || (!arm64 && file->describesSites()))
      continue;
    std::optional<DenseSet<uint32_t>> guardFids;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live ||
          !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE))
        continue;
      for (const coff_relocation &rel : sc->getRelocs()) {
        if (rel.Type != (arm64 ? IMAGE_REL_ARM64_REL21 : IMAGE_REL_AMD64_REL32))
          continue;
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        auto it = offered.find(s);
        if (it == offered.end() ||
            (!arm64 && isGuardMetadataBranch(guardFids, file, rel)))
          continue;
        Warn(ctx) << file << ": may take the address of "
                  << file->symtab.printSymbol(s) << ", which "
                  << it->second->getDLLName()
                  << " provides, in an instruction it does not describe, so "
                     "the image keeps its own copy as its address; rebuild "
                     "the object with clang";
        offered.erase(it);
      }
    }
  }
}

// A function whose address an object may take in an undescribed instruction,
// or in a 32-bit field, keeps its thunk as its address everywhere.
void ImportSlotContents::useThunkAsAddress(SlotScan &scan, ObjFile *file,
                                           Symbol *s, DefinedImportData *imp,
                                           SectionChunk *data) {
  imp->file->thunkIsAddress = true;
  if (!scan.reported.insert(s).second)
    return;
  if (data)
    Warn(ctx) << file << ": " << data->getSectionName()
              << " holds the address of " << file->symtab.printSymbol(s)
              << ", imported from " << imp->getDLLName()
              << ", in a 32-bit field, so the image uses its import thunk "
                 "as its address";
  else if (ctx.config.warnUndescribedAddress)
    Warn(ctx) << file << ": may take the address of "
              << file->symtab.printSymbol(s) << ", imported from "
              << imp->getDLLName()
              << ", in an instruction it does not describe, so the image "
                 "uses its import thunk as its address; declare it "
                 "imported, or rebuild the object with clang";
}

// A word holding an address inside an import that its DLL exports no name for
// is the fill's to write; a read-only one moves with its chunk to .sealed.
// Thread-local data is copied before the fill runs.
void ImportSlotContents::addResidualWord(SectionChunk *sc, uint32_t offset,
                                         Symbol *s, DefinedImportData *imp,
                                         int64_t addend) {
  Configuration &config = ctx.config;
  StringRef name = getPartialSectionName(sc, config.mingw);
  bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
  bool tls = name.starts_with(".tls");
  if (!tls && (!readOnly || name == ".rdata")) {
    if (readOnly)
      sealedChunks.insert(sc);
    residualWords.push_back({sc, offset, imp, addend, s});
    return;
  }
  Err(ctx) << sc->file << ": " << sc->getSectionName()
           << " holds the address of " << sc->file->symtab.printSymbol(s)
           << " plus " << addend << ", imported from " << imp->getDLLName()
           << ", which exports no name for it"
           << (tls ? ", in thread-local data"
                   : ", in a read-only section that cannot move");
}

void ImportSlotContents::scanChunk(SlotScan &scan, SectionChunk *sc) {
  Configuration &config = ctx.config;
  ObjFile *file = sc->file;
  bool arm64 = config.machine == ARM64;
  bool code = sc->header->Characteristics & IMAGE_SCN_CNT_CODE;
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  SlotScan::Pending p{sc, {}, nullptr};
  // lea becomes mov; the add after an ARM64 adrp becomes ldr.
  auto addressSite = [&](const coff_relocation &r, DefinedImportData *imp) {
    uint32_t off = r.VirtualAddress;
    RelaxEdit edit{off - 3, 7, 7, RelaxEdit::LoadPointer, imp};
    if (arm64)
      edit = {off, 4, 4,
              r.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 ? RelaxEdit::Arm64Page
                                                       : RelaxEdit::Arm64Load,
              imp};
    scan.addressSites.push_back({sc, edit, imp});
  };
  size_t e = relocs.size();
  auto importedBut = [&](Symbol *s, DefinedImportData *imp) {
    COFFSyncStream err = Err(ctx);
    err << file << ": " << file->symtab.printSymbol(s) << " is imported from "
        << imp->getDLLName() << ", but ";
    return err;
  };
  // An ARM64 adrp and add pair, or an instruction its object describes, that
  // takes an address, which the import's entry then gives.
  auto rewriteAddress = [&](size_t &i, DefinedImportData *imp) {
    const coff_relocation &r = relocs[i];
    if (arm64) {
      if (r.Type != IMAGE_REL_ARM64_PAGEBASE_REL21 || i + 1 == e ||
          !isArm64AddressPair(sc, r, relocs[i + 1]))
        return false;
      addressSite(r, imp);
      addressSite(relocs[++i], imp);
      return true;
    }
    const ObjFile::RelocFact *fact = file->relocFact(sc, r);
    if (r.Type != IMAGE_REL_AMD64_REL32 || !fact || !fact->hasSite ||
        fact->form != LinkSiteAddress ||
        !isSiteForm(sc->getContents(), r.VirtualAddress, LinkSiteAddress))
      return false;
    addressSite(r, imp);
    return true;
  };
  for (size_t i = 0; i != e; ++i) {
    const coff_relocation &rel = relocs[i];
    Symbol *s = file->getSymbol(rel.SymbolTableIndex);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s);
    auto *data = dyn_cast_or_null<DefinedImportData>(s);
    if (thunk)
      ++scan.thunkRefs[thunk].first;

    // The address of a function a DLL offers is the DLL's instance, and that
    // of an overridable default this DLL offers the program's choice; calls
    // and other references keep the image's copy.
    DefinedImportData *offer = ctx.symtab.offeredFunctions.lookup(s);
    if (!offer)
      offer = ctx.symtab.preemptedOffers.lookup(s);
    if (offer) {
      if (code && rewriteAddress(i, offer))
        continue;
      if (!code && !isExceptionData(sc) && sc->isAddressWord(rel) &&
          getAddressWordAddend(sc, rel) == 0) {
        p.slots.push_back({sc, rel.VirtualAddress, offer});
        if (!p.firstRef)
          p.firstRef = s;
        continue;
      }
      SectionChunk *copy = cast<DefinedRegular>(s)->getChunk();
      if (sc != copy && llvm::none_of(copy->children(), [&](SectionChunk &c) {
            return &c == sc;
          }))
        ++scan.localFunctionRefs[s];
      continue;
    }

    // An import's entry; its data is reached only through slots.
    if (data && !data->isRuntimePseudoReloc) {
      scan.use(data, readsWholeEntry(sc, rel, code, arm64) ? SlotScan::Read
                                                           : SlotScan::Other);
      // A delay-loaded function's: a load in code takes the thunk's address,
      // mov becoming lea and ldr add.
      if (!data->file->thunkSym || !data->file->isDelayLoaded())
        continue;
      auto *thunk = cast<Defined>(data->file->thunkSym);
      uint32_t off = rel.VirtualAddress;
      if (!code) {
        scan.delayLoadsRead.insert(data);
      } else if (arm64 && isArm64PointerLoad(sc, rel)) {
        scan.delayLoads[data].push_back(
            {sc,
             {off, 4, 4,
              rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 ? RelaxEdit::Arm64Page
                                                         : RelaxEdit::Arm64Add,
              thunk}});
      } else if (arm64) {
        scan.delayLoadsRead.insert(data);
      } else if (std::optional<LinkSiteForm> form = getDescribedSite(sc, rel);
                 form == LinkSiteLoad || form == LinkSiteLoadREX2) {
        uint16_t size = form == LinkSiteLoadREX2 ? 8 : 7;
        scan.addressSites.push_back(
            {sc,
             {off + 4 - size, size, size, RelaxEdit::LoadAddress, thunk},
             data});
      }
      continue;
    }

    DefinedImportData *imp = getAddressedImport(s);
    if (!imp)
      continue;
    // A section-relative offset names a section of this image.
    if (sc->isSectionRelative(rel)) {
      if (scan.reported.insert(s).second)
        importedBut(s, imp)
            << sc->getSectionName() << " refers to it with relocation type "
            << file->getCOFFObj()->getRelocationTypeName(rel.Type)
            << ", which names a section of this image";
      continue;
    }
    if (code) {
      if (!thunk) {
        // Data a DLL's instance preempted is reached as a function's is.
        bool preempted = file->symtab.preemptedData.contains(s);
        if (preempted && rewriteAddress(i, imp))
          continue;
        if (!scan.reported.insert(s).second)
          continue;
        if (preempted)
          Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                   << " is provided by " << imp->getDLLName()
                   << ", but is referenced as if it were local; recompile "
                      "the object, or give the symbol hidden visibility";
        else {
          StringRef name = imp->getName().drop_front(strlen("__imp_"));
          importedBut(s, imp)
              << "the object was compiled as if it were local; mark its "
                 "declaration, or compile the object with "
                 "-fno-direct-access-external-data; in assembly, reach it "
                 "through "
              << imp->getName() << " or "
              << (arm64 ? (":got:" + name).str() : (name + "@GOTPCREL").str());
        }
        continue;
      }
      if (sc->isAddressWord(rel)) {
        Err(ctx) << file << ": " << sc->getSectionName()
                 << " is executable and holds the address of "
                 << file->symtab.printSymbol(s) << ", imported from "
                 << imp->getDLLName();
        continue;
      }
      if (imp->file->isDelayLoaded())
        continue;
      if (arm64) {
        switch (rel.Type) {
        case IMAGE_REL_ARM64_PAGEBASE_REL21:
          if (rewriteAddress(i, imp)) {
            ++scan.thunkRefs[thunk].first;
            scan.thunkRefs[thunk].second += 2;
          } else {
            useThunkAsAddress(scan, file, s, imp);
          }
          break;
        case IMAGE_REL_ARM64_PAGEOFFSET_12A:
        case IMAGE_REL_ARM64_REL21:
          useThunkAsAddress(scan, file, s, imp);
          break;
        default:
          break;
        }
        continue;
      }
      switch (rel.Type) {
      case IMAGE_REL_AMD64_REL32: {
        if (!file->describesSites()) {
          if (!isGuardMetadataBranch(scan.guardFids, file, rel))
            useThunkAsAddress(scan, file, s, imp);
          break;
        }
        std::optional<LinkSiteForm> form;
        if (const ObjFile::RelocFact *fact = file->relocFact(sc, rel);
            fact && fact->hasSite)
          form = fact->form;
        if (form == LinkSiteAddress) {
          if (isSiteForm(sc->getContents(), rel.VirtualAddress,
                         LinkSiteAddress)) {
            addressSite(rel, imp);
            ++scan.thunkRefs[thunk].second;
          } else {
            Err(ctx) << file << ": the instruction at offset 0x"
                     << Twine::utohexstr(rel.VirtualAddress) << " in "
                     << sc->getSectionName()
                     << " is not the one its link-only record describes";
          }
        } else if (form == LinkSiteOther) {
          useThunkAsAddress(scan, file, s, imp);
        }
        break;
      }
      case IMAGE_REL_AMD64_REL32_1:
      case IMAGE_REL_AMD64_REL32_2:
      case IMAGE_REL_AMD64_REL32_3:
      case IMAGE_REL_AMD64_REL32_4:
      case IMAGE_REL_AMD64_REL32_5:
        importedBut(s, imp) << "an instruction in " << sc->getSectionName()
                            << " reads its bytes, which its import thunk's "
                               "are not";
        break;
      default:
        break;
      }
      continue;
    }

    // A 32-bit field cannot hold another image's address. One the function is
    // only called through, which the object lists or, failing that, any in
    // exception data, is served by the thunk; any other makes the thunk the
    // function's address.
    if (!sc->isAddressWord(rel)) {
      const ObjFile::RelocFact *fact = file->relocFact(sc, rel);
      bool callOnly = file->listsCallOnly() ? fact && fact->callOnly
                                            : isExceptionData(sc);
      if (thunk && !callOnly)
        useThunkAsAddress(scan, file, s, imp, sc);
      else if (!thunk && scan.reported.insert(s).second)
        importedBut(s, imp)
            << sc->getSectionName() << " refers to it with relocation type "
            << file->getCOFFObj()->getRelocationTypeName(rel.Type)
            << ", which cannot reach another image";
      continue;
    }
    if (imp->file->isDelayLoaded())
      continue;
    std::optional<int64_t> addend = getAddressWordAddend(sc, rel);
    if (!addend) {
      Err(ctx) << file << ": the address of " << file->symtab.printSymbol(s)
               << " at offset 0x" << Twine::utohexstr(rel.VirtualAddress)
               << " extends past the end of " << sc->getSectionName()
               << " (size 0x" << Twine::utohexstr(sc->getSize()) << ")";
      continue;
    }
    // The loader adds no addend: a word inside the import takes the name its
    // DLL exports for that address.
    if (*addend) {
      DefinedImportData *interior = ctx.symtab.findInteriorImport(imp, *addend);
      if (!interior) {
        if (thunk)
          Err(ctx) << file << ": " << sc->getSectionName()
                   << " holds the address of " << file->symtab.printSymbol(s)
                   << " plus " << *addend << ", imported from "
                   << imp->getDLLName()
                   << ", but no address inside a function is imported";
        else
          addResidualWord(sc, rel.VirtualAddress, s, imp, *addend);
        continue;
      }
      if (!interior->file->live)
        addImport(interior);
      interiorBases.try_emplace(imp->file, sc, rel.VirtualAddress);
      imp = interior;
    }
    p.slots.push_back({sc, rel.VirtualAddress, imp});
    if (thunk)
      ++scan.thunkRefs[thunk].second;
    if (!p.firstRef)
      p.firstRef = s;
  }
  if (!p.slots.empty())
    scan.pending.push_back(std::move(p));
}

void ImportSlotContents::resolve(SlotScan &scan) {
  Configuration &config = ctx.config;
  heldBackChunks.insert(sealedChunks.begin(), sealedChunks.end());

  // A rewritten load of a delay-loaded function takes its thunk's address.
  auto keepThunk = [](DefinedImportData *imp) {
    cast<DefinedImportThunk>(imp->file->thunkSym)->getChunk()->live = true;
  };
  // A load the linker already disposes of needs no rewrite.
  auto addSite = [&](SectionChunk *sc, const RelaxEdit &edit) {
    auto it = ctx.relaxations.find(sc);
    if (it == ctx.relaxations.end() || !it->second.findEdit(edit.offset))
      addEdit(sc, edit);
  };
  for (const SlotScan::Site &site : scan.addressSites) {
    if (site.imp->file->thunkIsAddress)
      continue;
    addSite(site.chunk, site.edit);
    if (site.imp->file->isDelayLoaded())
      keepThunk(site.imp);
    else
      scan.use(site.imp, SlotScan::Read);
  }
  for (auto &[imp, sites] : scan.delayLoads) {
    if (scan.delayLoadsRead.contains(imp))
      continue;
    for (auto &[sc, edit] : sites)
      addSite(sc, edit);
    keepThunk(imp);
  }

  // A thunk every reference bypasses is left out, unless a root or an export
  // names it.
  DenseSet<Symbol *> named(config.gcroot.begin(), config.gcroot.end());
  for (Export &e : ctx.symtab.exports)
    named.insert(e.sym);
  // Words holding an import bound at run time name its thunk until the fill.
  for (auto &[thunk, refs] : scan.thunkRefs)
    if (refs.first == refs.second && !thunk->wrappedSym->file->thunkIsAddress &&
        !thunk->wrappedSym->file->isBoundAtRunTime() &&
        !thunk->wrappedSym->file->isDelayLoaded() && !named.contains(thunk))
      thunk->getChunk()->live = false;

  // An offered function's copy that no call keeps is discarded.
  for (auto &[sym, imp] : ctx.symtab.offeredFunctions) {
    if (scan.localFunctionRefs.lookup(sym))
      continue;
    SectionChunk *copy = cast<DefinedRegular>(sym)->getChunk();
    copy->live = false;
    for (SectionChunk &child : copy->children())
      child.live = false;
  }

  std::vector<SectionChunk *> writable;
  for (SlotScan::Pending &p : scan.pending) {
    SectionChunk *sc = p.chunk;
    // The fill writes a word of an import bound at run time from its cell.
    llvm::erase_if(p.slots, [&](const ImportSlot &s) {
      if (!s.sym->file->isBoundAtRunTime() || s.sym->file->thunkIsAddress)
        return false;
      addResidualWord(sc, s.offset, s.sym, s.sym, 0);
      return true;
    });
    llvm::erase_if(p.slots, [](const ImportSlot &s) {
      return s.sym->file->thunkIsAddress;
    });
    if (p.slots.empty())
      continue;
    // An identical duplicate relocation is one slot.
    llvm::stable_sort(p.slots, [](const ImportSlot &a, const ImportSlot &b) {
      return a.offset < b.offset;
    });
    p.slots.erase(llvm::unique(p.slots,
                               [](const ImportSlot &a, const ImportSlot &b) {
                                 return a.offset == b.offset;
                               }),
                  p.slots.end());
    // The loader writes read-only slots only inside the import address table
    // directory: a .rdata chunk moves there, while another read-only section,
    // or a $-group of .rdata, keeps its order and is laid out beside it.
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
    StringRef name = getPartialSectionName(sc, config.mingw);
    StringRef outName = getOutputSectionName(name, config.mingw);
    if (readOnly && outName == ".xdata") {
      Err(ctx) << sc->file << ": " << sc->getSectionName()
               << " is read-only and holds the address of "
               << sc->file->symtab.printSymbol(p.firstRef) << ", imported from "
               << p.slots[0].sym->getDLLName()
               << ", but cannot be laid out with the import address table";
      continue;
    }
    sc->hasImportSlots = true;
    ctx.importSlots[sc] = std::move(p.slots);
    // A chunk with residual words stays writable until the fill seals it.
    if (sealedChunks.contains(sc)) {
      writable.push_back(sc);
      continue;
    }
    if (readOnly && name == ".rdata") {
      slotChunks.push_back(sc);
      continue;
    }
    if (!readOnly && name == ".data" &&
        sc->getOutputCharacteristics() ==
            (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
             IMAGE_SCN_MEM_WRITE)) {
      writableSlotChunks.push_back(sc);
      continue;
    }
    if (readOnly && outName == ".rdata")
      slotRdataGroups = true;
    else if (readOnly)
      slotSections.insert(outName);
    writable.push_back(sc);
  }
  // Exports, roots and KCFI lists use imports otherwise; the fill and a live
  // thunk read them.
  SmallVector<DefinedImportData *, 0> kcfiRead, kcfiOther;
  getKCFIImportUses(ctx, kcfiRead, kcfiOther);
  for (DefinedImportData *imp : kcfiRead)
    scan.use(imp, SlotScan::Read);
  for (DefinedImportData *imp : kcfiOther)
    scan.use(imp, SlotScan::Other);
  for (const COFFLinkerContext::VTableOwnerRun &r : ctx.vtableOwnerRuns)
    for (DefinedImportData *imp : r.words)
      scan.use(imp, SlotScan::Other);
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym))
      scan.use(imp, SlotScan::Other);
  for (Symbol *s : config.gcroot)
    if (auto *imp = dyn_cast<DefinedImportData>(s))
      scan.use(imp, SlotScan::Other);
  for (const ResidualWord &w : residualWords)
    scan.use(w.imp, SlotScan::Read);
  auto getUse = [&](ImportFile *file) {
    SlotScan::Use u = scan.uses.lookup(file);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(file->thunkSym);
    return thunk && thunk->getChunk()->live ? std::max(u, SlotScan::Read) : u;
  };
  // An import only discarded preempted data used needs no entry.
  auto omitPreemptedGroupImports =
      [&](const DenseSet<ImportFile *> &slotted,
          DenseSet<DefinedImportData *> &unneeded) {
        for (ImportFile *file : ctx.symtab.preemptedGroupImports) {
          if (!file->live || getUse(file) != SlotScan::Unused ||
              slotted.contains(file) || interiorBases.contains(file))
            continue;
          file->live = false;
          unneeded.insert(file->impSym);
        }
      };
  if (ctx.importSlots.empty()) {
    DenseSet<DefinedImportData *> unneeded;
    omitPreemptedGroupImports({}, unneeded);
    llvm::erase_if(idata.imports, [&](DefinedImportData *imp) {
      return unneeded.contains(imp);
    });
    return;
  }

  // Chunks laid out together are ordered by their first slot's DLL, so that
  // one DLL's single-pointer chunks form one run.
  DenseMap<SectionChunk *, int> dllIndexOf;
  auto dllIndex = [&](SectionChunk *sc) {
    auto [it, inserted] = dllIndexOf.try_emplace(sc, 0);
    if (inserted) {
      StringRef dll = ctx.importSlots[sc].front().sym->getDLLName();
      it->second = config.dllOrder[dll.lower()];
    }
    return it->second;
  };
  for (std::vector<SectionChunk *> *v : {&writableSlotChunks, &slotChunks}) {
    llvm::stable_sort(*v, [&](SectionChunk *a, SectionChunk *b) {
      return dllIndex(a) < dllIndex(b);
    });
    heldBackChunks.insert(v->begin(), v->end());
  }

  // Slots of one DLL one word apart form a run, also across adjacent chunks
  // laid out together.
  std::vector<ImportSlot *> *run = nullptr;
  ImportSlot *prev = nullptr;
  auto follows = [&](ImportSlot &s) {
    if (!prev ||
        !prev->sym->getDLLName().equals_insensitive(s.sym->getDLLName()))
      return false;
    if (prev->chunk == s.chunk)
      return s.offset == prev->offset + config.wordsize;
    SectionChunk *a = prev->chunk;
    return heldBackChunks.contains(a) && heldBackChunks.contains(s.chunk) &&
           !ctx.chunkPins.contains(s.chunk) && s.offset == 0 &&
           prev->offset + config.wordsize == a->getSize() &&
           s.chunk->getAlignment() <= a->getAlignment() &&
           a->getSize() % s.chunk->getAlignment() == 0;
  };
  auto addRuns = [&](SectionChunk *sc) {
    for (ImportSlot &s : ctx.importSlots[sc]) {
      if (!follows(s)) {
        idata.slotRuns.emplace_back();
        run = &idata.slotRuns.back();
      }
      run->push_back(&s);
      prev = &s;
    }
  };
  for (SectionChunk *sc : writable) {
    prev = nullptr;
    addRuns(sc);
  }
  for (std::vector<SectionChunk *> *v : {&writableSlotChunks, &slotChunks}) {
    prev = nullptr;
    for (SectionChunk *sc : *v)
      addRuns(sc);
  }

  // An import with slots whose other users only read its whole address takes
  // one of its slots as its entry: an aligned one in read-only data that no
  // /merge or /section changes, so nothing writes what they read.
  DenseSet<ImportFile *> slotted;
  for (const std::vector<ImportSlot *> &slotRun : idata.slotRuns)
    for (ImportSlot *slot : slotRun)
      slotted.insert(slot->sym->file);

  // An import whose words all take interior names, and that nothing else
  // uses, needs no entry; debug information names its first word.
  DenseSet<DefinedImportData *> unneeded;
  for (auto &[file, word] : interiorBases) {
    if (getUse(file) != SlotScan::Unused || slotted.contains(file))
      continue;
    file->live = false;
    file->impSym->setLocation(word.first, word.second);
    unneeded.insert(file->impSym);
  }
  omitPreemptedGroupImports(slotted, unneeded);
  llvm::erase_if(idata.imports, [&](DefinedImportData *imp) {
    return unneeded.contains(imp);
  });

  // A read-only chunk with residual words is writable until they are written.
  auto readable = [&](const ImportSlot &s) {
    StringRef outName = getOutputSectionName(
        getPartialSectionName(s.chunk, config.mingw), config.mingw);
    return !(s.chunk->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
           !sealedChunks.contains(s.chunk) &&
           s.chunk->getAlignment() >= config.wordsize &&
           s.offset % config.wordsize == 0 && !config.merge.count(outName) &&
           !config.section.count(outName);
  };
  // Each import's first readable slot, or for an unused import its first.
  MapVector<ImportFile *, ImportSlot *> chosen;
  for (bool any : {false, true})
    for (const std::vector<ImportSlot *> &slotRun : idata.slotRuns)
      for (ImportSlot *slot : slotRun) {
        SlotScan::Use u = getUse(slot->sym->file);
        if (u != SlotScan::Other &&
            (any ? u == SlotScan::Unused : readable(*slot)))
          chosen.try_emplace(slot->sym->file, slot);
      }
  for (auto &[file, slot] : chosen) {
    slot->sym->setLocation(slot->chunk, slot->offset);
    idata.slotOnly.insert(slot->sym);
    if (getUse(file) == SlotScan::Read)
      slotReadImports.push_back(slot->sym);
  }
}

// Adds an import to the import tables after createImportTables.
void ImportSlotContents::addImport(DefinedImportData *imp) {
  imp->file->live = true;
  ctx.config.dllOrder.try_emplace(StringRef(imp->file->dllName).lower(),
                                  ctx.config.dllOrder.size());
  idata.add(imp);
}

// Under -import-slots, the fill writes what the loader cannot: the cells, the
// imports bound at run time and the residual words.
void ImportSlotContents::createFill() {
  llvm::TimeTraceScope timeScope("Create fill");
  Configuration &config = ctx.config;
  auto *list = cast_or_null<FillListChunk>(fillList);
  bool hasXIA = isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a"));
  // A DLL that imports from an executable first runs the executable's copy.
  bool callsCopy =
      config.importSlots && config.dll && hasXIA &&
      llvm::any_of(ctx.importFileInstances, [](ImportFile *f) {
        return f->live && StringRef(f->dllName).ends_with_insensitive(".exe");
      });
  if (!callsCopy && !list && residualWords.empty() && runTimeCells.empty())
    return;
  if (!residualWords.empty() || !runTimeCells.empty()) {
    auto fail = [&](const Twine &why) {
      if (!runTimeCells.empty()) {
        DefinedImportData *imp = runTimeCells.front().imp;
        Err(ctx) << "cannot bind " << ctx.symtab.printSymbol(imp)
                 << ", imported from " << imp->getDLLName()
                 << ", at run time: " << why;
        return;
      }
      const ResidualWord &first = residualWords.front();
      Err(ctx) << first.chunk->file << ": " << first.chunk->getSectionName()
               << " holds the address of "
               << first.chunk->file->symtab.printSymbol(first.sym) << " plus "
               << first.addend << ", imported from " << first.imp->getDLLName()
               << ", which exports no name for it, and " << why;
    };
    if (!hasXIA)
      return fail("the image has no C initializer table (__xi_a) to write it");
    if ((!sealedChunks.empty() || !runTimeCells.empty()) && config.align % 4096)
      return fail("its section alignment is smaller than a page, which the "
                  "image cannot make read-only once written");
  }
  auto *f = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find("__llvm_fill"));
  if (!f) {
    Err(ctx) << "the image's fill needs __llvm_fill from the compiler-rt "
                "builtins";
    return;
  }
  if (!list) {
    list = make<FillListChunk>(ctx, std::vector<OverrideCell>());
    fillList = list;
  }
  list->runTimeCells = runTimeCells;
  list->words = residualWords;
  list->callsCopy = callsCopy;
  list->sealed = !runTimeCells.empty()   ? runTimeCells.front().cell
                 : !sealedChunks.empty() ? sealedChunks.front()
                                         : nullptr;
  list->selfKey =
      overrideNameKey(sys::path::filename(config.outputFile).lower());

  for (Chunk *c : cast<ObjFile>(f->getFile())->getChunks())
    if (auto *sc = dyn_cast<SectionChunk>(c))
      for (Symbol *sym : sc->symbols())
        if (auto *imp = dyn_cast_or_null<DefinedImportData>(sym);
            imp && !imp->file->live)
          addImport(imp);
  fill = f;
  fillPointer = make<LocalImportChunk>(ctx, f);
  markLive(ctx, {f});
  Symbol *s = ctx.symtab.find("__llvm_fill_list");
  replaceSymbol<DefinedSynthetic>(s, s->getName(), list);
}

// The targets of the relocations of the TLS directory's first and third
// fields.
static TLSTemplate findTLSTemplate(COFFLinkerContext &ctx) {
  TLSTemplate tls;
  auto *dir =
      dyn_cast_or_null<DefinedRegular>(ctx.symtab.findUnderscore("_tls_used"));
  if (!dir)
    return tls;
  SectionChunk *sc = dir->getChunk();
  for (const coff_relocation &rel : sc->getRelocs()) {
    if (!sc->isAddressWord(rel))
      continue;
    uint64_t field = rel.VirtualAddress - dir->getValue();
    auto *target =
        dyn_cast_or_null<Defined>(sc->file->getSymbol(rel.SymbolTableIndex));
    int64_t addend = getAddressWordAddend(sc, rel).value_or(0);
    if (field == 0) {
      tls.start = target;
      tls.startAddend = addend;
    } else if (field == 2 * ctx.config.wordsize) {
      tls.index = target;
      tls.indexAddend = addend;
    }
  }
  return tls;
}

// The loader copies an executable's thread-local template into the main
// thread's block before it binds the template's slots; the copy rebinds them.
void ImportSlotContents::createThreadLocalSlotCopy() {
  llvm::TimeTraceScope timeScope("Create thread-local slot copy");
  Configuration &config = ctx.config;
  if (!config.importSlots || config.dll)
    return;
  std::vector<const ImportSlot *> tlsSlots;
  for (auto &[sc, slots] : ctx.importSlots)
    if (getPartialSectionName(const_cast<SectionChunk *>(sc), config.mingw)
            .starts_with(".tls"))
      for (const ImportSlot &slot : slots)
        tlsSlots.push_back(&slot);
  // Without a TLS directory the loader gives no thread a block.
  if (tlsSlots.empty() || !ctx.symtab.findUnderscore("_tls_used"))
    return;
  auto fail = [&](const Twine &why) {
    Err(ctx) << tlsSlots.front()->chunk->file
             << ": the address of an import in thread-local data needs a copy "
                "into the main thread's block, but "
             << why;
  };
  TLSTemplate tls = findTLSTemplate(ctx);
  if (!tls.start || !tls.index)
    return fail("the image's TLS directory (_tls_used) names no template or "
                "index");
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xl_a")))
    return fail("the image has no TLS callback array (__xl_a) to run it");
  auto *copy = dyn_cast_or_null<DefinedRegular>(
      ctx.symtab.find("__llvm_thread_local_slot_copy"));
  if (!copy)
    return fail("the compiler-rt builtins, which define the copy "
                "(__llvm_thread_local_slot_copy), are not linked");
  threadLocalSlotCopy = copy;
  threadLocalSlotCopyPointer = make<LocalImportChunk>(ctx, copy);
  threadLocalSlotCopyList = make<CopyListChunk>(std::move(tlsSlots), tls);
  markLive(ctx, {copy});
  Symbol *s = ctx.symtab.find("__llvm_thread_local_slot_copy_list");
  replaceSymbol<DefinedSynthetic>(s, s->getName(), threadLocalSlotCopyList);
}

// A DLL's cells: the slots and entries of the overridable defaults it imports,
// and the words through which it reaches its own defaults and the vague
// entities another image's instance may preempt, as an ELF object reaches
// them through its GOT. An executable binds its own from its table.
void ImportSlotContents::collectCells() {
  llvm::TimeTraceScope timeScope("Collect cells");
  Configuration &config = ctx.config;
  if (!config.importSlots || !config.dll)
    return;
  std::vector<OverrideCell> cells;
  ctx.symtab.readOverrideRecords();
  StringMap<StringSet<>> preemptibleOffers;
  if (isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    preemptibleOffers = getPreemptibleOffers(ctx);
  auto add = [&](SectionChunk *sc, uint32_t offset, DefinedImportData *imp,
                 bool readOnly, bool threadLocal) {
    StringRef name = imp->getName().drop_front(strlen("__imp_"));
    if (std::optional<uint32_t> k = ctx.symtab.overrideImportSlot(name)) {
      cells.push_back({sc, offset, imp, *k, 0, readOnly, threadLocal});
    } else if (ctx.symtab.defaultsByName.contains(name)) {
      cells.push_back(
          {sc, offset, imp, 0, overrideNameKey(name), readOnly, threadLocal});
    } else if (!threadLocal) {
      auto it = preemptibleOffers.find(imp->getDLLName().lower());
      if (it != preemptibleOffers.end() && it->second.contains(name))
        cells.push_back({sc, offset, imp, 0, overrideNameKey(name), readOnly,
                         false, /*indirect=*/true, name});
    }
  };
  // A chunk with residual words is writable until the fill seals it.
  for (auto &[sc, slots] : ctx.importSlots) {
    auto *chunk = const_cast<SectionChunk *>(sc);
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
                    !sealedChunks.contains(chunk);
    bool threadLocal =
        getPartialSectionName(chunk, config.mingw).starts_with(".tls");
    for (const ImportSlot &slot : slots)
      add(chunk, slot.offset, slot.sym, readOnly, threadLocal);
  }
  // An import whose entry is one of its slots is covered by that slot.
  for (DefinedImportData *imp : idata.imports)
    if (!idata.slotOnly.contains(imp))
      add(nullptr, 0, imp, true, false);
  auto addOwn = [&](StringRef name, Chunk *c, uint32_t offset, bool readOnly,
                    bool threadLocal) {
    if (ctx.symtab.isOwnSlotDefault(name))
      cells.push_back({c, offset, nullptr, *ctx.symtab.overrideImportSlot(name),
                       0, readOnly, threadLocal});
    else if (ctx.symtab.overridablesByName.contains(name))
      cells.push_back({c, offset, nullptr, 0, overrideNameKey(name), readOnly,
                       threadLocal});
  };
  auto addVague = [&](Defined *d, Chunk *c, uint32_t offset, bool readOnly) {
    cells.push_back({c, offset, nullptr, 0, overrideNameKey(d->getName()),
                     readOnly, false, /*indirect=*/true, d->getName()});
  };
  // Each preemptible entity gets a pointer, which the offer table names; a
  // thread-local variable's instance is its record.
  ctx.symtab.isPreemptibleVagueEntity(nullptr); // which computes the set
  if (!ctx.symtab.preemptibleVagueEntities->empty()) {
    DenseSet<Defined *> pointed;
    for (Chunk *c : ctx.symtab.localImportChunks)
      if (auto *li = cast<LocalImportChunk>(c); !li->vagueEntity.empty())
        pointed.insert(li->getTarget());
    std::vector<Defined *> entities;
    for (Symbol *s : *ctx.symtab.preemptibleVagueEntities) {
      auto *d = dyn_cast<DefinedRegular>(s);
      if (!d || !d->isLive())
        continue;
      Defined *instance = d;
      if (ctx.symtab.isThreadLocal(d)) {
        instance = ctx.symtab.getThreadLocalRecord(d);
        if (!instance)
          continue;
        ctx.symtab.keepThreadLocalRecord(instance);
      }
      if (!pointed.contains(instance))
        entities.push_back(instance);
    }
    llvm::sort(entities, [](Defined *a, Defined *b) {
      return a->getName() < b->getName();
    });
    for (Defined *d : entities) {
      auto *li = make<LocalImportChunk>(ctx, d);
      li->vagueEntity = d->getName();
      ctx.symtab.localImportChunks.push_back(li);
    }
  }
  for (Chunk *c : ctx.symtab.localImportChunks) {
    auto *li = cast<LocalImportChunk>(c);
    if (!li->overrideDefault.empty())
      addOwn(li->overrideDefault, li, 0, true, false);
    else if (!li->vagueEntity.empty())
      addVague(li->getTarget(), li, 0, true);
  }
  // Static data holds cells only of own defaults and preemptible entities.
  bool scanStaticData = !ctx.symtab.overrideSlotFacts.empty() ||
                        !ctx.symtab.overridablesByName.empty() ||
                        !ctx.symtab.preemptibleVagueEntities->empty();
  for (ObjFile *file : ctx.objFileInstances) {
    if (!scanStaticData)
      break;
    if (&file->symtab != &ctx.symtab)
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live ||
          (sc->header->Characteristics &
           (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_DISCARDABLE)) ||
          isExceptionData(sc))
        continue;
      bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
                      !sealedChunks.contains(sc);
      bool threadLocal =
          getPartialSectionName(sc, config.mingw).starts_with(".tls");
      for (const coff_relocation &rel : sc->getRelocs()) {
        auto *d =
            dyn_cast_or_null<Defined>(file->getSymbol(rel.SymbolTableIndex));
        if (!d || !sc->isAddressWord(rel) || getAddressWordAddend(sc, rel) != 0)
          continue;
        if (!threadLocal && ctx.symtab.isPreemptibleVagueEntity(d))
          addVague(d, sc, rel.VirtualAddress, readOnly);
        else
          addOwn(d->getName(), sc, rel.VirtualAddress, readOnly, threadLocal);
      }
    }
  }
  if (cells.empty())
    return;

  const OverrideCell &first = cells.front();
  auto fail = [&](const Twine &why) {
    if (first.imp)
      Err(ctx) << "the image reaches " << ctx.symtab.printSymbol(first.imp)
               << ", an overridable default imported from "
               << first.imp->getDLLName() << ", through a cell " << why;
    else
      Err(ctx) << "the image reaches its own overridable default through a "
                  "cell "
               << why;
  };
  // The anchor's mask covers 32 slots.
  for (const OverrideCell &c : cells)
    if (!c.key && c.slot >= 32)
      return fail("that the override table's mask cannot name");
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    return fail("that the image has no C initializer table (__xi_a) to fill");
  TLSTemplate tls;
  if (llvm::any_of(cells,
                   [](const OverrideCell &c) { return c.threadLocal; })) {
    tls = findTLSTemplate(ctx);
    if (!tls.start || !tls.index)
      return fail("in thread-local data, but the image's TLS directory "
                  "(_tls_used) names no template or index");
  }
  auto *list = make<FillListChunk>(ctx, std::move(cells));
  list->tls = tls;
  fillList = list;
}

// A DLL loaded at run time is outside the executable's table; as an object
// opened RTLD_LOCAL resolves against the global scope, it binds its cells
// through the offer tables of the modules in that scope.
void ImportSlotContents::createOfferTable() {
  llvm::TimeTraceScope timeScope("Create offer table");
  Configuration &config = ctx.config;
  if (!config.importSlots)
    return;
  DenseMap<StringRef, const Export *> exports;
  for (const Export &e : ctx.symtab.exports)
    if (!e.noname && e.forwardTo.empty())
      exports.try_emplace(e.exportName, &e);
  std::vector<VagueOfferTableChunk::Offer> offers;
  DenseMap<uint64_t, StringRef> keys;
  auto offer = [&](StringRef name, Chunk *cell, Defined *instance) {
    const Export *e = exports.lookup(name);
    if (!e)
      return;
    uint64_t key = overrideNameKey(name);
    auto [it, inserted] = keys.try_emplace(key, name);
    if (inserted)
      offers.push_back({key, cell, instance, e});
    else if (it->second != name)
      Err(ctx) << "vague entities " << it->second << " and " << name
               << " have the same key in the offer table";
  };
  // The executable offers the vague entities it exports, but no type_info,
  // which such images compare by name.
  if (!config.dll) {
    DenseSet<Symbol *> vague;
    vague.insert_range(ctx.symtab.weakData);
    vague.insert_range(ctx.symtab.weakFunctions);
    for (const Export &e : ctx.symtab.exports) {
      // A thread-local variable's instance is its record.
      auto *d = dyn_cast_or_null<DefinedRegular>(e.sym);
      if (DefinedRegular *var = ctx.symtab.getRecordVariable(e.sym))
        d = var;
      if (d && d->isLive() && vague.contains(d) &&
          isShareableVagueName(d->getName()) && !isTypeInfoName(d->getName()))
        offer(e.exportName, nullptr, cast<Defined>(e.sym));
    }
    if (!offers.empty())
      vagueOfferTable = make<VagueOfferTableChunk>(
          std::move(offers), std::vector<uint64_t>(), /*direct=*/true);
    return;
  }
  // A DLL offers the pointers of its exported preemptible entities, and lists
  // the DLLs it delay-loads.
  for (Chunk *c : ctx.symtab.localImportChunks)
    if (auto *li = cast<LocalImportChunk>(c); !li->vagueEntity.empty())
      offer(li->vagueEntity, li, nullptr);
  std::vector<uint64_t> delayLoads;
  for (StringRef dll : config.delayLoads.keys())
    delayLoads.push_back(overrideNameKey(dll));
  llvm::sort(delayLoads);
  if (!offers.empty() || !delayLoads.empty())
    vagueOfferTable = make<VagueOfferTableChunk>(
        std::move(offers), std::move(delayLoads), /*direct=*/false);

  if (auto *list = cast_or_null<FillListChunk>(fillList)) {
    list->exports = std::move(exports);
    list->hintOf = &idata.hintOf;
  }
}

// The loader makes only the import address table directory writable while it
// binds, and restores one protection over it: a slot must be outside code, and
// a read-only one inside the directory, which only read-only data may share.
void ImportSlotContents::check(Chunk *iatStart, uint64_t iatSize) {
  llvm::TimeTraceScope timeScope("Check import slots");
  // Pages the fill makes writable must hold only read-only data.
  auto checkCells = [&](ArrayRef<std::pair<uint64_t, uint64_t>> ranges) {
    for (auto [start, end] : ranges) {
      uint64_t pageBegin = alignDown(start, ctx.config.align);
      uint64_t pageLimit = alignTo(end, ctx.config.align);
      for (OutputSection *os : ctx.outputSections) {
        if (os->getRVA() >= pageLimit ||
            os->getRVA() + os->getVirtualSize() <= pageBegin)
          continue;
        if ((os->header.Characteristics &
             (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE |
              IMAGE_SCN_MEM_EXECUTE)) != IMAGE_SCN_MEM_READ)
          Err(ctx) << "section " << os->name
                   << " shares a page with read-only cells, which the image's "
                      "fill makes writable, but is not read-only data";
      }
    }
  };
  if (auto *list = cast_or_null<FillListChunk>(fillList)) {
    SmallVector<std::pair<uint64_t, uint64_t>, 0> ranges =
        list->getReadOnlyRanges();
    if (ranges.size() > list->maxRanges)
      Err(ctx) << "read-only cells lie in more output sections than their "
                  "input sections name";
    checkCells(ranges);
  }

  if (ctx.importSlots.empty())
    return;
  const uint32_t perms =
      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;
  uint64_t iatBegin = iatStart ? iatStart->getRVA() : 0;
  uint64_t iatLimit = iatStart ? iatBegin + iatSize : 0;
  for (auto &[sc, slots] : ctx.importSlots) {
    OutputSection *os = ctx.getOutputSection(sc);
    uint32_t chars = os->header.Characteristics;
    for (const ImportSlot &s : slots) {
      uint64_t rva = sc->getRVA() + s.offset;
      if (chars & IMAGE_SCN_MEM_EXECUTE)
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(s.offset) << " in " << sc->getSectionName()
                 << " is in executable section " << os->name;
      else if (!(chars & IMAGE_SCN_MEM_WRITE) &&
               (rva < iatBegin || rva + ctx.config.wordsize > iatLimit))
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(s.offset) << " in " << sc->getSectionName()
                 << " is in read-only section " << os->name
                 << " outside the import address table";
    }
  }
  uint64_t pageBegin = alignDown(iatBegin, ctx.config.align);
  uint64_t pageLimit = alignTo(iatLimit, ctx.config.align);
  for (OutputSection *os : ctx.outputSections) {
    if (os->getRVA() >= pageLimit ||
        os->getRVA() + os->getVirtualSize() <= pageBegin)
      continue;
    if ((os->header.Characteristics & perms) != IMAGE_SCN_MEM_READ)
      Err(ctx) << "section " << os->name
               << " shares a page with the import address table, which holds "
                  "in-place import slots, but is not read-only data";
  }
  for (const std::vector<ImportSlot *> &run : idata.slotRuns)
    for (size_t i = 1; i < run.size(); ++i)
      if (run[i]->chunk->getRVA() + run[i]->offset !=
          run[i - 1]->chunk->getRVA() + run[i - 1]->offset +
              ctx.config.wordsize)
        Err(ctx) << run[i]->chunk->file
                 << ": a run of in-place import slots in "
                 << run[i]->chunk->getSectionName()
                 << " is not contiguous in the image";
  for (DefinedImportData *imp : slotReadImports) {
    OutputSection *os = ctx.getOutputSection(imp->getChunk());
    if (imp->getRVA() % ctx.config.wordsize ||
        (os->header.Characteristics & IMAGE_SCN_MEM_WRITE))
      Err(ctx) << "code reads the address of " << ctx.symtab.printSymbol(imp)
               << " from an in-place import slot in " << os->name
               << " that is misaligned or writable";
  }
}

// Local imports: a reference to the import pointer of a symbol the image
// defines reads a pointer the linker makes. Where the linker knows the
// instruction holding the reference, it rewrites it to reach the symbol
// directly, as ELF relaxes a GOT load, and leaves out a pointer only rewritten
// instructions read.

bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off, LinkSiteForm form) {
  // A displacement that names the pointer itself has no addend.
  if (off < 2 || off + 4 > data.size() || read32le(&data[off]) != 0)
    return false;
  uint8_t opcode = data[off - 2], modrm = data[off - 1];
  uint8_t prefix = off >= 3 ? data[off - 3] : 0;
  switch (form) {
  case LinkSiteCall:
    return opcode == 0xFF && modrm == 0x15;
  case LinkSiteJump:
    return opcode == 0xFF && modrm == 0x25;
  case LinkSiteJumpOnePrefix:
    // A REX prefix, or a legacy prefix.
    return off >= 3 && opcode == 0xFF && modrm == 0x25 &&
           ((prefix & 0xF0) == 0x40 ||
            is_contained(
                {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF2, 0xF3},
                prefix));
  case LinkSiteLoad:
  case LinkSiteLoadREX2:
  case LinkSiteAddress: {
    // mov or lea r64, [rip+d], with REX.W or with REX2's W bit.
    bool rex = (prefix & 0xF8) == 0x48;
    bool rex2 = off >= 4 && data[off - 4] == 0xD5 && (prefix & 0x88) == 0x08;
    return off >= 3 && opcode == (form == LinkSiteAddress ? 0x8D : 0x8B) &&
           (modrm & 0xC7) == 0x05 &&
           (form == LinkSiteLoad       ? rex
            : form == LinkSiteLoadREX2 ? rex2
                                       : rex || rex2);
  }
  default:
    return false;
  }
}

// The form sc's object describes for rel, a REL32 field.
static std::optional<LinkSiteForm> getSiteForm(const SectionChunk *sc,
                                               const coff_relocation &rel) {
  if (!sc->file->describesSites() || rel.Type != IMAGE_REL_AMD64_REL32)
    return std::nullopt;
  const ObjFile::RelocFact *f = sc->file->relocFact(sc, rel);
  if (!f || !f->hasSite)
    return std::nullopt;
  return f->form;
}

std::optional<LinkSiteForm> getDescribedSite(const SectionChunk *sc,
                                             const coff_relocation &rel) {
  std::optional<LinkSiteForm> form = getSiteForm(sc, rel);
  if (form && isSiteForm(sc->getContents(), rel.VirtualAddress, *form))
    return form;
  return std::nullopt;
}

static bool isLoadForm(std::optional<LinkSiteForm> form) {
  return form == LinkSiteLoad || form == LinkSiteLoadREX2;
}

// A pointer holding zero, an absent weak symbol's address, which an
// instruction can materialise instead of loading.
static bool isZeroPointer(DefinedLocalImport *li) {
  auto *abs = dyn_cast<DefinedAbsolute>(li->getTarget());
  return abs && abs->getVA() == 0;
}

// Whether a reference through s can reach its symbol directly: a local import
// pointer to something with an image-relative address, or to zero.
static bool canBypass(Symbol *s) {
  auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
  return li && !li->getChunk()->isCell() &&
         (!isa<DefinedAbsolute>(li->getTarget()) || isZeroPointer(li));
}

bool isArm64AddressPair(const SectionChunk *sc, const coff_relocation &adrp,
                        const coff_relocation &add) {
  ArrayRef<uint8_t> data = sc->getContents();
  if (adrp.Type != IMAGE_REL_ARM64_PAGEBASE_REL21 ||
      add.Type != IMAGE_REL_ARM64_PAGEOFFSET_12A ||
      adrp.SymbolTableIndex != add.SymbolTableIndex ||
      uint64_t(adrp.VirtualAddress) + 4 != add.VirtualAddress ||
      uint64_t(add.VirtualAddress) + 4 > data.size())
    return false;
  // adrp xd with no addend, then add xd, xd, #0.
  Adrp first;
  Add second;
  return parseAdrp(read32le(&data[adrp.VirtualAddress]), first) &&
         first.addend == 0 &&
         parseAdd(read32le(&data[add.VirtualAddress]), second) &&
         second.addend == 0 && second.destRegister == first.destRegister &&
         second.srcRegister == first.destRegister;
}

// adrp with no addend, or a 64-bit integer ldr with no offset.
bool isArm64PointerLoad(const SectionChunk *sc, const coff_relocation &rel) {
  ArrayRef<uint8_t> data = sc->getContents();
  if (uint64_t(rel.VirtualAddress) + 4 > data.size())
    return false;
  uint32_t insn = read32le(&data[rel.VirtualAddress]);
  Adrp adrp;
  Ldr ldr;
  if (rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21)
    return parseAdrp(insn, adrp) && adrp.addend == 0;
  return rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L && parseLdr(insn, ldr) &&
         ldr.p2Size == 3 && !ldr.isFloat && ldr.extendType == ZeroExtend &&
         ldr.offset == 0;
}

// The relocation of sc of type at off, if any.
static const coff_relocation *findReloc(const SectionChunk *sc, uint32_t off,
                                        uint16_t type) {
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  auto it = llvm::partition_point(relocs, [&](const coff_relocation &r) {
    return r.VirtualAddress < off;
  });
  return it != relocs.end() && it->VirtualAddress == off && it->Type == type
             ? &*it
             : nullptr;
}

bool isValueOnlyLoad(const SectionChunk *sc, size_t i, LinkSiteForm form,
                     uint16_t distance, size_t &page) {
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  const coff_relocation &rel = relocs[i];
  if (sc->getArch() != Triple::aarch64) {
    page = i;
    return rel.Type == IMAGE_REL_AMD64_REL32 && isLoadForm(form) &&
           isSiteForm(sc->getContents(), rel.VirtualAddress, form);
  }
  // The adrp before the ldr fills the ldr's base and names the same symbol.
  uint64_t dist = uint64_t(distance) * 4;
  if (rel.Type != IMAGE_REL_ARM64_PAGEOFFSET_12L || !distance ||
      dist > rel.VirtualAddress || !isArm64PointerLoad(sc, rel))
    return false;
  const coff_relocation *adrp = findReloc(sc, rel.VirtualAddress - dist,
                                          IMAGE_REL_ARM64_PAGEBASE_REL21);
  if (!adrp || adrp->SymbolTableIndex != rel.SymbolTableIndex ||
      !isArm64PointerLoad(sc, *adrp))
    return false;
  page = adrp - relocs.begin();
  ArrayRef<uint8_t> data = sc->getContents();
  Adrp a;
  Ldr ldr;
  return parseAdrp(read32le(&data[adrp->VirtualAddress]), a) &&
         parseLdr(read32le(&data[rel.VirtualAddress]), ldr) &&
         a.destRegister == ldr.baseRegister;
}

// Rewrites the described instruction of form at rel to reach li's symbol
// directly: a load becomes lea, a call addr32 call, a jump jmp and int3, or
// each materialises zero for a zero pointer. Resized code drops the padding.
static void addSiteEdit(SectionChunk *sc, const coff_relocation &rel,
                        LinkSiteForm form, DefinedLocalImport *li) {
  uint32_t off = rel.VirtualAddress;
  Defined *target = li->getTarget();
  bool zero = isZeroPointer(li);
  if (form == LinkSiteCall)
    addEdit(sc, {off - 2, 6, uint16_t(zero ? 6 : 5),
                 zero ? RelaxEdit::ZeroCall : RelaxEdit::DirectCall, target});
  else if (form == LinkSiteJump || form == LinkSiteJumpOnePrefix)
    addEdit(sc, {off + 4 - (form == LinkSiteJump ? 6 : 7),
                 uint16_t(form == LinkSiteJump ? 6 : 7), 5,
                 RelaxEdit::DirectJump, target});
  else {
    uint16_t size = form == LinkSiteLoadREX2 ? 8 : 7;
    addEdit(sc, {off + 4 - size, size, size,
                 zero ? RelaxEdit::ZeroLoad : RelaxEdit::LoadAddress, target});
  }
}

// The register a described load fills: ModRM.reg with REX.R, or with REX2's
// R3 and R4.
static unsigned getLoadRegister(ArrayRef<uint8_t> data, uint32_t off,
                                LinkSiteForm form) {
  uint8_t prefix = data[off - 3];
  unsigned reg = (data[off - 1] >> 3 & 7) | (prefix & 0x04) << 1;
  if (form == LinkSiteLoadREX2)
    reg |= (prefix & 0x40) >> 2;
  return reg;
}

// The length of a fused add: add or sub of an 8- or 32-bit immediate, or lea
// of a base plus an 8- or 32-bit displacement, with a SIB byte or not.
static unsigned getAddLength(ArrayRef<uint8_t> data, uint32_t off) {
  uint8_t op = data[off + 1], modrm = data[off + 2];
  if (op == 0x83 || op == 0x81)
    return op == 0x83 ? 4 : 7;
  return 3 + ((modrm & 7) == 4) + (modrm >> 6 == 1 ? 1 : 4);
}

// The scaled immediate offset of an ARM64 load, store or prefetch at an
// unsigned 12-bit offset, or of a pair at a signed 7-bit one.
struct Arm64Access {
  unsigned scale = 0;
  bool pair = false;

  explicit Arm64Access(uint32_t insn) {
    bool simd = insn & 0x04000000;
    unsigned opc = insn >> 30;
    if ((insn & 0x3B000000) == 0x39000000) {
      // A SIMD and floating-point access with opc bit 1 set is 128-bit.
      scale = simd && (insn & 0x00800000) ? 16 : 1u << opc;
    } else if ((insn & 0x3B800000) == 0x29000000) {
      pair = true;
      scale = simd ? 4u << opc : (opc == 2 ? 8 : 4);
    }
  }
  int64_t getOffset(uint32_t insn) const {
    return pair ? SignExtend64<7>(insn >> 15) * scale
                : int64_t(insn >> 10 & 0xFFF) * scale;
  }
  bool fits(int64_t offset) const {
    return offset % scale == 0 &&
           (pair ? isInt<7>(offset / scale) : isUInt<12>(offset / scale));
  }
  uint32_t setOffset(uint32_t insn, int64_t offset) const {
    if (pair)
      return (insn & ~0x003F8000) | uint32_t(offset / scale & 0x7F) << 15;
    return (insn & ~0x003FFC00) | uint32_t(offset / scale) << 10;
  }
};

// Binds the fused uses of sc's pointer loads once the pointers are bound. A
// call goes direct into the image, through the pointer to a defined import
// entry, or stays through its register, keeping its load; an access or add
// reaches data in the image directly where the code may change, or the access
// reads the image's copy of immutable data; a load nothing keeps is dead. On
// ARM64 writeFusedArm64 writes the direct forms once addresses are known.
static void bindFusedUses(SectionChunk *sc,
                          MutableArrayRef<ObjFile::LinkFusion> fusions) {
  ObjFile *file = sc->file;
  SymbolTable &symtab = file->symtab;
  bool arm64 = symtab.machine == ARM64;
  uint16_t loadType =
      arm64 ? IMAGE_REL_ARM64_PAGEOFFSET_12L : IMAGE_REL_AMD64_REL32;
  ArrayRef<uint8_t> data = sc->getContents();
  bool covered = file->isCovered(sc);
  auto it = sc->file->symtab.ctx.relaxations.find(sc);
  Relaxation *edits =
      it == sc->file->symtab.ctx.relaxations.end() ? nullptr : &it->second;
  DenseSet<uint32_t> keptLoads;
  for (ObjFile::LinkFusion &f : fusions) {
    const coff_relocation *rel;
    if (f.isLoad || (arm64 && f.use == ObjFile::LinkFusion::Call) ||
        !(rel = findReloc(sc, f.load, loadType)))
      continue;
    Symbol *s = file->getSymbol(rel->SymbolTableIndex);
    auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
    if (!s || (arm64 ? !isArm64PointerLoad(sc, *rel)
                     : !isLoadForm(getDescribedSite(sc, *rel)))) {
      Err(symtab.ctx) << file << ": the instruction at offset 0x"
                      << Twine::utohexstr(f.load) << " in "
                      << sc->getSectionName()
                      << " is not the one its link-only record describes";
      continue;
    }
    bool zero = li && isZeroPointer(li);
    bool reaches = li && !zero &&
                   (arm64 ? li->getChunk()->bypassed : canBypass(li));
    bool immutable = f.use == ObjFile::LinkFusion::Access && f.valueRead &&
                     symtab.immutableReads.contains(s);
    if (arm64) {
      if (!reaches)
        keptLoads.insert(f.load);
      if (reaches || immutable)
        f.form = ObjFile::LinkFusion::Direct;
      continue;
    }
    switch (f.use) {
    case ObjFile::LinkFusion::Access:
      if (!covered || !reaches)
        keptLoads.insert(f.load);
      if (covered && (reaches || immutable))
        addEdit(sc, {f.offset, f.length,
                     uint16_t(f.length - (f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0) -
                              f.sib + 4),
                     RelaxEdit::Access,
                     li ? li->getTarget() : symtab.immutableReads.lookup(s),
                     &f});
      break;
    case ObjFile::LinkFusion::Add:
      // The load takes the sum of the add, which then goes.
      if (RelaxEdit *load = edits ? edits->findEdit(f.load) : nullptr;
          reaches && load && load->kind == RelaxEdit::LoadAddress) {
        load->entry = &f;
        addEdit(sc, {f.offset, uint16_t(getAddLength(data, f.offset)), 0,
                     RelaxEdit::Dispose, nullptr, &f});
      }
      break;
    case ObjFile::LinkFusion::Call: {
      // A call through a delay-loaded import's pointer stays through its
      // register: the pointer holds the loader's stub until the first call.
      auto *imp = dyn_cast_or_null<DefinedImportData>(s);
      uint16_t size = f.isJump ? 7 : 6;
      if (reaches)
        addEdit(sc, {f.offset, size, 5,
                     f.isJump ? RelaxEdit::DirectJump : RelaxEdit::DirectCall,
                     li->getTarget()});
      else if (imp && imp->file->isDelayLoaded())
        addEdit(sc, {f.offset, size, size,
                     f.isJump ? RelaxEdit::PointerJump : RelaxEdit::PointerCall,
                     cast<Defined>(s)});
      else {
        // Resized, a jump keeps REX.W jmp through the register and drops the
        // int3; a call drops the segment overrides before the register call.
        keptLoads.insert(f.load);
        if (covered)
          addEdit(sc, f.isJump ? RelaxEdit(f.offset, 7, 3, RelaxEdit::KeepHead)
                               : RelaxEdit(f.offset, 6,
                                           f.reg >= 16  ? 4
                                           : f.reg >= 8 ? 3
                                                        : 2,
                                           RelaxEdit::KeepTail));
      }
      break;
    }
    }
  }
  for (ObjFile::LinkFusion &f : fusions) {
    if (!f.isLoad || keptLoads.contains(f.offset) ||
        !findReloc(sc, f.offset, loadType))
      continue;
    if (arm64) {
      f.dead = true;
      continue;
    }
    uint16_t size = getDescribedSite(sc, *findReloc(sc, f.offset, loadType)) ==
                            LinkSiteLoadREX2
                        ? 8
                        : 7;
    if (RelaxEdit *e = edits ? edits->findEdit(f.offset) : nullptr) {
      e->kind = RelaxEdit::Dispose;
      e->relaxedSize = 0;
      e->entry = &f;
    } else {
      addEdit(sc, {f.offset + 4 - size, size, 0, RelaxEdit::Dispose, nullptr,
                   &f});
    }
  }
  if (arm64)
    sc->edited |= llvm::any_of(fusions, [](const ObjFile::LinkFusion &f) {
      return f.dead || f.form == ObjFile::LinkFusion::Direct;
    });
}

void bindFusedCalls(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  if ((symtab.machine != AMD64 && symtab.machine != ARM64) || ctx.hybridSymtab)
    return;
  llvm::TimeTraceScope timeScope("Bind fused calls");
  for (ObjFile *file : ctx.objFileInstances)
    if (&file->symtab == &symtab && file->hasLinkFusions())
      for (Chunk *c : file->getChunks())
        if (auto *sc = dyn_cast_or_null<SectionChunk>(c); sc && sc->live)
          if (MutableArrayRef<ObjFile::LinkFusion> f = file->getLinkFusions(sc);
              !f.empty())
            bindFusedUses(sc, f);
}

void writeFusedArm64(const SectionChunk *sc, uint8_t *buf) {
  const uint32_t nop = 0xD503201F;
  MutableArrayRef<ObjFile::LinkFusion> fusions = sc->file->getLinkFusions(sc);
  auto getSymbol = [&](uint32_t load) {
    return sc->file->getSymbol(
        findReloc(sc, load, IMAGE_REL_ARM64_PAGEOFFSET_12L)->SymbolTableIndex);
  };
  auto getData = [&](uint32_t load) {
    return cast<DefinedLocalImport>(getSymbol(load))->getTarget();
  };
  DenseMap<uint32_t, SmallVector<const ObjFile::LinkFusion *, 2>> accessesOf;
  for (const ObjFile::LinkFusion &f : fusions) {
    if (f.isLoad)
      continue;
    if (f.use == ObjFile::LinkFusion::Access)
      accessesOf[f.load].push_back(&f);
    if (f.form != ObjFile::LinkFusion::Direct)
      continue;
    if (f.use == ObjFile::LinkFusion::Add) {
      // Where the data's page offset plus the constant fits, the load (which
      // the pointer's bypass made an add of the offset) adds the sum into the
      // add's register, and the add becomes a no-op.
      int64_t sum = (getData(f.load)->getRVA() & 0xFFF) + f.addend;
      if (sum >= 0 && sum <= 0xFFF) {
        uint32_t load = read32le(buf + f.load);
        write32le(buf + f.load,
                  0x91000000 | uint32_t(sum) << 10 | (load & 0x3E0) | f.reg);
        write32le(buf + f.offset, nop);
      }
    }
  }
  // An access that only reads immutable data, whose load stays, reads the
  // image's copy PC-relative where it is near enough and aligned.
  for (const ObjFile::LinkFusion &f : fusions) {
    if (f.isLoad || f.use != ObjFile::LinkFusion::Access || !f.valueRead ||
        f.form != ObjFile::LinkFusion::Direct)
      continue;
    Defined *copy = sc->file->symtab.immutableReads.lookup(getSymbol(f.load));
    Ldr ldr;
    if (!copy || !parseLdr(read32le(buf + f.offset), ldr))
      continue;
    ldr.offset += int64_t(copy->getRVA()) - int64_t(sc->getRVA() + f.offset);
    if (isLiteralLdrEligible(ldr))
      writeLiteralLdr(buf + f.offset, ldr);
  }
  // A dead load whose accesses all take the data's page offset in their
  // fields becomes a no-op, its register keeping the adrp's page.
  for (const ObjFile::LinkFusion &l : fusions) {
    auto it = accessesOf.find(l.offset);
    if (!l.isLoad || !l.dead || it == accessesOf.end())
      continue;
    uint64_t lo12 = getData(l.offset)->getRVA() & 0xFFF;
    if (!llvm::all_of(it->second, [&](const ObjFile::LinkFusion *f) {
          uint32_t insn = read32le(buf + f->offset);
          Arm64Access access(insn);
          return access.fits(lo12 + access.getOffset(insn));
        }))
      continue;
    for (const ObjFile::LinkFusion *f : it->second) {
      uint32_t insn = read32le(buf + f->offset);
      Arm64Access access(insn);
      write32le(buf + f->offset,
                access.setOffset(insn, lo12 + access.getOffset(insn)));
    }
    write32le(buf + l.offset, nop);
  }
}

// Our compiler reaches every symbol it cannot place in the image through its
// import pointer, which the linker rewrites, so only references that keep the
// pointer warrant LNK4217: on x86-64 these objects describe their sites, on
// ARM64 they carry link-only records.
bool defersLocalImportWarning(const ObjFile *file, bool arm64) {
  return file->describesSites() || (arm64 && file->hasLinkRecords());
}

// A .refptr.X section holds only a pointer to X. Where X is in the image or an
// absent weak reference, and references can be rewritten, the pointer binds a
// local import to X or zero; it stays wherever the image still needs it.
void bindPointerCells(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  if (symtab.pointerCells.empty() || symtab.isEC() || ctx.hybridSymtab)
    return;
  if (symtab.machine != ARM64 &&
      (symtab.machine != AMD64 ||
       llvm::none_of(ctx.objFileInstances,
                     [](ObjFile *f) { return f->describesSites(); })))
    return;
  llvm::TimeTraceScope timeScope("Bind pointer cells");
  for (Symbol *s : symtab.pointerCells) {
    auto *cell = dyn_cast<DefinedRegular>(s);
    SectionChunk *sc = cell ? cell->getChunk() : nullptr;
    if (!sc || sc->getSize() != ctx.config.wordsize ||
        sc->getRelocs().size() != 1 || sc->getRelocs()[0].VirtualAddress != 0 ||
        !sc->children().empty())
      continue;
    Symbol *x = sc->file->getSymbol(sc->getRelocs()[0].SymbolTableIndex);
    if (!x || x->getName() != cell->getName().substr(strlen(".refptr.")))
      continue;
    // An unresolved weak reference resolves to its default after mark-live.
    Defined *target = x->getDefined();
    auto *abs = dyn_cast_or_null<DefinedAbsolute>(target);
    bool absentWeak = isa<Undefined>(x) && abs && abs->getVA() == 0;
    if (!target || isa<DefinedImportThunk, DefinedImportData>(target) ||
        (abs && !absentWeak))
      continue;
    replaceSymbol<DefinedLocalImport>(cell, ctx, cell->getName(), target);
    LocalImportChunk *c = cast<DefinedLocalImport>(cell)->getChunk();
    c->cell = sc;
    // Mark-live keeps the pointer when something live refers to it.
    c->live = !ctx.config.doGC;
    symtab.localImportChunks.push_back(c);
  }
}

// Decides which local import pointers a reference still reads. On x86-64 a
// described call, jump or load is rewritten per reference; on ARM64 every adrp
// and 64-bit ldr of a pointer is, per symbol, since an adrp may serve several
// loads. Other references, GC roots and objects that describe nothing read the
// pointer; under -import-slots a data reference reads it but blocks no
// rewrite.
void bindLocalImports(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  std::vector<Chunk *> &localImportChunks = symtab.localImportChunks;
  if (localImportChunks.empty())
    return;
  bool arm64 = symtab.machine == ARM64 && !ctx.hybridSymtab;
  if (!arm64 && llvm::none_of(ctx.objFileInstances, [&](ObjFile *f) {
        return &f->symtab == &symtab && f->describesSites();
      }))
    return;
  llvm::TimeTraceScope timeScope("Bind local imports");
  SmallPtrSet<DefinedLocalImport *, 8> bypassed, read, readByData;
  for (Symbol *b : ctx.config.gcroot)
    if (auto *li = dyn_cast<DefinedLocalImport>(b))
      read.insert(li);
  // The references to warn about if they still read the pointer.
  SmallVector<std::tuple<ObjFile *, DefinedLocalImport *, bool>, 0> refs;
  SmallVector<std::pair<SectionChunk *, const coff_relocation *>, 0>
      arm64Sites;

  // Without mark-live, a compiler pointer stays if another name reaches its
  // section: any symbol in it a relocation names, or an external one.
  DenseMap<SectionChunk *, LocalImportChunk *> cellOf;
  if (!ctx.config.doGC)
    for (Chunk *c : localImportChunks)
      if (SectionChunk *cell = cast<LocalImportChunk>(c)->cell)
        cellOf.try_emplace(cell, cast<LocalImportChunk>(c));
  SmallPtrSet<LocalImportChunk *, 4> named;
  auto findCell = [&](Symbol *s) -> LocalImportChunk * {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    return d && !cellOf.empty() ? cellOf.lookup(d->getChunk()) : nullptr;
  };

  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &symtab)
      continue;
    // An object that describes nothing reads every pointer it names; it is
    // scanned only for a compiler pointer it names, which may stay.
    bool describes = file->describesSites() || arm64;
    if (llvm::none_of(file->getSymbols(), [&](Symbol *s) {
          auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
          return li && (describes || li->getChunk()->cell);
        })) {
      if (!describes)
        for (Symbol *s : file->getSymbols())
          if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s))
            read.insert(li);
      continue;
    }
    for (Symbol *s : file->getSymbols())
      if (LocalImportChunk *c = findCell(s);
          c && c->cell->file == file &&
          cast<DefinedRegular>(s)->getCOFFSymbol().isExternal())
        named.insert(c);

    MapVector<DefinedLocalImport *, bool> seen;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      bool fromData = ctx.config.importSlots &&
                      !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE);
      for (const coff_relocation &rel : sc->getRelocs()) {
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
        if (!li) {
          if (LocalImportChunk *c = findCell(s); c && c->cell->file == file)
            named.insert(c);
          continue;
        }
        if (fromData || !describes) {
          (fromData ? readByData : read).insert(li);
          continue;
        }
        bool rewritable = false;
        if (arm64) {
          rewritable = canBypass(li) && isArm64PointerLoad(sc, rel);
          if (rewritable)
            arm64Sites.push_back({sc, &rel});
        } else if (std::optional<LinkSiteForm> form = getSiteForm(sc, rel);
                   canBypass(li) && form && *form != LinkSiteOther &&
                   *form != LinkSiteAddress) {
          // A jump through a zero pointer keeps reading it.
          if (!isSiteForm(sc->getContents(), rel.VirtualAddress, *form))
            Err(ctx) << file << ": the instruction at offset 0x"
                     << Twine::utohexstr(rel.VirtualAddress) << " in "
                     << sc->getSectionName()
                     << " is not the one its link-only record describes";
          else if (!isZeroPointer(li) || *form == LinkSiteCall ||
                   isLoadForm(form))
            rewritable = true;
          if (rewritable)
            addSiteEdit(sc, rel, *form, li);
        }
        (rewritable ? bypassed : read).insert(li);
        auto [it, inserted] = seen.try_emplace(li, rewritable);
        it->second &= rewritable;
      }
    }
    // A .refptr pointer, a pointer to a data copy, and one to an absent weak
    // symbol's zero import nothing; other objects' references were reported
    // with the undefined symbols.
    if (defersLocalImportWarning(file, arm64))
      for (auto [li, rewritable] : seen)
        if (!li->getName().starts_with(".refptr.") && !isZeroPointer(li) &&
            !li->getChunk()->isCell() && !li->getChunk()->copy)
          refs.push_back({file, li, rewritable});
  }

  // A compiler pointer stays if something live reads it other than through a
  // rewritten instruction, or another name reaches its section.
  if (!ctx.config.doGC)
    for (Chunk *c : localImportChunks)
      if (SectionChunk *cell = cast<LocalImportChunk>(c)->cell)
        cell->live = named.contains(cast<LocalImportChunk>(c));
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
  // The adrp and ldr of a bypassed pointer become adrp and add of its symbol,
  // or movz of zero.
  for (auto [sc, rel] : arm64Sites) {
    auto *li = cast<DefinedLocalImport>(sc->file->getSymbol(rel->SymbolTableIndex));
    if (li->getChunk()->bypassed)
      addEdit(sc, {rel->VirtualAddress, 4, 4,
                   isZeroPointer(li) ? RelaxEdit::Arm64Zero
                   : rel->Type == IMAGE_REL_ARM64_PAGEBASE_REL21
                       ? RelaxEdit::Arm64Page
                       : RelaxEdit::Arm64Add,
                   li->getTarget()});
  }
  llvm::erase_if(localImportChunks, [](Chunk *c) {
    auto *li = cast<LocalImportChunk>(c);
    if (li->cell)
      li->live = li->cell->live;
    return !li->live || li->cell;
  });
  // Laid out by their targets' names, not by symbol-table order.
  llvm::stable_sort(localImportChunks, [](Chunk *a, Chunk *b) {
    return cast<LocalImportChunk>(a)->getTarget()->getName() <
           cast<LocalImportChunk>(b)->getTarget()->getName();
  });
  if (ctx.config.warnLocallyDefinedImported)
    for (auto [file, li, rewritable] : refs)
      if (!rewritable || (arm64 && !li->getChunk()->bypassed))
        Warn(ctx) << file << ": locally defined symbol imported: "
                  << symtab.printSymbol(li->getTarget()) << " (defined in "
                  << li->getTarget()->getFile() << ") [LNK4217]";
}
} // namespace lld::coff
