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
#include "LocalImports.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;

namespace lld::coff {
namespace {
// Under -import-slots, the image's residual fill: an initializer, which the C
// initializer table runs before any other, that writes each residual word
// from its import's entry. When read-only words are among them, they are in a
// writable section of their own, which the function then makes read-only with
// NtProtectVirtualMemory; that call gives it a frame, which ResidualFillUnwind
// describes. It returns that call's status, so that a failed seal fails the
// image's start-up, as any nonzero return from the table does; without the
// call it returns 0.
class ResidualFillChunk : public NonSectionCodeChunk {
public:
  ResidualFillChunk(COFFLinkerContext &ctx, std::vector<ResidualWord> words,
                    Chunk *sealed, DefinedImportData *protect);
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override;
  // The size of the prologue, which sets up the frame, if there is one.
  size_t getPrologueSize() const;

private:
  size_t getWordSize(const ResidualWord &w) const;

  std::vector<ResidualWord> words;
  // A chunk of the section the function seals, or null.
  Chunk *sealed;
  // NtProtectVirtualMemory's import, when it seals.
  DefinedImportData *protect;
  COFFLinkerContext &ctx;
};

// The unwind information of the residual fill when it has a frame.
class ResidualFillUnwindChunk : public NonSectionChunk {
public:
  ResidualFillUnwindChunk(COFFLinkerContext &ctx, ResidualFillChunk *fill)
      : fill(fill), ctx(ctx) {
    setAlignment(4);
  }
  size_t getSize() const override { return 8; }
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }

private:
  ResidualFillChunk *fill;
  COFFLinkerContext &ctx;
};

// The exception table entry of the residual fill when it has a frame.
class ResidualFillPdataChunk : public NonSectionChunk {
public:
  ResidualFillPdataChunk(COFFLinkerContext &ctx, ResidualFillChunk *fill,
                         ResidualFillUnwindChunk *unwind)
      : fill(fill), unwind(unwind), ctx(ctx) {
    setAlignment(4);
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }

private:
  ResidualFillChunk *fill;
  ResidualFillUnwindChunk *unwind;
  COFFLinkerContext &ctx;
};
ResidualFillChunk::ResidualFillChunk(COFFLinkerContext &ctx,
                                     std::vector<ResidualWord> words,
                                     Chunk *sealed, DefinedImportData *protect)
    : words(std::move(words)), sealed(sealed), protect(protect), ctx(ctx) {
  // Control Flow Guard checks the initializer table's call to it.
  setAlignment(16);
}

MachineTypes ResidualFillChunk::getMachine() const {
  return ctx.config.machine;
}

size_t ResidualFillChunk::getPrologueSize() const {
  if (!protect)
    return 0;
  return ctx.config.machine == ARM64 ? 8 : 4;
}

// The instructions that add an addend to the import's address.
size_t getArm64AddendSize(int64_t addend) {
  if (addend > -4096 && addend < 4096)
    return 4;
  size_t n = 0;
  for (int shift = 0; shift != 64; shift += 16)
    n += (uint64_t(addend) >> shift & 0xFFFF) != 0;
  return 4 * std::max<size_t>(n, 1) + 4;
}

size_t ResidualFillChunk::getWordSize(const ResidualWord &w) const {
  if (ctx.config.machine == ARM64)
    return 8 + getArm64AddendSize(w.addend) + 12;
  return 7 + (isInt<32>(w.addend) ? 6 : 13) + 7;
}

size_t ResidualFillChunk::getSize() const {
  size_t size = getPrologueSize();
  for (const ResidualWord &w : words)
    size += getWordSize(w);
  if (ctx.config.machine == ARM64)
    return size + (protect ? 56 + 8 : 4) + 4;
  return size + (protect ? 60 + 4 : 2) + 1;
}

void ResidualFillChunk::writeTo(uint8_t *buf) const {
  const uint32_t pageReadOnly = 2;
  uint64_t sealStart = 0, sealSize = 0;
  if (protect) {
    OutputSection *os = ctx.getOutputSection(sealed);
    sealStart = os->getRVA();
    sealSize = os->getVirtualSize();
  }
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    // adrp xN, target; ldr xN, [xN, :lo12:target]
    auto load = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0xF9400000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Ldr(buf + off + 4, target & 0xfff);
      off += 8;
    };
    // adrp xN, target; add xN, xN, :lo12:target
    auto address = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0x91000000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
      off += 8;
    };
    if (protect) {
      put(0xA9BD7BFD); // stp x29, x30, [sp, #-48]!
      put(0x910003FD); // mov x29, sp
    }
    for (const ResidualWord &w : words) {
      load(16, w.imp->getRVA());
      if (w.addend > 0 && w.addend < 4096) {
        put(0x91000210 | uint32_t(w.addend) << 10); // add x16, x16, #addend
      } else if (w.addend < 0 && w.addend > -4096) {
        put(0xD1000210 | uint32_t(-w.addend) << 10); // sub x16, x16, #-addend
      } else {
        // movz x17, #h0; movk x17, #hN, lsl #16N; add x16, x16, x17
        bool first = true;
        for (uint32_t hw = 0; hw != 4; ++hw) {
          uint32_t half = uint64_t(w.addend) >> (16 * hw) & 0xFFFF;
          if (!half && !(first && hw == 3))
            continue;
          put((first ? 0xD2800011 : 0xF2800011) | hw << 21 | half << 5);
          first = false;
        }
        put(0x8B110210);
      }
      address(17, w.chunk->getRVA() + w.offset);
      put(0xF9000230); // str x16, [x17]
    }
    if (protect) {
      // NtProtectVirtualMemory(-1, &base, &size, PAGE_READONLY, &old), with
      // base, size and old at sp + 16, 24 and 32.
      address(0, sealStart);
      put(0xF9000BE0);                                    // str x0, [sp, #16]
      put(0xD2800001 | uint32_t(sealSize & 0xFFFF) << 5); // movz x1, #lo
      put(0xF2A00001 | uint32_t(sealSize >> 16 & 0xFFFF) << 5); // movk x1, #hi
      put(0xF9000FE1);                     // str x1, [sp, #24]
      put(0x92800000);                     // mov x0, #-1
      put(0x910043E1);                     // add x1, sp, #16
      put(0x910063E2);                     // add x2, sp, #24
      put(0x52800003 | pageReadOnly << 5); // mov w3, #PAGE_READONLY
      put(0x910083E4);                     // add x4, sp, #32
      load(16, protect->getRVA());
      put(0xD63F0200); // blr x16
      put(0x910003BF); // mov sp, x29
      put(0xA8C37BFD); // ldp x29, x30, [sp], #48
    } else {
      put(0x52800000); // mov w0, #0
    }
    put(0xD65F03C0); // ret
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  // The RIP-relative displacement to target of the instruction just put,
  // which ends with it.
  auto disp = [&](uint64_t target) {
    write32le(buf + off - 4, target - (rva + off));
  };
  if (protect)
    put({0x48, 0x83, 0xEC, 0x48}); // sub rsp, 0x48
  for (const ResidualWord &w : words) {
    put({0x48, 0x8B, 0x05, 0, 0, 0, 0}); // mov rax, [rip + imp]
    disp(w.imp->getRVA());
    if (isInt<32>(w.addend)) {
      put({0x48, 0x05, 0, 0, 0, 0}); // add rax, addend
      write32le(buf + off - 4, uint32_t(w.addend));
    } else {
      put({0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0}); // mov rcx, addend
      write64le(buf + off - 8, uint64_t(w.addend));
      put({0x48, 0x01, 0xC8}); // add rax, rcx
    }
    put({0x48, 0x89, 0x05, 0, 0, 0, 0}); // mov [rip + word], rax
    disp(w.chunk->getRVA() + w.offset);
  }
  if (protect) {
    // NtProtectVirtualMemory(-1, &base, &size, PAGE_READONLY, &old), with
    // base, size and old at rsp + 0x30, 0x38 and 0x40.
    put({0x48, 0x8D, 0x05, 0, 0, 0, 0}); // lea rax, [rip + base]
    disp(sealStart);
    put({0x48, 0x89, 0x44, 0x24, 0x30});             // mov [rsp + 0x30], rax
    put({0x48, 0xC7, 0x44, 0x24, 0x38, 0, 0, 0, 0}); // mov [rsp + 0x38], size
    write32le(buf + off - 4, uint32_t(sealSize));
    put({0x48, 0x8D, 0x44, 0x24, 0x40});               // lea rax, [rsp + 0x40]
    put({0x48, 0x89, 0x44, 0x24, 0x20});               // mov [rsp + 0x20], rax
    put({0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF});   // mov rcx, -1
    put({0x48, 0x8D, 0x54, 0x24, 0x30});               // lea rdx, [rsp + 0x30]
    put({0x4C, 0x8D, 0x44, 0x24, 0x38});               // lea r8, [rsp + 0x38]
    put({0x41, 0xB9, uint8_t(pageReadOnly), 0, 0, 0}); // mov r9d, PAGE_READONLY
    put({0xFF, 0x15, 0, 0, 0, 0}); // call [rip + NtProtectVirtualMemory]
    disp(protect->getRVA());
    put({0x48, 0x83, 0xC4, 0x48}); // add rsp, 0x48
  } else {
    put({0x31, 0xC0}); // xor eax, eax
  }
  put({0xC3}); // ret
}

