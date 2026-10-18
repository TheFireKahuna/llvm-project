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
//===----------------------------------------------------------------------===//
// Instruction forms
//===----------------------------------------------------------------------===//

static void reportMismatch(const SectionChunk *sc, uint32_t off) {
  Err(sc->file->symtab.ctx)
      << sc->file << ": the instruction at offset 0x" << Twine::utohexstr(off)
      << " in " << sc->getSectionName()
      << " is not the one its link-only record describes";
}

static bool isLoadForm(std::optional<LinkSiteForm> form) {
  return form == LinkSiteLoad || form == LinkSiteLoadREX2;
}

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


// The edit of sc whose bytes include off, if any.
static RelaxEdit *findEdit(const SectionChunk *sc, uint32_t off) {
  auto &relaxations = sc->file->symtab.ctx.relaxations;
  auto it = relaxations.find(sc);
  return it == relaxations.end() ? nullptr : it->second.findEdit(off);
}

// The rewrite that makes the instruction at rel, of the given form, reach
// target, as lld/ELF's relaxGot rewrites a GOT reference: a load (or adrp and
// ldr) becomes an address-take, an address-take (or adrp and add) a load of
// target, a pointer, and a call or jump through a pointer a direct one; with
// zero, the instruction materialises zero. Resized code drops the padding a
// direct call or jump leaves. The form is ignored on ARM64.
static RelaxEdit getRetarget(const SectionChunk *sc, const coff_relocation &rel,
                             LinkSiteForm form, Defined *target, bool zero) {
  uint32_t off = rel.VirtualAddress;
  if (sc->getArch() == Triple::aarch64)
    return {off, 4, 4,
            zero ? RelaxEdit::Arm64Zero
            : rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 ? RelaxEdit::Arm64Page
            : rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L ? RelaxEdit::Arm64Add
                                                         : RelaxEdit::Arm64Load,
            target};
  switch (form) {
  case LinkSiteCall:
    return {off - 2, 6, uint16_t(zero ? 6 : 5),
            zero ? RelaxEdit::ZeroCall : RelaxEdit::DirectCall, target};
  case LinkSiteJump:
  case LinkSiteJumpOnePrefix: {
    uint16_t size = form == LinkSiteJump ? 6 : 7;
    return {off + 4 - size, size, 5, RelaxEdit::DirectJump, target};
  }
  case LinkSiteAddress:
    return {off - 3, 7, 7, RelaxEdit::LoadPointer, target};
  default: {
    uint16_t size = form == LinkSiteLoadREX2 ? 8 : 7;
    return {off + 4 - size, size, size,
            zero ? RelaxEdit::ZeroLoad : RelaxEdit::LoadAddress, target};
  }
  }
}

