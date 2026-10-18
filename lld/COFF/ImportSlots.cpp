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

// A word of this image that holds the address of an overridable default X: an
// in-place import slot of X, or the entry through which the image calls X and
// loads its address. The loader writes it with the runtime's entry; the fill
// writes the program's replacement over it when the executable replaces X.
struct OverrideCell {
  // The cell's chunk and offset, or null for the import's entry.
  Chunk *chunk;
  uint32_t offset;
  // The import of the default, or null for a cell of the image's own default.
  DefinedImportData *imp;
  // The default's slot in the table, or, for a default replaced by name, its
  // key in the table's block, which is never zero.
  uint32_t slot;
  uint64_t key;
  bool readOnly;
  // Whether the slot is in the template of the image's thread-local data.
  bool threadLocal;
  // Whether the block's entry for the key names a word that holds the
  // address, as for a vague entity, whose instance the executable imports,
  // rather than the address itself.
  bool indirect = false;
  // For a vague entity, its name, by which a DLL loaded outside the
  // executable's link finds the instance it binds to.
  StringRef name = {};

  uint64_t getRVA() const {
    return chunk ? chunk->getRVA() + offset : imp->getRVA();
  }
  // The key the fill's list orders the cell by: its name's, or for a slot
  // the offset of the slot's RVA in the executable's header and the slot.
  uint64_t getKey() const {
    return key ? key
               : uint64_t(overrideSlotOffset(slot) + 4) << 32 | uint64_t(slot)
                                                                    << 1;
  }
};

// Where the TLS directory says the template of the image's thread-local data
// starts, and the index the loader writes for the image's thread-local blocks:
// a symbol and an addend each.
struct TLSTemplate {
  Defined *start = nullptr;
  int64_t startAddend = 0;
  Defined *index = nullptr;
  int64_t indexAddend = 0;

  uint64_t getIndexRVA() const { return index->getRVA() + indexAddend; }
  uint64_t getStartRVA() const { return start->getRVA() + startAddend; }
};

// The list of the builtins' __llvm_thread_local_slot_copy: the number of
// slots, the RVAs of the TLS index and of the template's start, and a reserved
// word; then for each slot its RVA, a reserved word, and the value of its
// lookup entry, which the slot holds in the file.
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

// The list of the builtins' __llvm_fill, which works through it in order.
//
// The header holds the number of read-only ranges, of cells, of imports bound
// at run time and of residual words; the RVAs of the TLS index and template;
// the RVA and size of the section the fill seals; the key of this DLL's name,
// by which an image that delay-loads it lists it; and a flag word, bit 0 set
// where the fill first runs the executable's copy of its thread-local slots.
//
// A range of the read-only cells is {rva u32, size u32}, one for each output
// section that holds any, which the fill makes writable and read-only again:
// one range across sections would give those between them the protection of
// the first page. Room is left for as many as the output sections the cells'
// input sections are named for, which merging can only reduce.
//
// A cell is {key u64, rva u32, kind u32}, in the order of the keys, so that
// the cells of one key are written together; bit 0 of the key marks a cell in
// read-only data. A cell bound by name has its name's key, which never has bit
// 0 set; a cell of an overridable default with a slot has the offset of the
// slot's RVA in the executable's header in the top half of its key and the
// slot shifted left by one in the bottom. kind is fillCellByName for a default
// bound by name and fillCellSlot for one with a slot, each with
// fillCellThreadLocal set for a cell in the thread-local template; or, for a
// vague entity, whose table entry names a word that holds the address, the RVA
// of its name, the string its export or its import already holds, by which a
// search of the loaded modules binds a cell the executable's table does not
// name, or fillCellNoName if it has none to search by.
//
// An import bound at run time is {cell u32, name u32, dll u32, name length
// u16, dll length u16}: dll is the RVA of the UTF-16 name of the DLL a weak
// import is resolved in, or for an import from the executable, 1 if it is
// weak and 0 if not.
//
// A residual word is {word u32, import u32, addend i64}.
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
    // The pointers and the import address tables are in .rdata.
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
  // The ranges of the read-only cells, one for each output section that holds
  // any, in address order.
  SmallVector<std::pair<uint64_t, uint64_t>, 0> getReadOnlyRanges() const;

  std::vector<OverrideCell> cells;
  std::vector<RunTimeCell> runTimeCells;
  std::vector<ResidualWord> words;
  size_t maxRanges;
  TLSTemplate tls;
  // The exports by name, whose names those of the DLL's own vague entities
  // are, and the hint/name records of the imports.
  DenseMap<StringRef, const Export *> exports;
  const DenseMap<DefinedImportData *, Chunk *> *hintOf = nullptr;
  // A chunk of the section the fill makes read-only at its end, or null.
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