void ResidualFillUnwindChunk::writeTo(uint8_t *buf) const {
  if (ctx.config.machine == ARM64) {
    // One epilog at the end, which the prologue's codes describe: set_fp,
    // save_fplr_x of 48 bytes, end.
    write32le(buf, fill->getSize() / 4 | 1 << 21 | 1 << 27);
    const uint8_t codes[] = {0xE1, 0x85, 0xE4, 0xE4};
    memcpy(buf + 4, codes, sizeof(codes));
    return;
  }
  // Version 1, a prologue of 4 bytes with one code: UWOP_ALLOC_SMALL of
  // 0x48 bytes at its end.
  const uint8_t info[] = {0x01, 0x04, 0x01, 0x00, 0x04, 0x82, 0x00, 0x00};
  memcpy(buf, info, sizeof(info));
}

size_t ResidualFillPdataChunk::getSize() const {
  return ctx.config.machine == ARM64 ? 8 : 12;
}

void ResidualFillPdataChunk::writeTo(uint8_t *buf) const {
  write32le(buf, fill->getRVA());
  if (ctx.config.machine == ARM64) {
    write32le(buf + 4, unwind->getRVA());
    return;
  }
  write32le(buf + 4, fill->getRVA() + fill->getSize());
  write32le(buf + 8, unwind->getRVA());
}
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