namespace {
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

// The list that the builtins' __llvm_thread_local_slot_copy reads, laid out as
// fill_windows.c describes.
class CopyListChunk : public NonSectionChunk {
public:
  CopyListChunk(std::vector<const ImportSlot *> slots)
      : slots(std::move(slots)) {
    setAlignment(8);
  }
  size_t getSize() const override { return 8 + 16 * slots.size(); }
  void writeTo(uint8_t *buf) const override {
    write64le(buf, slots.size());
    for (const ImportSlot *s : slots) {
      write32le(buf + 8, s->chunk->getRVA() + s->offset);
      s->lookup->writeTo(buf + 16);
      buf += 16;
    }
  }
  uint32_t getOutputCharacteristics() const override {
    return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  }

private:
  std::vector<const ImportSlot *> slots;
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
  static constexpr uint32_t headerSize = 40;
  size_t getSize() const override {
    return headerSize + 8 * maxRanges + 16 * (cells.size() + words.size()) +
           12 * runTimeCells.size();
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
  auto *header = reinterpret_cast<support::ulittle32_t *>(buf);
  header[0] = ranges.size();
  header[1] = cells.size();
  header[2] = words.size();
  header[3] = runTimeCells.size();
  if (OutputSection *seal = sealed ? ctx.getOutputSection(sealed) : nullptr) {
    header[4] = seal->getRVA();
    header[5] = seal->getVirtualSize();
  }
  write64le(buf + 24, selfKey);
  header[8] = callsCopy;
  auto *range = reinterpret_cast<support::ulittle32_t *>(buf + headerSize);
  for (auto [start, end] : ranges) {
    *range++ = start;
    *range++ = end - start;
  }
  buf += headerSize + 8 * maxRanges;
  for (const OverrideCell &c : cells) {
    write64le(buf, c.getKey() | c.readOnly);
    write32le(buf + 8, c.getRVA());
    write32le(buf + 12, getKind(c));
    buf += 16;
  }
  for (const ResidualWord &w : words) {
    write32le(buf, w.chunk->getRVA() + w.offset);
    write32le(buf + 4, w.imp->getRVA());
    write64le(buf + 8, w.addend);
    buf += 16;
  }
  for (const RunTimeCell &c : runTimeCells) {
    write32le(buf, c.cell->getRVA());
    write32le(buf + 4, c.name->getRVA());
    write32le(buf + 8, c.dllName ? c.dllName->getRVA()
                                 : uint32_t(c.imp->file->isWeakImport));
    buf += 12;
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

static void noteUse(DefinedImportData *imp, ImportFile::EntryUse use) {
  imp->file->entryUse = std::max(imp->file->entryUse, use);
}

// How references other than slots use an import's entry; a live thunk reads
// it whole.
static ImportFile::EntryUse getEntryUse(ImportFile *f) {
  auto *thunk = dyn_cast_or_null<DefinedImportThunk>(f->thunkSym);
  return thunk && thunk->getChunk()->live
             ? std::max(f->entryUse, ImportFile::ReadWhole)
             : f->entryUse;
}

// A candidate slot, with the symbol its relocation names and whether that is
// an offered function; and a section's candidates.
struct SlotCandidate {
  ImportSlot slot;
  Symbol *ref;
  bool offer;
};
struct SlotScanPending {
  SectionChunk *chunk;
  std::vector<SlotCandidate> slots;
};

// What the scan of the image's relocations gathers for the decisions after it.
struct SlotScan {
  using Candidate = SlotCandidate;
  using Pending = SlotScanPending;
  // An instruction rewritten for imp, unless its thunk becomes its address,
  // and the offered function it takes the address of, if any.
  struct Site {
    SectionChunk *chunk;
    RelaxEdit edit;
    DefinedImportData *imp;
    Symbol *offer = nullptr;
  };
  std::vector<Pending> pending;
  std::vector<Site> addressSites;
  // The offered functions some reference keeps the image's copy of, and those
  // an object may take the address of in an instruction it does not describe,
  // which keep the copy as their address, so it is not split.
  DenseSet<Symbol *> keptCopies;
  MapVector<Symbol *, ObjFile *> undescribedOffers;
  // The words of a DLL's static data that may hold its own overridable
  // default's or preemptible vague entity's address, which are cells.
  bool collectsOwnWords = false;
  std::vector<std::tuple<SectionChunk *, uint32_t, Defined *>> ownWords;
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
  if (!slotScan)
    return;
  llvm::TimeTraceScope timeScope("Import slots");
  resolve(*slotScan);
}

// An import bound at run time has a cell in .sealed instead of an entry. A
// DLL's KCFI code range bounds, which a thunk loads together, share a 16-byte
// block.
void ImportSlotContents::addRunTimeCells() {
  // ntdll takes a module's name in UTF-16: its little-endian bytes, the first
  // byte of the terminator, and StringChunk's NUL.
  auto wideName = [&](StringRef name) {
    SmallVector<UTF16, 0> wide;
    convertUTF8ToUTF16String(name, wide);
    std::string bytes;
    for (UTF16 c : wide)
      bytes += {char(c), char(c >> 8)};
    bytes += '\0';
    auto *chunk = make<StringChunk>(saver().save(bytes));
    chunk->setAlignment(2);
    return chunk;
  };
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
                  imp->file->isHostImport ? nullptr
                                          : wideName(imp->getDLLName())};
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

// Gathers what the decisions need from the reference at relocs[i] of sc; an
// ARM64 address pair is one reference, and i moves to its add.
void ImportSlotContents::scanReference(SlotScan &scan, SectionChunk *sc,
                                       size_t &i, SlotScan::Pending &p) {
  Configuration &config = ctx.config;
  ObjFile *file = sc->file;
  bool arm64 = config.machine == ARM64;
  bool code = sc->header->Characteristics & IMAGE_SCN_CNT_CODE;
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  size_t e = relocs.size();
  const coff_relocation &rel = relocs[i];
  Symbol *s = file->getSymbol(rel.SymbolTableIndex);
  auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s);
  auto *data = dyn_cast_or_null<DefinedImportData>(s);
  ImportFile *thunkFile = thunk ? thunk->wrappedSym->file : nullptr;
  if (thunkFile)
    ++thunkFile->thunkRefs;
  if (auto *d = dyn_cast_or_null<Defined>(s);
      d && scan.collectsOwnWords && !code && !isExceptionData(sc) &&
      sc->isAddressWord(rel) && getAddressWordAddend(sc, rel) == 0 &&
      (ctx.symtab.isPreemptibleVagueEntity(d) ||
       ctx.symtab.isOwnSlotDefault(d->getName()) ||
       ctx.symtab.overridablesByName.contains(d->getName())))
    scan.ownWords.push_back({sc, rel.VirtualAddress, d});
  auto importedBut = [&](DefinedImportData *imp) {
    COFFSyncStream err = Err(ctx);
    err << file << ": " << file->symtab.printSymbol(s) << " is imported from "
        << imp->getDLLName() << ", but ";
    return err;
  };
  auto badType = [&](DefinedImportData *imp, StringRef why) {
    if (scan.reported.insert(s).second)
      importedBut(imp) << sc->getSectionName()
                       << " refers to it with relocation type "
                       << file->getCOFFObj()->getRelocationTypeName(rel.Type)
                       << ", which " << why;
  };
  // An ARM64 adrp and add pair, or an instruction its object describes, that
  // takes an address, which the import's entry then gives; it bypasses the
  // thunk.
  auto rewriteAddress = [&](DefinedImportData *imp, Symbol *offer = nullptr) {
    const coff_relocation &r = relocs[i];
    const ObjFile::RelocFact *fact = file->relocFact(sc, r);
    bool pair = arm64 && r.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
                i + 1 != e && isArm64AddressPair(sc, r, relocs[i + 1]);
    if (!pair && (arm64 || r.Type != IMAGE_REL_AMD64_REL32 || !fact ||
                  !fact->hasSite || fact->form != LinkSiteAddress ||
                  !isSiteForm(sc->getContents(), r.VirtualAddress,
                              LinkSiteAddress)))
      return false;
    for (size_t k = i, last = i + pair; k <= last; ++k)
      scan.addressSites.push_back(
          {sc, getRetarget(sc, relocs[k], LinkSiteAddress, imp, false), imp,
           offer});
    if (thunkFile) {
      thunkFile->thunkRefs += pair;
      thunkFile->thunkBypasses += 1 + pair;
    }
    i += pair;
    return true;
  };

  // The address of a function a DLL offers is the DLL's instance, and that
  // of an overridable default this DLL offers the program's choice; calls
  // and other references keep the image's copy, as does an address-take in
  // an instruction its object does not describe.
  DefinedImportData *offer = ctx.symtab.offeredFunctions.lookup(s);
  if (offer && code &&
      (arm64 ? rel.Type == IMAGE_REL_ARM64_REL21
             : !file->describesSites() && rel.Type == IMAGE_REL_AMD64_REL32 &&
                   !isGuardMetadataBranch(scan.guardFids, file, rel)))
    scan.undescribedOffers.try_emplace(s, file);
  else if (!offer)
    offer = ctx.symtab.preemptedOffers.lookup(s);
  if (offer) {
    if (code && !scan.undescribedOffers.count(s) &&
        rewriteAddress(offer, s))
      return;
    if (!code && !isExceptionData(sc) && sc->isAddressWord(rel) &&
        getAddressWordAddend(sc, rel) == 0) {
      p.slots.push_back({{sc, rel.VirtualAddress, offer}, s, true});
      return;
    }
    SectionChunk *copy = cast<DefinedRegular>(s)->getChunk();
    if (sc != copy && llvm::none_of(copy->children(), [&](SectionChunk &c) {
          return &c == sc;
        }))
      scan.keptCopies.insert(s);
    return;
  }

  // An import's entry; its data is reached only through slots.
  if (data && !data->isRuntimePseudoReloc) {
    // A described load, an ARM64 adrp or ldr, or a KCFI list entry reads
    // the whole address.
    std::optional<LinkSiteForm> form = getDescribedSite(sc, rel);
    bool load = arm64 ? isArm64PointerLoad(sc, rel) : isLoadForm(form);
    bool whole = !code       ? isKCFIListEntry(sc, rel)
                 : arm64     ? load
                             : form && *form != LinkSiteAddress;
    noteUse(data, whole ? ImportFile::ReadWhole : ImportFile::UsedOtherwise);
    // A delay-loaded function's: a load in code takes the thunk's address,
    // mov becoming lea and ldr add.
    if (!data->file->thunkSym || !data->file->isDelayLoaded())
      return;
    if (code && load)
      scan.addressSites.push_back(
          {sc,
           getRetarget(sc, rel, form.value_or(LinkSiteOther),
                       cast<Defined>(data->file->thunkSym), false),
           data});
    else if (!code || arm64)
      data->file->delayLoadRead = true;
    return;
  }

  DefinedImportData *imp = getAddressedImport(s);
  if (!imp)
    return;
  // A section-relative offset names a section of this image.
  if (sc->isSectionRelative(rel))
    return badType(imp, "names a section of this image");
  if (code) {
    if (!thunk) {
      // Data a DLL's instance preempted is reached as a function's is.
      bool preempted = file->symtab.preemptedData.contains(s);
      if (preempted && rewriteAddress(imp))
        return;
      if (!scan.reported.insert(s).second)
        return;
      if (preempted)
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is provided by " << imp->getDLLName()
                 << ", but is referenced as if it were local; recompile "
                    "the object, or give the symbol hidden visibility";
      else {
        StringRef name = imp->getName().drop_front(strlen("__imp_"));
        importedBut(imp)
            << "the object was compiled as if it were local; mark its "
               "declaration, or compile the object with "
               "-fno-direct-access-external-data; in assembly, reach it "
               "through "
            << imp->getName() << " or "
            << (arm64 ? (":got:" + name).str() : (name + "@GOTPCREL").str());
      }
      return;
    }
    if (sc->isAddressWord(rel)) {
      Err(ctx) << file << ": " << sc->getSectionName()
               << " is executable and holds the address of "
               << file->symtab.printSymbol(s) << ", imported from "
               << imp->getDLLName();
      return;
    }
    if (imp->file->isDelayLoaded())
      return;
    // An address-take the linker cannot rewrite makes the thunk the
    // function's address.
    std::optional<LinkSiteForm> form = getSiteForm(sc, rel);
    if (arm64 ? rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 && rewriteAddress(imp)
              : rel.Type == IMAGE_REL_AMD64_REL32 && file->describesSites() &&
                    rewriteAddress(imp))
      return;
    if (arm64 ? is_contained({IMAGE_REL_ARM64_PAGEBASE_REL21,
                              IMAGE_REL_ARM64_PAGEOFFSET_12A,
                              IMAGE_REL_ARM64_REL21},
                             rel.Type)
              : rel.Type == IMAGE_REL_AMD64_REL32 &&
                    (file->describesSites()
                         ? form == LinkSiteOther
                         : !isGuardMetadataBranch(scan.guardFids, file, rel)))
      useThunkAsAddress(scan, file, s, imp);
    else if (form == LinkSiteAddress)
      reportMismatch(sc, rel.VirtualAddress);
    else if (rel.Type >= IMAGE_REL_AMD64_REL32_1 &&
             rel.Type <= IMAGE_REL_AMD64_REL32_5 && !arm64)
      importedBut(imp) << "an instruction in " << sc->getSectionName()
                       << " reads its bytes, which its import thunk's are not";
    return;
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
    else if (!thunk)
      badType(imp, "cannot reach another image");
    return;
  }
  if (imp->file->isDelayLoaded())
    return;
  std::optional<int64_t> addend = getAddressWordAddend(sc, rel);
  if (!addend) {
    Err(ctx) << file << ": the address of " << file->symtab.printSymbol(s)
             << " at offset 0x" << Twine::utohexstr(rel.VirtualAddress)
             << " extends past the end of " << sc->getSectionName()
             << " (size 0x" << Twine::utohexstr(sc->getSize()) << ")";
    return;
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
      return;
    }
    if (!interior->file->live)
      addImport(interior);
    interiorBases.try_emplace(imp->file, sc, rel.VirtualAddress);
    imp = interior;
  }
  p.slots.push_back({{sc, rel.VirtualAddress, imp}, s, false});
  if (thunkFile)
    ++thunkFile->thunkBypasses;
}

void ImportSlotContents::resolve(SlotScan &scan) {
  Configuration &config = ctx.config;
  heldBackChunks.insert(sealedChunks.begin(), sealedChunks.end());

  // An offered function whose address an object may take in an instruction
  // it does not describe is no offer: its address-takes and words keep the
  // image's copy.
  for (auto [s, file] : scan.undescribedOffers) {
    Warn(ctx) << file << ": may take the address of "
              << file->symtab.printSymbol(s) << ", which "
              << ctx.symtab.offeredFunctions.lookup(s)->getDLLName()
              << " provides, in an instruction it does not describe, so the "
                 "image keeps its own copy as its address; rebuild the "
                 "object with clang";
    ctx.symtab.offeredFunctions.erase(s);
  }
  llvm::erase_if(scan.addressSites, [&](const SlotScan::Site &site) {
    return site.offer && scan.undescribedOffers.count(site.offer);
  });
  // A compiler's pointer that local imports leave out holds no slot.
  llvm::erase_if(scan.pending, [&](SlotScan::Pending &p) {
    llvm::erase_if(p.slots, [&](const SlotScan::Candidate &c) {
      return c.offer && scan.undescribedOffers.count(c.ref);
    });
    return p.slots.empty() || !p.chunk->live;
  });

  // A rewritten load of a delay-loaded function takes its thunk's address.
  auto keepThunk = [](DefinedImportData *imp) {
    cast<DefinedImportThunk>(imp->file->thunkSym)->getChunk()->live = true;
  };
  // A load the linker already disposes of needs no rewrite.
  auto addSite = [&](SectionChunk *sc, const RelaxEdit &edit) {
    if (!findEdit(sc, edit.offset))
      addEdit(sc, edit);
  };
  // ARM64 loads of a delay-loaded function's entry are rewritten all or none,
  // since an adrp may serve several loads.
  bool arm64 = config.machine == ARM64;
  for (const SlotScan::Site &site : scan.addressSites) {
    ImportFile *f = site.imp->file;
    if (f->thunkIsAddress || (arm64 && f->isDelayLoaded() && f->delayLoadRead))
      continue;
    addSite(site.chunk, site.edit);
    if (f->isDelayLoaded())
      keepThunk(site.imp);
    else
      noteUse(site.imp, ImportFile::ReadWhole);
  }

  // A thunk every reference bypasses is left out, unless a root or an export
  // names it.
  DenseSet<Symbol *> named(config.gcroot.begin(), config.gcroot.end());
  for (Export &e : ctx.symtab.exports)
    named.insert(e.sym);
  // Words holding an import bound at run time name its thunk until the fill.
  for (ImportFile *f : ctx.importFileInstances)
    if (f->thunkRefs && f->thunkRefs == f->thunkBypasses && !f->thunkIsAddress &&
        !f->isBoundAtRunTime() && !f->isDelayLoaded() &&
        !named.contains(f->thunkSym))
      cast<DefinedImportThunk>(f->thunkSym)->getChunk()->live = false;

  // An offered function's copy that no call keeps is discarded.
  for (auto &[sym, imp] : ctx.symtab.offeredFunctions) {
    if (scan.keptCopies.contains(sym))
      continue;
    SectionChunk *copy = cast<DefinedRegular>(sym)->getChunk();
    copy->live = false;
    for (SectionChunk &child : copy->children())
      child.live = false;
  }

  std::vector<SectionChunk *> writable;
  for (SlotScan::Pending &p : scan.pending) {
    SectionChunk *sc = p.chunk;
    Symbol *firstRef = p.slots.front().ref;
    // The fill writes a word of an import bound at run time from its cell.
    std::vector<ImportSlot> slots;
    for (const SlotScan::Candidate &c : p.slots) {
      ImportFile *f = c.slot.sym->file;
      if (f->isBoundAtRunTime() && !f->thunkIsAddress)
        addResidualWord(sc, c.slot.offset, c.slot.sym, c.slot.sym, 0);
      else if (!f->thunkIsAddress)
        slots.push_back(c.slot);
    }
    if (slots.empty())
      continue;
    // An identical duplicate relocation is one slot.
    llvm::stable_sort(slots, [](const ImportSlot &a, const ImportSlot &b) {
      return a.offset < b.offset;
    });
    slots.erase(llvm::unique(slots,
                             [](const ImportSlot &a, const ImportSlot &b) {
                               return a.offset == b.offset;
                             }),
                slots.end());
    // The loader writes read-only slots only inside the import address table
    // directory: a .rdata chunk moves there, while another read-only section,
    // or a $-group of .rdata, keeps its order and is laid out beside it.
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
    StringRef name = getPartialSectionName(sc, config.mingw);
    StringRef outName = getOutputSectionName(name, config.mingw);
    if (readOnly && outName == ".xdata") {
      Err(ctx) << sc->file << ": " << sc->getSectionName()
               << " is read-only and holds the address of "
               << sc->file->symtab.printSymbol(firstRef) << ", imported from "
               << slots[0].sym->getDLLName()
               << ", but cannot be laid out with the import address table";
      continue;
    }
    sc->hasImportSlots = true;
    ctx.importSlots[sc] = std::move(slots);
    // A chunk with residual words stays writable until the fill seals it.
    if (sealedChunks.contains(sc)) {
      writable.push_back(sc);
    } else if (readOnly && name == ".rdata") {
      slotChunks.push_back(sc);
    } else if (!readOnly && name == ".data" &&
               sc->getOutputCharacteristics() ==
                   (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                    IMAGE_SCN_MEM_WRITE)) {
      writableSlotChunks.push_back(sc);
    } else {
      if (readOnly && outName == ".rdata")
        slotRdataGroups = true;
      else if (readOnly)
        slotSections.insert(outName);
      writable.push_back(sc);
    }
  }
  // KCFI lists and the fill read imports whole; KCFI ranges, vtable owner
  // runs, exports and roots use them otherwise.
  for (auto [imp, section] : ctx.kcfi.listedImports)
    noteUse(imp, ImportFile::ReadWhole);
  for (const ResidualWord &w : residualWords)
    noteUse(w.imp, ImportFile::ReadWhole);
  for (const KCFIState::ImportedRange &r : ctx.kcfi.importedRanges)
    for (DefinedImportData *imp : {r.start, r.end})
      noteUse(imp, ImportFile::UsedOtherwise);
  for (const COFFLinkerContext::VTableOwnerRun &r : ctx.vtableOwnerRuns)
    for (DefinedImportData *imp : r.words)
      noteUse(imp, ImportFile::UsedOtherwise);
  for (Symbol *s : config.gcroot)
    if (auto *imp = dyn_cast<DefinedImportData>(s))
      noteUse(imp, ImportFile::UsedOtherwise);
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym))
      noteUse(imp, ImportFile::UsedOtherwise);
  // Chunks laid out together are ordered by their first slot's DLL, so that
  // one DLL's single-pointer chunks form one run.
  DenseMap<SectionChunk *, int> dllIndexOf;
  auto dllIndex = [&](SectionChunk *sc) {
    auto [it, inserted] = dllIndexOf.try_emplace(sc, 0);
    if (inserted)
      it->second =
          config.dllOrder[ctx.importSlots[sc].front().sym->getDLLName().lower()];
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
  auto addRuns = [&](ArrayRef<SectionChunk *> chunks, bool together) {
    prev = nullptr;
    for (SectionChunk *sc : chunks) {
      if (!together)
        prev = nullptr;
      for (ImportSlot &s : ctx.importSlots[sc]) {
        if (!follows(s))
          idata.slotRuns.emplace_back();
        idata.slotRuns.back().push_back(&s);
        prev = &s;
      }
    }
  };
  addRuns(writable, false);
  addRuns(writableSlotChunks, true);
  addRuns(slotChunks, true);

  // An import whose words all take interior names, and that nothing else
  // uses, needs no entry; debug information names its first word. Nor does
  // one that only discarded preempted data used.
  DenseSet<ImportFile *> slotted;
  for (const std::vector<ImportSlot *> &run : idata.slotRuns)
    for (ImportSlot *slot : run)
      slotted.insert(slot->sym->file);
  DenseSet<DefinedImportData *> unneeded;
  auto unneed = [&](ImportFile *file) {
    file->live = false;
    unneeded.insert(file->impSym);
  };
  if (!ctx.importSlots.empty())
    for (auto &[file, word] : interiorBases)
      if (getEntryUse(file) == ImportFile::Unused && !slotted.contains(file)) {
        file->impSym->setLocation(word.first, word.second);
        unneed(file);
      }
  for (ImportFile *file : ctx.symtab.preemptedGroupImports)
    if (file->live && getEntryUse(file) == ImportFile::Unused &&
        !slotted.contains(file) && !interiorBases.contains(file))
      unneed(file);
  llvm::erase_if(idata.imports,
                 [&](DefinedImportData *imp) { return unneeded.contains(imp); });

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
        ImportFile::EntryUse u = getEntryUse(slot->sym->file);
        if (u != ImportFile::UsedOtherwise &&
            (any ? u == ImportFile::Unused : readable(*slot)))
          chosen.try_emplace(slot->sym->file, slot);
      }
  for (auto &[file, slot] : chosen) {
    slot->sym->setLocation(slot->chunk, slot->offset);
    idata.slotOnly.insert(slot->sym);
    if (getEntryUse(file) == ImportFile::ReadWhole)
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

// Keeps the builtins' function name, with what it reaches, for an image that
// needs it, and defines the list it reads.
static Defined *useBuiltin(COFFLinkerContext &ctx, StringRef name,
                           StringRef listName, Chunk *list) {
  auto *f = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(name));
  if (!f)
    return nullptr;
  markLive(ctx, {f});
  Symbol *s = ctx.symtab.find(listName);
  replaceSymbol<DefinedSynthetic>(s, s->getName(), list);
  return f;
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
  SymbolTable &symtab = ctx.symtab;
  symtab.readOverrideRecords();
  StringMap<StringSet<>> preemptibleOffers;
  if (isa_and_nonnull<Defined>(symtab.findUnderscore("__xi_a")))
    preemptibleOffers = getPreemptibleOffers(ctx);
  // A word holding the address of name, an overridable default, imported
  // through imp or the image's own, is keyed by its slot or by its name; one
  // holding a vague entity's is keyed by its name and names a word holding
  // the instance: the image's own entity before its defaults, an imported one
  // its DLL keeps preemptible after them.
  auto addCell = [&](Chunk *c, uint32_t offset, DefinedImportData *imp,
                     Defined *own, StringRef name, bool readOnly,
                     bool threadLocal) {
    auto vague = [&] {
      if (threadLocal)
        return false;
      if (own)
        return symtab.isPreemptibleVagueEntity(own);
      auto it = preemptibleOffers.find(imp->getDLLName().lower());
      return it != preemptibleOffers.end() && it->second.contains(name);
    };
    std::optional<uint32_t> slot = imp || symtab.isOwnSlotDefault(name)
                                       ? symtab.overrideImportSlot(name)
                                       : std::nullopt;
    uint64_t key = overrideNameKey(name);
    if (own && vague())
      cells.push_back({c, offset, imp, 0, key, readOnly, false, true, name});
    else if (slot)
      cells.push_back({c, offset, imp, *slot, 0, readOnly, threadLocal});
    else if (imp ? symtab.defaultsByName.contains(name)
                 : symtab.overridablesByName.contains(name))
      cells.push_back({c, offset, imp, 0, key, readOnly, threadLocal});
    else if (imp && vague())
      cells.push_back({c, offset, imp, 0, key, readOnly, false, true, name});
  };
  auto importName = [](DefinedImportData *imp) {
    return imp->getName().drop_front(strlen("__imp_"));
  };
  // A chunk with residual words is writable until the fill seals it.
  auto cellReadOnly = [&](SectionChunk *sc) {
    return !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
           !sealedChunks.contains(sc);
  };
  for (auto &[sc, slots] : ctx.importSlots) {
    auto *chunk = const_cast<SectionChunk *>(sc);
    for (const ImportSlot &slot : slots)
      addCell(chunk, slot.offset, slot.sym, nullptr, importName(slot.sym),
              cellReadOnly(chunk),
              getPartialSectionName(chunk, config.mingw).starts_with(".tls"));
  }
  // An import whose entry is one of its slots is covered by that slot.
  for (DefinedImportData *imp : idata.imports)
    if (!idata.slotOnly.contains(imp))
      addCell(nullptr, 0, imp, nullptr, importName(imp), true, false);

  // Each preemptible entity gets a pointer, which the offer table names; a
  // thread-local variable's instance is its record.
  symtab.isPreemptibleVagueEntity(nullptr); // which computes the set
  if (!symtab.preemptibleVagueEntities->empty()) {
    DenseSet<Defined *> pointed;
    for (Chunk *c : symtab.localImportChunks)
      if (auto *li = cast<LocalImportChunk>(c); !li->vagueEntity.empty())
        pointed.insert(li->getTarget());
    std::vector<Defined *> entities;
    for (Symbol *s : *symtab.preemptibleVagueEntities) {
      auto *d = dyn_cast<DefinedRegular>(s);
      if (!d || !d->isLive())
        continue;
      Defined *instance = d;
      if (symtab.isThreadLocal(d)) {
        instance = symtab.getThreadLocalRecord(d);
        if (!instance)
          continue;
        symtab.keepThreadLocalRecord(instance);
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
      symtab.localImportChunks.push_back(li);
    }
  }
  for (Chunk *c : symtab.localImportChunks) {
    auto *li = cast<LocalImportChunk>(c);
    if (!li->overrideDefault.empty())
      addCell(li, 0, nullptr, nullptr, li->overrideDefault, true, false);
    else if (!li->vagueEntity.empty())
      cells.push_back({li, 0, nullptr, 0,
                       overrideNameKey(li->getTarget()->getName()), true,
                       false, /*indirect=*/true, li->getTarget()->getName()});
  }
  // The words of static data the scan found, in chunks still in the image.
  if (slotScan)
    for (auto [sc, offset, d] : slotScan->ownWords)
      if (sc->live)
        addCell(sc, offset, nullptr, d, d->getName(), cellReadOnly(sc),
                getPartialSectionName(sc, config.mingw).starts_with(".tls"));
  if (cells.empty())
    return;

  auto fail = [&](const Twine &why) {
    if (DefinedImportData *imp = cells.front().imp)
      Err(ctx) << "the image reaches " << symtab.printSymbol(imp)
               << ", an overridable default imported from "
               << imp->getDLLName() << ", through a cell " << why;
    else
      Err(ctx) << "the image reaches its own overridable default through a "
                  "cell "
               << why;
  };
  // The anchor's mask covers 32 slots.
  if (llvm::any_of(cells,
                   [](const OverrideCell &c) { return !c.key && c.slot >= 32; }))
    return fail("that the override table's mask cannot name");
  if (!isa_and_nonnull<Defined>(symtab.findUnderscore("__xi_a")))
    return fail("that the image has no C initializer table (__xi_a) to fill");
  fillList = make<FillListChunk>(ctx, std::move(cells));
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
  std::vector<uint64_t> delayLoads;
  if (!config.dll) {
    // The executable offers the vague entities it exports, but no type_info,
    // which such images compare by name. A thread-local variable's instance
    // is its record.
    DenseSet<Symbol *> vague;
    vague.insert_range(ctx.symtab.weakData);
    vague.insert_range(ctx.symtab.weakFunctions);
    for (const Export &e : ctx.symtab.exports) {
      auto *d = dyn_cast_or_null<DefinedRegular>(e.sym);
      if (DefinedRegular *var = ctx.symtab.getRecordVariable(e.sym))
        d = var;
      if (d && d->isLive() && vague.contains(d) &&
          isShareableVagueName(d->getName()) && !isTypeInfoName(d->getName()))
        offer(e.exportName, nullptr, cast<Defined>(e.sym));
    }
  } else {
    // A DLL offers the pointers of its exported preemptible entities, and
    // lists the DLLs it delay-loads.
    for (Chunk *c : ctx.symtab.localImportChunks)
      if (auto *li = cast<LocalImportChunk>(c); !li->vagueEntity.empty())
        offer(li->vagueEntity, li, nullptr);
    for (StringRef dll : config.delayLoads.keys())
      delayLoads.push_back(overrideNameKey(dll));
    llvm::sort(delayLoads);
  }
  if (!offers.empty() || !delayLoads.empty())
    vagueOfferTable = make<VagueOfferTableChunk>(
        std::move(offers), std::move(delayLoads), /*direct=*/!config.dll);
  if (auto *list = cast_or_null<FillListChunk>(fillList); list && config.dll) {
    list->exports = std::move(exports);
    list->hintOf = &idata.hintOf;
  }
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
  if (!list)
    fillList = list = make<FillListChunk>(ctx, std::vector<OverrideCell>());
  list->runTimeCells = runTimeCells;
  list->words = residualWords;
  list->callsCopy = callsCopy;
  list->sealed = !runTimeCells.empty()   ? runTimeCells.front().cell
                 : !sealedChunks.empty() ? sealedChunks.front()
                                         : nullptr;
  list->selfKey =
      overrideNameKey(sys::path::filename(config.outputFile).lower());
  // The fill calls ntdll.
  for (Chunk *c : cast<ObjFile>(f->getFile())->getChunks())
    if (auto *sc = dyn_cast<SectionChunk>(c))
      for (Symbol *sym : sc->symbols())
        if (auto *imp = dyn_cast_or_null<DefinedImportData>(sym);
            imp && !imp->file->live)
          addImport(imp);
  fill = useBuiltin(ctx, "__llvm_fill", "__llvm_fill_list", list);
  fillPointer = make<LocalImportChunk>(ctx, fill);
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
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xl_a")))
    return fail("the image has no TLS callback array (__xl_a) to run it");
  auto *list = make<CopyListChunk>(std::move(tlsSlots));
  threadLocalSlotCopy = useBuiltin(ctx, "__llvm_thread_local_slot_copy",
                                   "__llvm_thread_local_slot_copy_list", list);
  if (!threadLocalSlotCopy)
    return fail("the compiler-rt builtins, which define the copy "
                "(__llvm_thread_local_slot_copy), are not linked");
  threadLocalSlotCopyPointer = make<LocalImportChunk>(ctx, threadLocalSlotCopy);
  threadLocalSlotCopyList = list;
}

// The loader makes only the import address table directory writable while it
// binds, and restores one protection over it: a slot must be outside code, and
// a read-only one inside the directory, which only read-only data may share;
// so must the pages the fill makes writable.
void ImportSlotContents::check(Chunk *iatStart, uint64_t iatSize) {
  llvm::TimeTraceScope timeScope("Check import slots");
  auto requireReadOnly = [&](uint64_t start, uint64_t end, StringRef what) {
    uint64_t pageBegin = alignDown(start, ctx.config.align);
    uint64_t pageLimit = alignTo(end, ctx.config.align);
    for (OutputSection *os : ctx.outputSections)
      if (os->getRVA() < pageLimit &&
          os->getRVA() + os->getVirtualSize() > pageBegin &&
          (os->header.Characteristics &
           (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) !=
              IMAGE_SCN_MEM_READ)
        Err(ctx) << "section " << os->name << " shares a page with " << what
                 << ", but is not read-only data";
  };
  if (auto *list = cast_or_null<FillListChunk>(fillList)) {
    SmallVector<std::pair<uint64_t, uint64_t>, 0> ranges =
        list->getReadOnlyRanges();
    if (ranges.size() > list->maxRanges)
      Err(ctx) << "read-only cells lie in more output sections than their "
                  "input sections name";
    for (auto [start, end] : ranges)
      requireReadOnly(start, end,
                      "read-only cells, which the image's fill makes writable");
  }
  if (ctx.importSlots.empty())
    return;

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
  requireReadOnly(iatBegin, iatLimit,
                  "the import address table, which holds in-place import "
                  "slots");
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

// Binds the uses that an object fuses with its loads of pointers, once the
// pointers are bound. A use whose pointer leads to a definition in the image
// reaches it directly: a call becomes a direct call, and an access or add
// reaches the data where the code may be resized, the add's sum going to the
// load. An access that only reads immutable data reads the image's copy. A
// call through a delay-loaded import's pointer reads the pointer, which holds
// the loader's stub until the first call; any other call stays through its
// register. A load no use keeps is dead. On ARM64, writeFusedArm64 writes the
// direct forms once addresses are known.
static void bindFusedUses(SectionChunk *sc,
                          MutableArrayRef<ObjFile::LinkFusion> fusions) {
  ObjFile *file = sc->file;
  SymbolTable &symtab = file->symtab;
  bool arm64 = symtab.machine == ARM64;
  uint16_t loadType =
      arm64 ? IMAGE_REL_ARM64_PAGEOFFSET_12L : IMAGE_REL_AMD64_REL32;
  ArrayRef<uint8_t> data = sc->getContents();
  bool covered = file->isCovered(sc);
  DenseSet<uint32_t> keptLoads;
  for (ObjFile::LinkFusion &f : fusions) {
    const coff_relocation *rel;
    if (f.isLoad || (arm64 && f.use == ObjFile::LinkFusion::Call) ||
        !(rel = findReloc(sc, f.load, loadType)))
      continue;
    Symbol *s = file->getSymbol(rel->SymbolTableIndex);
    if (!s || (arm64 ? !isArm64PointerLoad(sc, *rel)
                     : !isLoadForm(getDescribedSite(sc, *rel)))) {
      reportMismatch(sc, f.load);
      continue;
    }
    auto *li = dyn_cast<DefinedLocalImport>(s);
    bool reaches = li && !isZeroPointer(li) &&
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
    if (f.use == ObjFile::LinkFusion::Access) {
      if (!covered || !reaches)
        keptLoads.insert(f.load);
      unsigned disp = f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0;
      if (covered && (reaches || immutable))
        addEdit(sc, {f.offset, f.length, uint16_t(f.length - disp - f.sib + 4),
                     RelaxEdit::Access,
                     li ? li->getTarget() : symtab.immutableReads.lookup(s),
                     &f});
    } else if (f.use == ObjFile::LinkFusion::Add) {
      // The add is add or sub of an 8- or 32-bit immediate, or lea of a base
      // and an 8- or 32-bit displacement, with or without a SIB byte.
      RelaxEdit *load = findEdit(sc, f.load);
      if (reaches && load && load->kind == RelaxEdit::LoadAddress) {
        uint8_t op = data[f.offset + 1], modrm = data[f.offset + 2];
        unsigned length = op == 0x83   ? 4
                          : op == 0x81 ? 7
                                       : 3 + ((modrm & 7) == 4) +
                                             (modrm >> 6 == 1 ? 1 : 4);
        load->entry = &f;
        addEdit(sc, {f.offset, uint16_t(length), 0, RelaxEdit::Dispose, nullptr,
                     &f});
      }
    } else if (auto *imp = dyn_cast<DefinedImportData>(s);
               reaches || (imp && imp->file->isDelayLoaded())) {
      uint16_t size = f.isJump ? 7 : 6;
      addEdit(sc,
              reaches ? RelaxEdit(f.offset, size, 5,
                                  f.isJump ? RelaxEdit::DirectJump
                                           : RelaxEdit::DirectCall,
                                  li->getTarget())
                      : RelaxEdit(f.offset, size, size,
                                  f.isJump ? RelaxEdit::PointerJump
                                           : RelaxEdit::PointerCall,
                                  imp));
    } else {
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
  }
  for (ObjFile::LinkFusion &f : fusions) {
    const coff_relocation *rel;
    if (!f.isLoad || keptLoads.contains(f.offset) ||
        !(rel = findReloc(sc, f.offset, loadType)))
      continue;
    f.dead = true;
    if (arm64)
      continue;
    uint16_t size = getDescribedSite(sc, *rel) == LinkSiteLoadREX2 ? 8 : 7;
    if (RelaxEdit *e = findEdit(sc, f.offset)) {
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
      // Where the data's page offset plus the constant fits, the load, which
      // the pointer's bypass made an add of the offset, adds the sum into the
      // add's register, and the add becomes a no-op.
      int64_t sum = (getData(f.load)->getRVA() & 0xFFF) + f.addend;
      if (sum >= 0 && sum <= 0xFFF) {
        uint32_t load = read32le(buf + f.load);
        write32le(buf + f.load,
                  0x91000000 | uint32_t(sum) << 10 | (load & 0x3E0) | f.reg);
        write32le(buf + f.offset, nop);
      }
    } else if (Defined *copy =
                   f.use == ObjFile::LinkFusion::Access && f.valueRead
                       ? sc->file->symtab.immutableReads.lookup(
                             getSymbol(f.load))
                       : nullptr) {
      // An access that only reads immutable data, whose load stays, reads the
      // image's copy PC-relative where it is near enough and aligned.
      Ldr ldr;
      if (!parseLdr(read32le(buf + f.offset), ldr))
        continue;
      ldr.offset += int64_t(copy->getRVA()) - int64_t(sc->getRVA() + f.offset);
      if (isLiteralLdrEligible(ldr))
        writeLiteralLdr(buf + f.offset, ldr);
    }
  }
  // A dead load whose accesses all take the data's page offset in their
  // fields becomes a no-op, its register keeping the adrp's page.
  for (const ObjFile::LinkFusion &l : fusions) {
    auto it = accessesOf.find(l.offset);
    if (!l.isLoad || !l.dead || it == accessesOf.end())
      continue;
    uint64_t lo12 = getData(l.offset)->getRVA() & 0xFFF;
    auto fits = [&](const ObjFile::LinkFusion *f) {
      uint32_t insn = read32le(buf + f->offset);
      return Arm64Access(insn).fits(lo12 + Arm64Access(insn).getOffset(insn));
    };
    if (!llvm::all_of(it->second, fits))
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

// The one scan of the image's relocations, as lld/ELF's scanRelocations is.
// It decides which local import pointers a reference still reads: on x86-64 a
// described call, jump or load is rewritten per reference, on ARM64 every adrp
// and 64-bit ldr of a pointer per symbol, since an adrp may serve several
// loads; other references, GC roots and objects that describe nothing read the
// pointer, and under -import-slots a data reference reads it but blocks no
// rewrite. Under -import-slots it also gathers, for bind, the references to
// imports and offered functions.
void ImportSlotContents::scan(SymbolTable &symtab) {
  std::vector<Chunk *> &localImportChunks = symtab.localImportChunks;
  bool arm64 = symtab.machine == ARM64 && !ctx.hybridSymtab;
  bool locals = !localImportChunks.empty() &&
                (arm64 || llvm::any_of(ctx.objFileInstances, [&](ObjFile *f) {
                   return &f->symtab == &symtab && f->describesSites();
                 }));
  bool slots = ctx.config.importSlots && &symtab == &ctx.symtab;
  if (!locals && !slots)
    return;
  llvm::TimeTraceScope timeScope("Scan references");
  if (slots) {
    // An export of data resolved to its import would publish its entry.
    for (Export &e : ctx.symtab.exports)
      if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym);
          imp && imp->isRuntimePseudoReloc)
        Err(ctx) << "cannot export " << ctx.symtab.printSymbol(imp)
                 << ": it is imported from " << imp->getDLLName()
                 << "; export a forwarder to it instead";
    addRunTimeCells();
    slotScan = make<SlotScan>();
    if (ctx.config.dll) {
      symtab.readOverrideRecords();
      symtab.isPreemptibleVagueEntity(nullptr); // which computes the set
      slotScan->collectsOwnWords = !symtab.overrideSlotFacts.empty() ||
                                   !symtab.overridablesByName.empty() ||
                                   !symtab.preemptibleVagueEntities->empty();
    }
  }
  for (Symbol *b : ctx.config.gcroot)
    if (auto *li = dyn_cast<DefinedLocalImport>(b))
      li->getChunk()->readByCode = true;
  // The references to warn about if they still read the pointer.
  SmallVector<std::tuple<ObjFile *, DefinedLocalImport *, bool>, 0> refs;
  SmallVector<std::pair<SectionChunk *, const coff_relocation *>, 0>
      arm64Sites;

  // Without mark-live, a compiler pointer stays if another name reaches its
  // section: any symbol in it a relocation names, or an external one.
  DenseMap<SectionChunk *, LocalImportChunk *> cellOf;
  if (locals && !ctx.config.doGC)
    for (Chunk *c : localImportChunks)
      if (SectionChunk *cell = cast<LocalImportChunk>(c)->cell)
        cellOf.try_emplace(cell, cast<LocalImportChunk>(c));
  SmallPtrSet<LocalImportChunk *, 4> named;
  auto findCell = [&](ObjFile *file, Symbol *s) -> LocalImportChunk * {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    LocalImportChunk *c =
        d && !cellOf.empty() ? cellOf.lookup(d->getChunk()) : nullptr;
    return c && c->cell->file == file ? c : nullptr;
  };

  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &symtab)
      continue;
    if (slots) {
      slotScan->reported.clear();
      slotScan->guardFids.reset();
    }
    // An object that describes nothing reads every pointer it names; it is
    // scanned only for a compiler pointer it names, which may stay.
    bool describes = file->describesSites() || arm64;
    bool localFile =
        locals && llvm::any_of(file->getSymbols(), [&](Symbol *s) {
          auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
          return li && (describes || li->getChunk()->cell);
        });
    for (Symbol *s : file->getSymbols()) {
      if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
          locals && !describes && !localFile && li)
        li->getChunk()->readByCode = true;
      if (LocalImportChunk *c = localFile ? findCell(file, s) : nullptr;
          c && cast<DefinedRegular>(s)->getCOFFSymbol().isExternal())
        named.insert(c);
    }

    MapVector<DefinedLocalImport *, bool> seen;
    for (Chunk *chunk : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(chunk);
      bool slotChunk = slots && sc && sc->live &&
                       !(sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE);
      if (!sc || !sc->live || (!localFile && !slotChunk))
        continue;
      bool fromData = ctx.config.importSlots &&
                      !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE);
      ArrayRef<coff_relocation> relocs = sc->getRelocs();
      SlotScan::Pending p{sc, {}};
      for (size_t i = 0; i != relocs.size(); ++i) {
        const coff_relocation &rel = relocs[i];
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        auto *li = dyn_cast_or_null<DefinedLocalImport>(s);
        if (!li) {
          if (LocalImportChunk *c = localFile ? findCell(file, s) : nullptr)
            named.insert(c);
          if (slotChunk)
            scanReference(*slotScan, sc, i, p);
          continue;
        }
        if (!localFile)
          continue;
        LocalImportChunk *c = li->getChunk();
        if (fromData || !describes) {
          (fromData ? c->readByData : c->readByCode) = true;
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
            reportMismatch(sc, rel.VirtualAddress);
          else if (!isZeroPointer(li) || *form == LinkSiteCall ||
                   isLoadForm(form))
            rewritable = true;
          if (rewritable)
            addEdit(sc, getRetarget(sc, rel, *form, li->getTarget(),
                                    isZeroPointer(li)));
        }
        (rewritable ? c->rewritten : c->readByCode) = true;
        auto [it, inserted] = seen.try_emplace(li, rewritable);
        it->second &= rewritable;
      }
      if (!p.slots.empty())
        slotScan->pending.push_back(std::move(p));
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
  if (!locals)
    return;

  // A compiler pointer stays if something live reads it other than through a
  // rewritten instruction, or another name reaches its section. A pointer
  // every instruction bypasses is left out unless data reads it.
  for (Chunk *chunk : localImportChunks) {
    auto *c = cast<LocalImportChunk>(chunk);
    if (c->cell && !ctx.config.doGC)
      c->cell->live = named.contains(c);
    if (c->cell && c->live && (c->readByCode || c->readByData))
      c->cell->live = true;
    if (c->rewritten && !c->readByCode) {
      c->bypassed = true;
      c->live &= c->readByData;
    }
  }
  // The adrp and ldr of a bypassed pointer become adrp and add of its symbol,
  // or movz of zero.
  for (auto [sc, rel] : arm64Sites) {
    auto *li =
        cast<DefinedLocalImport>(sc->file->getSymbol(rel->SymbolTableIndex));
    if (li->getChunk()->bypassed)
      addEdit(sc, getRetarget(sc, *rel, LinkSiteOther, li->getTarget(),
                              isZeroPointer(li)));
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