// An image's table of the vague entities it shares, through which the search
// of a DLL loaded at run time finds the instances it uses: a DLL's, which its
// MZ header names, of the entities it keeps preemptible, and the executable's,
// of those it exports. Its header holds the magic, its flags, bit 0 set where
// an entry names the instance itself rather than a word that holds it, the
// number of buckets, a reserved word, the mask of the number of words of its
// filter, a power of two, the number and offset of the keys of the names of
// the DLLs it delay-loads, and the offset of the buckets. The filter follows,
// as ELF's DT_GNU_HASH has one, two bits of each key in one of its words, so
// that a probe of an image that lacks the entity seldom reads a bucket; then
// the buckets, each {key u32, rva u32, name u32}: the top half of the key, the
// entry's RVA, zero for a free bucket, which ends a probe, and the RVA of the
// entity's name, the string its export holds, which a search compares, so
// that the key's half only filters; then those keys. An entity's home bucket
// is the top half of its key times the number of buckets, shifted right by 32,
// so that the number is the entities' and a third, a quarter of the buckets
// free.
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

// Whether a section is unwind or exception-handling data, which the PE format
// defines as RVAs of the image's own code and handlers.
static bool isExceptionData(SectionChunk *sc) {
  StringRef name = sc->getSectionName();
  return name == ".xdata" || name == ".pdata" || name.starts_with(".xdata$") ||
         name.starts_with(".pdata$");
}

DefinedImportData *getAddressedImport(Symbol *s) {
  if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s))
    return thunk->wrappedSym;
  // Data that resolved to its import is a copy of the import's symbol; the
  // import tables know the original.
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

// Whether rel, a reference to an import address table entry, reads the whole
// address the entry holds: a load that its object describes, an ARM64 adrp or
// ldr of it, or an entry of a KCFI type's list, which is the address of a word
// that the type's open routine reads whole.
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

// What the scan of the relocations of the image's sections gathers for the
// decisions that follow it.
struct SlotScan {
  // A field that a relocation writes.
  struct Span {
    uint32_t offset;
    uint32_t size;
    const coff_relocation *rel;
    bool word;
  };
  // A section's candidate slots, and the pairs of its fields that overlap,
  // each field after the one before it that reaches furthest.
  struct Pending {
    SectionChunk *chunk;
    std::vector<ImportSlot> slots;
    Symbol *firstRef = nullptr;
    std::vector<std::pair<Span, Span>> overlaps;
  };
  // An instruction rewritten for imp, unless the import's thunk is its
  // address.
  struct Site {
    SectionChunk *chunk;
    RelaxEdit edit;
    DefinedImportData *imp;
  };
  // How the references other than its slots use an import's address: not at
  // all, only to read it whole, or otherwise.
  enum Use : uint8_t { Unused, Read, Other };

  void use(DefinedImportData *imp, Use u) {
    Use &v = uses[imp->file];
    v = std::max(v, u);
  }

  std::vector<Pending> pending;
  std::vector<Site> addressSites;
  // On ARM64 the loads of a delay-loaded function's entry are rewritten for
  // the symbol as a whole, since an adrp may serve several loads.
  MapVector<DefinedImportData *,
            std::vector<std::pair<SectionChunk *, RelaxEdit>>>
      delayLoads;
  DenseSet<DefinedImportData *> delayLoadsRead;
  // For each import thunk, the references to it, and those that a slot or a
  // rewritten instruction bypasses.
  MapVector<DefinedImportThunk *, std::pair<size_t, size_t>> thunkRefs;
  DenseMap<ImportFile *, Use> uses;
  // For each function whose address is a DLL's instance, the references that
  // keep the image's copy, such as calls.
  DenseMap<Symbol *, size_t> localFunctionRefs;
  // The symbols of the object being scanned that have been reported.
  SmallPtrSet<Symbol *, 4> reported;
  // The symbol table indices that the guard metadata of the object being
  // scanned lists as address-taken, once read.
  std::optional<DenseSet<uint32_t>> guardFids;
  // The fields of the section being checked.
  std::vector<Span> spans;
};

