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
#include "LocalImports.h"
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

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;

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
  // A field that a relocation writes.
  struct Span {
    uint32_t offset;
    uint32_t size;
    const coff_relocation *rel;
    bool word;
  };
  // A section's candidate slots, and its overlapping pairs of fields.
  struct Pending {
    SectionChunk *chunk;
    std::vector<ImportSlot> slots;
    Symbol *firstRef = nullptr;
    std::vector<std::pair<Span, Span>> overlaps;
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
  std::vector<Span> spans;
};

// Collects the pairs of sc's fields that overlap, each with the one before it
// that reaches furthest.
static void
findOverlaps(SlotScan &scan, SectionChunk *sc,
             std::vector<std::pair<SlotScan::Span, SlotScan::Span>> &overlaps) {
  uint32_t wordsize = sc->file->symtab.ctx.config.wordsize;
  std::vector<SlotScan::Span> &spans = scan.spans;
  spans.clear();
  for (const coff_relocation &rel : sc->getRelocs()) {
    if (rel.Type == 0) // IMAGE_REL_*_ABSOLUTE writes nothing.
      continue;
    bool word = sc->isAddressWord(rel);
    spans.push_back({rel.VirtualAddress, word ? wordsize : 4u, &rel, word});
  }
  llvm::stable_sort(spans,
                    [](const SlotScan::Span &a, const SlotScan::Span &b) {
                      return a.offset < b.offset;
                    });
  const SlotScan::Span *last = nullptr;
  for (const SlotScan::Span &s : spans) {
    if (last && uint64_t(last->offset) + last->size > s.offset)
      overlaps.push_back({*last, s});
    if (!last ||
        uint64_t(s.offset) + s.size > uint64_t(last->offset) + last->size)
      last = &s;
  }
}

// A slot must be its word's only writer; an identical duplicate is one.
static bool validate(COFFLinkerContext &ctx, SlotScan::Pending &p) {
  auto isSlot = [&](const SlotScan::Span &s) {
    auto it = llvm::partition_point(
        p.slots, [&](const ImportSlot &x) { return x.offset < s.offset; });
    return s.word && it != p.slots.end() && it->offset == s.offset;
  };
  bool ok = true;
  for (auto &[last, s] : p.overlaps) {
    bool lastSlot = isSlot(last);
    if (!lastSlot && !isSlot(s))
      continue;
    if (last.offset == s.offset && last.size == s.size &&
        last.rel->Type == s.rel->Type &&
        last.rel->SymbolTableIndex == s.rel->SymbolTableIndex)
      continue;
    Err(ctx) << p.chunk->file << ": the address of an import at offset 0x"
             << Twine::utohexstr(lastSlot ? last.offset : s.offset) << " in "
             << p.chunk->getSectionName()
             << " overlaps the relocation at offset 0x"
             << Twine::utohexstr(lastSlot ? s.offset : last.offset);
    ok = false;
  }
  if (!ok)
    return false;
  p.slots.erase(llvm::unique(p.slots,
                             [](const ImportSlot &a, const ImportSlot &b) {
                               return a.offset == b.offset;
                             }),
                p.slots.end());
  return true;
}

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
  SlotScan::Pending p{sc, {}, nullptr, {}};
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
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is imported from " << imp->getDLLName() << ", but "
                 << sc->getSectionName()
                 << " refers to it with relocation type "
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
          Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                   << " is imported from " << imp->getDLLName()
                   << ", but the object was compiled as if it were "
                      "local; mark its declaration, or compile the object "
                      "with -fno-direct-access-external-data; in assembly, "
                      "reach it through "
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
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is imported from " << imp->getDLLName()
                 << ", but an instruction in " << sc->getSectionName()
                 << " reads its bytes, which its import thunk's are not";
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
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is imported from " << imp->getDLLName() << ", but "
                 << sc->getSectionName()
                 << " refers to it with relocation type "
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
  if (p.slots.empty())
    return;
  findOverlaps(scan, sc, p.overlaps);
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
    llvm::stable_sort(p.slots, [](const ImportSlot &a, const ImportSlot &b) {
      return a.offset < b.offset;
    });
    if (!validate(ctx, p))
      continue;
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
} // namespace lld::coff