std::optional<int64_t> getAddressWordAddend(SectionChunk *sc,
                                            const coff_relocation &rel) {
  const Configuration &config = sc->file->symtab.ctx.config;
  if (uint64_t(rel.VirtualAddress) + config.wordsize > sc->getSize())
    return std::nullopt;
  const uint8_t *word = sc->getContents().data() + rel.VirtualAddress;
  return config.is64() ? int64_t(read64le(word)) : int32_t(read32le(word));
}

static bool isDelayLoaded(COFFLinkerContext &ctx, DefinedImportData *imp) {
  return ctx.config.delayLoads.contains(imp->getDLLName().lower());
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
  return false;
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
  // An instruction that is rewritten unless the import's thunk is its
  // address.
  struct Site {
    SectionChunk *chunk;
    uint32_t offset;
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
            std::vector<std::pair<SectionChunk *, uint32_t>>>
      delayLoads;
  DenseSet<DefinedImportData *> delayLoadsRead;
  // For each import thunk, the references to it, and those that a slot or a
  // rewritten instruction bypasses.
  MapVector<DefinedImportThunk *, std::pair<size_t, size_t>> thunkRefs;
  DenseMap<ImportFile *, Use> uses;
  // The symbols of the object being scanned that have been reported.
  SmallPtrSet<Symbol *, 4> reported;
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
  if (!config.importSlots || ctx.hybridSymtab || isArm64EC(config.machine))
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

  SlotScan scan;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab)
      continue;
    scan.reported.clear();
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (sc && sc->live &&
          !(sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE))
        scanChunk(scan, sc);
    }
  }
  resolve(scan);
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
  else
    Warn(ctx) << file << ": may take the address of "
              << file->symtab.printSymbol(s) << ", imported from "
              << imp->getDLLName()
              << ", in an instruction it does not describe, so the image "
                 "uses its import thunk as its address; declare it "
                 "imported, or rebuild the object with clang";
}