// Collects into overlaps the pairs of fields of sc that overlap, which may not
// cover a slot.
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

// Every word that an in-place import slot covers has that slot as its only
// writer: a relocation overlapping it, which would also be a second writer of
// a base relocation, is an error. An identical duplicate is one binding.
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

// Under -import-slots, a word of static data that holds the address of an
// import is an in-place import slot: the loader writes the address there
// through an import descriptor whose address table is the run of such words
// the word belongs to, so the word needs no base relocation, and a function's
// address is the function's, not its import thunk's. Code that takes the
// address of an imported function in an instruction its object describes, or
// in an ARM64 adrp and add pair, is rewritten to load it from the import
// address table, so that code and static data agree. An object that may take
// it in an instruction it does not describe makes the image use the import
// thunk as the function's address everywhere, with a warning. A delay-loaded
// function keeps its thunk, and code that loads its address from the import
// address table is rewritten to take the thunk's. A reference to data that
// resolved to its import other than by such a word cannot reach the data, and
// is an error.
//
// One scan of the relocations gathers what each decision needs, as MinGW's
// runtime pseudo-relocations and ELF's relocation scan do; the decisions are
// taken once it has settled which functions keep their thunks as their
// addresses, on which every slot and rewritten instruction depends.
void ImportSlotContents::bind() {
  Configuration &config = ctx.config;
  if (!config.importSlots)
    return;
  llvm::TimeTraceScope timeScope("Import slots");

  // An export of data that resolved to its import would publish the address
  // of the import address table entry.
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym);
        imp && imp->isRuntimePseudoReloc)
      Err(ctx) << "cannot export " << ctx.symtab.printSymbol(imp)
               << ": it is imported from " << imp->getDLLName()
               << "; export a forwarder to it instead";

  // An import bound at run time has a cell in .sealed in place of an entry of
  // an import address table, which code reads unchanged, and no descriptor.
  // The bounds of a DLL's KCFI code range, which a KCFI thunk loads together,
  // take one 16-byte block, start then end.
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
    // A .refptr pointer to the function reads the cell instead.
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

// x86-64 cl.exe has no inline assembly, and MSVC's control-flow guard fails at
// run time if a function-typed address-take is missing from an object's
// .gfids$y table. So in an object built with /guard:cf, a REL32 against a
// function-typed symbol that the table does not list is a branch. A symbol
// declared as data, which keeps a function out of the table, proves nothing.
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

// A function that a DLL the image imports from offers, and whose address an
// object may take in an instruction it does not describe, keeps the image's
// copy as its address, which that instruction takes: the image does not split
// the function's address, as link.exe would not.
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

// A function that an object may take the address of in an instruction it
// does not describe, or in a 32-bit field of data, keeps its thunk as its
// address: its words in static data name the thunk, and the instructions that
// take its address are left as they are.
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

// A word that holds an address inside an import's data for which its DLL
// exports no name is the fill's to write. A read-only one moves with its chunk
// to pages that the fill makes read-only again once written, which a read-only
// section that may not move cannot give it. Thread-local data is the
// loader's to copy before any initializer runs.
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
  // An instruction that takes the address of an import thunk, or of what a
  // DLL provides, is rewritten to load it from the import's entry: lea
  // becomes mov, and the add after an ARM64 adrp a 64-bit ldr.
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
  for (size_t i = 0, e = relocs.size(); i != e; ++i) {
    const coff_relocation &rel = relocs[i];
    Symbol *s = file->getSymbol(rel.SymbolTableIndex);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s);
    auto *data = dyn_cast_or_null<DefinedImportData>(s);
    if (thunk)
      ++scan.thunkRefs[thunk].first;

    // The address of a function that a DLL offers is the DLL's instance:
    // a described address-take loads it from the import's entry and a word of
    // static data takes it in place. Calls, and any other reference from
    // outside the function's own sections, keep the image's copy. The address
    // of a definition this DLL offers as an overridable default is the
    // program's choice, which the default's import holds, alike.
    DefinedImportData *offer = ctx.symtab.offeredFunctions.lookup(s);
    if (!offer)
      offer = ctx.symtab.preemptedOffers.lookup(s);
    if (offer) {
      if (code && arm64 && rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
          i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
        addressSite(rel, offer);
        addressSite(relocs[++i], offer);
        continue;
      }
      if (const ObjFile::RelocFact *fact = file->relocFact(sc, rel);
          code && !arm64 && rel.Type == IMAGE_REL_AMD64_REL32 && fact &&
          fact->hasSite && fact->form == LinkSiteAddress &&
          isSiteForm(sc->getContents(), rel.VirtualAddress, LinkSiteAddress)) {
        addressSite(rel, offer);
        continue;
      }
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

    // An import address table entry. The import's data itself is reached only
    // through its slots.
    if (data && !data->isRuntimePseudoReloc) {
      scan.use(data, readsWholeEntry(sc, rel, code, arm64) ? SlotScan::Read
                                                           : SlotScan::Other);
      // That of a delay-loaded function: a load of the address in code takes
      // the thunk's instead, mov becoming lea, and ldr add.
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
        // The address of data that a DLL's instance preempted, which a
        // described instruction takes, becomes a load of the import's entry,
        // as that of a function does.
        bool preempted = file->symtab.preemptedData.contains(s);
        if (preempted && arm64 && rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
            i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
          addressSite(rel, imp);
          addressSite(relocs[++i], imp);
          continue;
        }
        const ObjFile::RelocFact *fact = file->relocFact(sc, rel);
        if (preempted && !arm64 && rel.Type == IMAGE_REL_AMD64_REL32 && fact &&
            fact->hasSite && fact->form == LinkSiteAddress &&
            isSiteForm(sc->getContents(), rel.VirtualAddress,
                       LinkSiteAddress)) {
          addressSite(rel, imp);
          continue;
        }
        if (!scan.reported.insert(s).second)
          continue;
        if (preempted)
          Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                   << " is provided by " << imp->getDLLName()
                   << ", but is referenced as if it were local; recompile "
                      "the object, or give the symbol hidden visibility";
        else {
          // Assembly reaches it through its import pointer, which the GOT
          // spellings of position-independent ELF assembly name.
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
          if (i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
            addressSite(rel, imp);
            addressSite(relocs[++i], imp);
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

    // A 32-bit field cannot hold another image's address. A field through
    // which the function is only called, as a frame's handler is in unwind
    // data, is served by its import thunk; any other function address there
    // makes the thunk the function's address image-wide. An object that lists
    // its call-only fields says which they are; otherwise they are those of
    // exception data, which the PE format defines as RVAs that the system
    // calls.
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
    // The loader adds nothing to the address it writes, so a word that holds
    // an address inside the import's data takes the name that its DLL exports
    // for that address.
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

  // The thunk whose address a rewritten load takes is kept in the image. An
  // instruction that takes an imported function's address loads it from the
  // import's entry once rewritten.
  auto keepThunk = [](DefinedImportData *imp) {
    cast<DefinedImportThunk>(imp->file->thunkSym)->getChunk()->live = true;
  };
  // A load that the linker disposes of, which nothing reads, needs no rewrite.
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

  // An import thunk that every reference bypasses is left out of the image,
  // and so out of the Control Flow Guard tables, unless something other than
  // a relocation names it.
  DenseSet<Symbol *> named(config.gcroot.begin(), config.gcroot.end());
  for (Export &e : ctx.symtab.exports)
    named.insert(e.sym);
  // The words of static data that hold the address of an import bound at run
  // time name its thunk until the fill writes them.
  for (auto &[thunk, refs] : scan.thunkRefs)
    if (refs.first == refs.second && !thunk->wrappedSym->file->thunkIsAddress &&
        !thunk->wrappedSym->file->isBoundAtRunTime() &&
        !thunk->wrappedSym->file->isDelayLoaded() && !named.contains(thunk))
      thunk->getChunk()->live = false;

  // A function's copy that no call keeps is discarded, as one that no
  // reference reached would have been; a root names the function, whose
  // address is the DLL's instance, not the copy.
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
    // A word holding the address of an import bound at run time is the fill's
    // to write, from the import's cell.
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
    // A read-only slot can be written only inside the range the loader
    // makes writable while it binds imports. A chunk of .rdata is moved
    // there. A read-only section whose order or bounds its program may rely
    // on keeps its place: another section is laid out beside that range, and
    // the $-groups of .rdata, together and in their order, at its start.
    // Exception data never holds an import's address.
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
    // A chunk that also holds residual words is writable until they are
    // written.
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
  // How the references other than slots use each import: an export or a root
  // uses it otherwise, as KCFI's lists may, and the fill and a live import
  // thunk read it.
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
  // An import that only the discarded sections of preempted data used needs
  // no entry.
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

  // The slot chunks laid out together are ordered by the DLL of their first
  // slot, so that single-pointer chunks of one DLL form one run. The DLL order
  // of a chunk is found and lower-cased once, not for each comparison.
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

  // Slots of one DLL one word apart form a run: within a chunk, and across
  // two chunks laid out together with no padding between them.
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

  // An import whose address the image keeps in in-place slots needs no entry of
  // its own in its DLL's import address table when nothing else needs one. Its
  // address is then taken to be one of its slots, which every other user reads
  // instead: code that loads the whole address through an instruction its
  // object describes, or through ARM64 adrp and ldr, the import thunk, and the
  // lists of KCFI's open routines. Such a slot must be naturally aligned and in
  // a read-only section that no /merge or /section option changes, so that the
  // image cannot write what they read. An import that anything uses otherwise
  // keeps its entry: a reference from data, an instruction that takes the
  // entry's address or reads part of it, an object that does not describe its
  // instructions, an export or a root.
  DenseSet<ImportFile *> slotted;
  for (const std::vector<ImportSlot *> &slotRun : idata.slotRuns)
    for (ImportSlot *slot : slotRun)
      slotted.insert(slot->sym->file);

  // An import whose words all take interior names, and that nothing else
  // uses, needs no entry at all. What still names it, such as debug
  // information, is given the place of its first such word.
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
  // The first readable slot of each import in the order of the runs, and
  // for an import that nothing else uses, its first slot if none is.
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

// Adds an import that nothing else made live to the import tables, for a pass
// after createImportTables that needs it.
void ImportSlotContents::addImport(DefinedImportData *imp) {
  imp->file->live = true;
  ctx.config.dllOrder.try_emplace(StringRef(imp->file->dllName).lower(),
                                  ctx.config.dllOrder.size());
  idata.add(imp);
}

// Under -import-slots, the image's fill writes what the loader cannot: the
// cells, the imports bound at run time and the residual words, and in a DLL
// that may reach the executable while the DLLs initialize, it first runs the
// executable's copy of its thread-local slots.
void ImportSlotContents::createFill() {
  llvm::TimeTraceScope timeScope("Create fill");
  Configuration &config = ctx.config;
  auto *list = cast_or_null<FillListChunk>(fillList);
  bool hasXIA = isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a"));
  // A DLL that imports from an executable runs the executable's copy, unless
  // it has no C initializer table, in which case nothing could.
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

  // The fill calls ntdll: NtProtectVirtualMemory, and the loader's functions
  // that resolve an import, find a loaded DLL and pin it.
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

// The TLS directory's template start and index: the targets of the
// relocations of its first and third fields. Either is null when the image has
// no directory or the directory names none.
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

// Under -import-slots, an executable's in-place import slots in its
// thread-local template are bound in the template only after the loader has
// copied it into the main thread's block; the copy writes them there too.
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

// Under -import-slots, a DLL's cells of the overridable defaults it imports
// are the words through which it reaches them: each in-place import slot of a
// default, and the default's own entry, which its calls and the instructions
// that take its address read. The fill writes those of a replaced default with
// the replacement, so the DLL observes and calls the one the program chose, as
// on ELF. An executable binds its own at link time, from the table it writes.
void ImportSlotContents::collectCells() {
  llvm::TimeTraceScope timeScope("Collect cells");
  Configuration &config = ctx.config;
  if (!config.importSlots || !config.dll)
    return;
  std::vector<OverrideCell> cells;
  ctx.symtab.readOverrideRecords();
  // As on ELF, where an importer reaches another object's preemptible data
  // through its GOT, the words through which the DLL reaches a vague entity
  // that the DLL it imports it from keeps preemptible are cells too, keyed by
  // the entity's name, since the program may use another image's instance.
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
  // A slot in a chunk with residual words is in a section that stays writable
  // until the fill seals it, after it writes the cells.
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
  // The DLL's own defaults have cells too, as on ELF its address in the
  // defining object is the program's one: the pointer its address-takes read,
  // and the words of its static data that hold its address. Each holds the
  // default until the fill writes the replacement. Both are in read-only data
  // beside the import address tables, or in writable data. A default with a
  // slot is keyed by the slot its definition records, and one without by its
  // name.
  auto addOwn = [&](StringRef name, Chunk *c, uint32_t offset, bool readOnly,
                    bool threadLocal) {
    if (ctx.symtab.isOwnSlotDefault(name))
      cells.push_back({c, offset, nullptr, *ctx.symtab.overrideImportSlot(name),
                       0, readOnly, threadLocal});
    else if (ctx.symtab.overridablesByName.contains(name))
      cells.push_back({c, offset, nullptr, 0, overrideNameKey(name), readOnly,
                       threadLocal});
  };
  // As on ELF, where a shared object's own references to its preemptible
  // data go through its GOT, its references to a vague entity another image's
  // instance may preempt read a cell: the pointer its import-form references
  // read, and the words of its static data that hold its address, keyed by
  // the entity's name. The executable's block names a word holding the
  // instance's address.
  auto addVague = [&](Defined *d, Chunk *c, uint32_t offset, bool readOnly) {
    cells.push_back({c, offset, nullptr, 0, overrideNameKey(d->getName()),
                     readOnly, false, /*indirect=*/true, d->getName()});
  };
  // Each live entity the DLL keeps preemptible has a pointer that its offer
  // table names to the search of a DLL loaded later, made here where no
  // import-form reference of its own made one. A thread-local variable's
  // instance is its record.
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
  // Static data reaches a cell only through an own default or a preemptible
  // vague entity; with none the walk over every live word finds nothing.
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
  // The anchor's mask, which the fill reads, has a bit for each of the first
  // 32 slots.
  for (const OverrideCell &c : cells)
    if (!c.key && c.slot >= 32)
      return fail("that the override table's mask cannot name");
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    return fail("that the image has no C initializer table (__xi_a) to fill");
  // A thread-local cell is written in the template and in the loading thread's
  // block.
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

// A DLL loaded after the process initializes, or delay-loaded by such a DLL,
// is outside the executable's link, and so outside its table: as an ELF object
// opened RTLD_LOCAL resolves against the global scope, it binds its cells to
// the instances that the modules loaded with the process, or one that
// delay-loads it, use, through the offer table each names in its MZ header.
void ImportSlotContents::createOfferTable() {
  llvm::TimeTraceScope timeScope("Create offer table");
  Configuration &config = ctx.config;
  if (!config.importSlots)
    return;
  // The exports by name, whose names those of the image's vague entities are.
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
  // The executable's table: the vague entities it exports, which are the
  // instances it shares, as an ELF executable's dynamic symbols are. No DLL
  // searches for a type_info object, which images no link joins compare by
  // name.
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
  // A DLL's: the pointer of each entity it keeps preemptible and exports,
  // which holds the instance it uses, and the keys of the names of the DLLs it
  // delay-loads.
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

// The loader writes an in-place import slot with an absolute address, makes
// only the import address table directory writable while it binds imports, and
// restores one protection over all of it. Each slot's word must therefore be
// in a section that is not executable, a read-only one inside the directory,
// whose page-rounded range has one protection; each run must still be one word
// a slot.
void ImportSlotContents::check(Chunk *iatStart, uint64_t iatSize) {
  llvm::TimeTraceScope timeScope("Check import slots");
  // A fill makes the pages of each range of its read-only cells writable and
  // then read-only again; every section those pages hold must therefore be
  // read-only data.
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