// A word that holds an address inside an import's data for which its DLL
// exports no name is the residual fill's to write. A read-only one moves with
// its chunk to pages that the fill makes read-only again once written, which a
// read-only section that may not move cannot give it. Thread-local data is the
// loader's to copy before any initializer runs.
void ImportSlotContents::addResidualWord(SectionChunk *sc, uint32_t offset,
                                         Symbol *s, DefinedImportData *imp,
                                         int64_t addend) {
  Configuration &config = ctx.config;
  StringRef name = getPartialSectionName(sc, config.mingw);
  bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
  StringRef why;
  if (name.starts_with(".tls")) {
    why = ", in thread-local data";
  } else if (readOnly && name != ".rdata") {
    why = ", in a read-only section that cannot move";
  } else if (config.machine == AMD64 || config.machine == ARM64) {
    if (readOnly)
      sealedChunks.insert(sc);
    residualWords.push_back({sc, offset, imp, addend, s});
    return;
  }
  Err(ctx) << sc->file << ": " << sc->getSectionName()
           << " holds the address of " << sc->file->symtab.printSymbol(s)
           << " plus " << addend << ", imported from " << imp->getDLLName()
           << ", which exports no name for it" << why;
}

void ImportSlotContents::scanChunk(SlotScan &scan, SectionChunk *sc) {
  Configuration &config = ctx.config;
  ObjFile *file = sc->file;
  bool arm64 = config.machine == ARM64;
  bool code = sc->header->Characteristics & IMAGE_SCN_CNT_CODE;
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  SlotScan::Pending p{sc, {}, nullptr, {}};
  for (size_t i = 0, e = relocs.size(); i != e; ++i) {
    const coff_relocation &rel = relocs[i];
    Symbol *s = file->getSymbol(rel.SymbolTableIndex);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s);
    auto *data = dyn_cast_or_null<DefinedImportData>(s);
    if (thunk)
      ++scan.thunkRefs[thunk].first;

    // An import address table entry. The import's data itself is reached only
    // through its slots.
    if (data && !data->isRuntimePseudoReloc) {
      scan.use(data, readsWholeEntry(sc, rel, code, arm64) ? SlotScan::Read
                                                           : SlotScan::Other);
      // That of a delay-loaded function: a load of the address in code takes
      // the thunk's instead.
      if (!data->file->thunkSym || !isDelayLoaded(ctx, data))
        continue;
      if (!code)
        scan.delayLoadsRead.insert(data);
      else if (arm64 && isArm64PointerLoad(sc, rel))
        scan.delayLoads[data].push_back({sc, rel.VirtualAddress});
      else if (arm64)
        scan.delayLoadsRead.insert(data);
      else if (std::optional<LinkSiteForm> form = getDescribedSite(sc, rel);
               form == LinkSiteLoad || form == LinkSiteLoadREX2)
        scan.addressSites.push_back({sc, rel.VirtualAddress, data});
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
        if (scan.reported.insert(s).second)
          Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                   << " is imported from " << imp->getDLLName()
                   << ", but the object was compiled as if it were "
                      "local; mark its declaration, or compile the object "
                      "with -fauto-import";
        continue;
      }
      if (sc->isAddressWord(rel)) {
        Err(ctx) << file << ": " << sc->getSectionName()
                 << " is executable and holds the address of "
                 << file->symtab.printSymbol(s) << ", imported from "
                 << imp->getDLLName();
        continue;
      }
      if (isDelayLoaded(ctx, imp))
        continue;
      if (arm64) {
        switch (rel.Type) {
        case IMAGE_REL_ARM64_PAGEBASE_REL21:
          if (i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
            scan.addressSites.push_back({sc, rel.VirtualAddress, imp});
            scan.addressSites.push_back(
                {sc, relocs[i + 1].VirtualAddress, imp});
            ++scan.thunkRefs[thunk].first;
            scan.thunkRefs[thunk].second += 2;
            ++i;
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
          useThunkAsAddress(scan, file, s, imp);
          break;
        }
        std::optional<LinkSiteForm> form =
            file->getLinkSiteForm(sc, rel.VirtualAddress);
        if (form == LinkSiteAddress) {
          if (getDescribedSite(sc, rel) == LinkSiteAddress) {
            scan.addressSites.push_back({sc, rel.VirtualAddress, imp});
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
      bool callOnly = file->listsCallOnly()
                          ? file->isCallOnlyRef(sc, rel.VirtualAddress)
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
    if (isDelayLoaded(ctx, imp))
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
  for (const SlotScan::Site &site : scan.addressSites) {
    if (site.imp->file->thunkIsAddress)
      continue;
    ctx.importSites.insert({site.chunk, site.offset});
    site.chunk->hasImportSites = true;
    if (isDelayLoaded(ctx, site.imp))
      keepThunk(site.imp);
    else
      scan.use(site.imp, SlotScan::Read);
  }
  for (auto &[imp, sites] : scan.delayLoads) {
    if (scan.delayLoadsRead.contains(imp))
      continue;
    for (auto [sc, offset] : sites) {
      ctx.importSites.insert({sc, offset});
      sc->hasImportSites = true;
    }
    keepThunk(imp);
  }

  // An import thunk that every reference bypasses is left out of the image,
  // and so out of the Control Flow Guard tables, unless something other than
  // a relocation names it.
  DenseSet<Symbol *> named(config.gcroot.begin(), config.gcroot.end());
  for (Export &e : ctx.symtab.exports)
    named.insert(e.sym);
  for (auto &[thunk, refs] : scan.thunkRefs)
    if (refs.first == refs.second && !thunk->wrappedSym->file->thunkIsAddress &&
        !isDelayLoaded(ctx, thunk->wrappedSym) && !named.contains(thunk))
      thunk->getChunk()->live = false;

  std::vector<SectionChunk *> writable;
  for (SlotScan::Pending &p : scan.pending) {
    SectionChunk *sc = p.chunk;
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
  if (ctx.importSlots.empty())
    return;

  // The slot chunks laid out together are ordered by the DLL of their first
  // slot, so that single-pointer chunks of one DLL form one run.
  auto dllIndex = [&](SectionChunk *sc) {
    StringRef dll = ctx.importSlots[sc].front().sym->getDLLName();
    return config.dllOrder[dll.lower()];
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
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym))
      scan.use(imp, SlotScan::Other);
  for (Symbol *s : config.gcroot)
    if (auto *imp = dyn_cast<DefinedImportData>(s))
      scan.use(imp, SlotScan::Other);
  // The residual fill reads the address of each import it adds to.
  for (const ResidualWord &w : residualWords)
    scan.use(w.imp, SlotScan::Read);
  // So does a live import thunk.
  auto getUse = [&](ImportFile *file) {
    SlotScan::Use u = scan.uses.lookup(file);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(file->thunkSym);
    return thunk && thunk->getChunk()->live ? std::max(u, SlotScan::Read) : u;
  };
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

// Under -import-slots, the residual fill writes the words of static data that
// hold an address inside an import for which its DLL exports no name, as the
// loader would write the import's address. It runs from the image's C
// initializer table, ahead of every initializer, and makes the pages of the
// read-only words read-only once it has written them.
void ImportSlotContents::createResidualFill() {
  if (residualWords.empty())
    return;
  Configuration &config = ctx.config;
  const ResidualWord &first = residualWords.front();
  auto fail = [&](const Twine &why) {
    Err(ctx) << first.chunk->file << ": " << first.chunk->getSectionName()
             << " holds the address of "
             << first.chunk->file->symtab.printSymbol(first.sym) << " plus "
             << first.addend << ", imported from " << first.imp->getDLLName()
             << ", which exports no name for it, and " << why;
  };
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    return fail("the image has no C initializer table (__xi_a) to write it");

  DefinedImportData *protect = nullptr;
  if (!sealedChunks.empty()) {
    if (config.align % 4096)
      return fail("its section alignment is smaller than a page, which the "
                  "image cannot make read-only once written");
    protect = dyn_cast_or_null<DefinedImportData>(
        ctx.symtab.find("__imp_NtProtectVirtualMemory"));
    if (!protect)
      return fail("the image cannot make it read-only once written without "
                  "NtProtectVirtualMemory from ntdll.lib");
    if (config.delayLoads.contains(protect->getDLLName().lower()))
      return fail("NtProtectVirtualMemory, which makes it read-only once "
                  "written, is delay-loaded");
    if (!protect->file->live)
      addImport(protect);
  }

  auto *fill = make<ResidualFillChunk>(
      ctx, residualWords, sealedChunks.empty() ? nullptr : sealedChunks.front(),
      protect);
  residualFill = fill;
  residualFillPointer = make<LocalImportChunk>(
      ctx, make<DefinedSynthetic>("__llvm_residual_fill", fill));
  if (protect) {
    auto *unwind = make<ResidualFillUnwindChunk>(ctx, fill);
    residualFillUnwind = unwind;
    residualFillPdata = make<ResidualFillPdataChunk>(ctx, fill, unwind);
  }
}

// The loader writes an in-place import slot with an absolute address, makes
// only the import address table directory writable while it binds imports, and
// restores one protection over all of it. Each slot's word must therefore be
// in a section that is not executable, a read-only one inside the directory,
// whose page-rounded range has one protection; each run must still be one word
// a slot.
void ImportSlotContents::check(Chunk *iatStart, uint64_t iatSize) {
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
