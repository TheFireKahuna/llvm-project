//===- ICF.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ICF is short for Identical Code Folding. That is a size optimization to
// identify and merge two or more read-only sections (typically functions)
// that happened to have the same contents. It usually reduces output size
// by a few percent.
//
// On Windows, ICF is enabled by default.
//
// See ELF/ICF.cpp for the details about the algorithm.
//
//===----------------------------------------------------------------------===//

#include "ICF.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "Relocations.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/Timer.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <atomic>
#include <vector>

using namespace llvm;

namespace lld::coff {

class ICF {
public:
  ICF(COFFLinkerContext &c, bool unwind) : unwind(unwind), ctx(c) {}
  size_t run();

private:
  bool addUnwindRecord(SectionChunk *c);

  ArrayRef<uint8_t> getContents(const SectionChunk *c) const {
    return unwind ? ArrayRef(records.find(c)->second) : c->getContents();
  }

  template <bool constant> void segregate(size_t begin, size_t end);

  bool assocEquals(const SectionChunk *a, const SectionChunk *b);

  template <bool constant>
  bool sectionsEqual(const SectionChunk *a, const SectionChunk *b);

  bool isEligible(SectionChunk *c);

  size_t findBoundary(size_t begin, size_t end);

  void forEachClassRange(size_t begin, size_t end,
                         std::function<void(size_t, size_t)> fn);

  void forEachClass(std::function<void(size_t, size_t)> fn);

  std::vector<SectionChunk *> chunks;
  // Whether this pass folds the unwind records of resized code, after
  // relaxation, and each such record with its spans' values in its bytes. Two
  // functions' records name each its own function's labels, so they are equal
  // only in what their spans measure.
  bool unwind;
  llvm::DenseMap<const SectionChunk *, SmallVector<uint8_t, 0>> records;
  int cnt = 0;
  std::atomic<bool> repeat = {false};

  COFFLinkerContext &ctx;
};

// Returns true if section S is subject of ICF.
//
// Microsoft's documentation
// (https://msdn.microsoft.com/en-us/library/bxwfs976.aspx; visited April
// 2017) says that /opt:icf folds both functions and read-only data.
// Despite that, the MSVC linker folds only functions. We found
// a few instances of programs that are not safe for data merging.
// Therefore, we merge only functions just like the MSVC tool. However, we also
// merge read-only sections in a couple of cases where the address of the
// section is insignificant to the user program and the behaviour matches that
// of the Visual C++ linker.
bool ICF::isEligible(SectionChunk *c) {
  // Non-comdat chunks, dead chunks, writable chunks and chunks kept distinct
  // are not eligible.
  bool writable =
      c->getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_MEM_WRITE;
  if (!c->isCOMDAT() || !c->live || writable || c->keepDistinct)
    return false;

  // Under regular (not safe) ICF, all code sections are eligible.
  if ((ctx.config.doICF == ICFLevel::All) &&
      c->getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)
    return true;

  // So is a read-only section an /icfsection names, as in link.exe.
  if (ctx.config.icfSections.contains(c->getSectionName()))
    return true;

  // .pdata and .xdata unwind info sections are eligible.
  StringRef outSecName = c->getSectionName().split('$').first;
  if (outSecName == ".pdata" || outSecName == ".xdata")
    return true;

  // So are vtables.
  const char *itaniumVtablePrefix =
      ctx.config.machine == I386 ? "__ZTV" : "_ZTV";
  if (c->sym && (c->sym->getName().starts_with("??_7") ||
                 c->sym->getName().starts_with(itaniumVtablePrefix)))
    return true;

  // Anything else not in an address-significance table is eligible.
  return !c->keepUnique;
}

// Split an equivalence class into smaller classes.
template <bool constant> void ICF::segregate(size_t begin, size_t end) {
  while (begin < end) {
    // Divide [Begin, End) into two. Let Mid be the start index of the
    // second group.
    auto bound = std::stable_partition(
        chunks.begin() + begin + 1, chunks.begin() + end, [&](SectionChunk *s) {
          return sectionsEqual<constant>(chunks[begin], s);
        });
    size_t mid = bound - chunks.begin();

    // Split [Begin, End) into [Begin, Mid) and [Mid, End). We use Mid as an
    // equivalence class ID because every group ends with a unique index.
    for (size_t i = begin; i < mid; ++i)
      chunks[i]->eqClass[(cnt + 1) % 2] = mid;

    // If we created a group, we need to iterate the main loop again.
    if (mid != end)
      repeat = true;

    begin = mid;
  }
}

// Returns true if two sections' associative children, i.e. exception handling
// metadata such as .pdata and .xdata, are equal.
bool ICF::assocEquals(const SectionChunk *a, const SectionChunk *b) {
  // Ignore associated metadata sections that don't participate in ICF, such as
  // debug info and CFGuard metadata.
  auto considerForICF = [](const SectionChunk &assoc) {
    StringRef Name = assoc.getSectionName();
    return !(Name.starts_with(".debug") || Name == ".gfids$y" ||
             Name == ".giats$y" || Name == ".gljmp$y");
  };
  auto ra = make_filter_range(a->children(), considerForICF);
  auto rb = make_filter_range(b->children(), considerForICF);
  return std::equal(ra.begin(), ra.end(), rb.begin(), rb.end(),
                    [&](const SectionChunk &ia, const SectionChunk &ib) {
                      return ia.eqClass[cnt % 2] == ib.eqClass[cnt % 2];
                    });
}

// Compare the "non-moving" or "moving" parts of two sections.
template <bool constant>
bool ICF::sectionsEqual(const SectionChunk *a, const SectionChunk *b) {
  auto eqSym = [&](Symbol *b1, Symbol *b2) {
    if (b1 == b2)
      return true;
    // Only one is null where only one PAIR is a fork object's.
    auto *d1 = dyn_cast_or_null<DefinedRegular>(b1);
    auto *d2 = dyn_cast_or_null<DefinedRegular>(b2);
    if (!d1 || !d2)
      return false;
    if constexpr (constant)
      if (d1->getValue() != d2->getValue())
        return false;
    return d1->getChunk()->eqClass[cnt % 2] == d2->getChunk()->eqClass[cnt % 2];
  };

  auto eqReloc = [&](const coff_relocation &r1, const coff_relocation &r2) {
    // Fused sites bind only where the assembler checked them.
    if constexpr (constant)
      if (r1.Type != r2.Type || r1.VirtualAddress != r2.VirtualAddress ||
          (r1.Type == COFF::IMAGE_REL_LLVM_ALIGN &&
           r1.SymbolTableIndex != r2.SymbolTableIndex) ||
          (COFF::getLLVMRelocProperties(r1.Type) & COFF::LLVMReloc::Fused &&
           a->file->fusedChecked != b->file->fusedChecked))
        return false;
    return eqSym(a->getRelocTarget(r1), b->getRelocTarget(r2));
  };
  // The facts of a section's sites say how the link rewrites them.
  if constexpr (constant)
    if (a->file->getSiteFacts(a) != b->file->getSiteFacts(b))
      return false;
  // An unwind record's spans are in its bytes, as their values.
  auto relocs = [&](const SectionChunk *c) {
    return make_filter_range(c->getRelocs(), [=](const coff_relocation &r) {
      return !unwind || !(c->isPair(r.Type) || c->getPair(r));
    });
  };
  auto ra = relocs(a), rb = relocs(b);
  if (!std::equal(ra.begin(), ra.end(), rb.begin(), rb.end(), eqReloc))
    return false;

  auto eqSwitch = [&](const SwitchTable &s1, const SwitchTable &s2) {
    if constexpr (constant)
      if (s1.count != s2.count || s1.type != s2.type || s1.flags != s2.flags)
        return false;
    return eqSym(s1.branch, s2.branch) && eqSym(s1.table, s2.table) &&
           eqSym(s1.base, s2.base) && eqSym(s1.load, s2.load);
  };
  if (!llvm::equal(a->file->getSwitchTables(a), b->file->getSwitchTables(b),
                   eqSwitch))
    return false;

  // Spans are equal where they state the same of code that folds, or of the
  // two sections themselves. An unwind record's are in its bytes.
  auto eqSpans = [&](const SpanList &l1, const SpanList &l2) {
    if constexpr (constant)
      if (l1.spans != l2.spans)
        return false;
    return (l1.target == a && l2.target == b) ||
           l1.target->eqClass[cnt % 2] == l2.target->eqClass[cnt % 2];
  };
  if (!unwind &&
      !llvm::equal(a->file->getSpans(a), b->file->getSpans(b), eqSpans))
    return false;

  if constexpr (constant) {
    return a->getOutputCharacteristics() == b->getOutputCharacteristics() &&
           a->getSectionName() == b->getSectionName() &&
           a->header->SizeOfRawData == b->header->SizeOfRawData &&
           (unwind || a->checksum == b->checksum) &&
           getContents(a) == getContents(b) &&
           a->getMachine() == b->getMachine() && a->resizable == b->resizable &&
           a->guardSuppressed == b->guardSuppressed && assocEquals(a, b);
  } else {
    Symbol *e1 = a->getEntryThunk();
    Symbol *e2 = b->getEntryThunk();
    if ((e1 || e2) && (!e1 || !e2 || !eqSym(e1, e2)))
      return false;

    // Check associated children sections, i.e. exception handling data, for
    // equality.
    return assocEquals(a, b);
  }
}

// Find the first Chunk after Begin that has a different class from Begin.
size_t ICF::findBoundary(size_t begin, size_t end) {
  for (size_t i = begin + 1; i < end; ++i)
    if (chunks[begin]->eqClass[cnt % 2] != chunks[i]->eqClass[cnt % 2])
      return i;
  return end;
}

void ICF::forEachClassRange(size_t begin, size_t end,
                            std::function<void(size_t, size_t)> fn) {
  while (begin < end) {
    size_t mid = findBoundary(begin, end);
    fn(begin, mid);
    begin = mid;
  }
}

// Call Fn on each class group.
void ICF::forEachClass(std::function<void(size_t, size_t)> fn) {
  // If the number of sections are too small to use threading,
  // call Fn sequentially.
  if (chunks.size() < 1024) {
    forEachClassRange(0, chunks.size(), fn);
    ++cnt;
    return;
  }

  // Shard into non-overlapping intervals, and call Fn in parallel.
  // The sharding must be completed before any calls to Fn are made
  // so that Fn can modify the Chunks in its shard without causing data
  // races.
  const size_t numShards = 256;
  size_t step = chunks.size() / numShards;
  size_t boundaries[numShards + 1];
  boundaries[0] = 0;
  boundaries[numShards] = chunks.size();
  parallelFor(1, numShards, [&](size_t i) {
    boundaries[i] = findBoundary((i - 1) * step, chunks.size());
  });
  parallelFor(1, numShards + 1, [&](size_t i) {
    if (boundaries[i - 1] < boundaries[i]) {
      forEachClassRange(boundaries[i - 1], boundaries[i], fn);
    }
  });
  ++cnt;
}

// Keeps the bytes of c with its spans' values, where c is an unwind record
// each of whose spans has a value that fits its field.
bool ICF::addUnwindRecord(SectionChunk *c) {
  ArrayRef<coff_relocation> rels = c->getRelocs();
  ArrayRef<uint8_t> contents = c->getContents();
  if (!c->file->forkObject || !isEligible(c) ||
      c->getSectionName().split('$').first != ".xdata")
    return false;
  // A field is read as a word wherever it ends.
  SmallVector<uint8_t, 0> record(contents.size() + 4);
  memcpy(record.data(), contents.data(), contents.size());
  ScannedSection *s = c->getScanned();
  for (size_t i = 0, e = rels.size(); i != e; ++i) {
    if (!c->getPair(rels[i]))
      continue;
    auto *a = dyn_cast_or_null<DefinedRegular>(c->getTarget(i, s));
    auto *b = dyn_cast_or_null<DefinedRegular>(c->getTarget(i + 1, s));
    uint32_t offset = rels[i].VirtualAddress;
    bool aligned;
    if (!a || !b || !a->getChunk()->live || !b->getChunk()->live ||
        offset >= contents.size())
      return false;
    int64_t v = int64_t(a->getRVA()) - b->getRVA();
    if (!spanFits(rels[i].Type, v, contents.drop_front(offset), aligned) ||
        !aligned)
      return false;
    writeSpan(rels[i].Type, &record[offset], v, contents.drop_front(offset));
  }
  writeSpans(*c, record.data());
  records[c] = std::move(record);
  return true;
}

// Merge identical COMDAT sections.
// Two sections are considered the same if their section headers,
// contents and relocations are all the same.
size_t ICF::run() {
  llvm::TimeTraceScope timeScope("ICF");
  ScopedTimer t(ctx.icfTimer);

  // Collect only mergeable sections and group by hash value.
  uint32_t nextId = 1;
  for (Chunk *c : ctx.driver.getChunks()) {
    if (auto *sc = dyn_cast<SectionChunk>(c)) {
      if (unwind ? addUnwindRecord(sc) : isEligible(sc))
        chunks.push_back(sc);
      else
        sc->eqClass[0] = nextId++;
    }
  }

  // Make sure that ICF doesn't merge sections that are being handled by string
  // tail merging.
  for (MergeChunk *mc : ctx.mergeChunkInstances)
    if (mc)
      for (SectionChunk *sc : mc->sections)
        sc->eqClass[0] = nextId++;

  // Initially, we use hash values to partition sections.
  parallelForEach(chunks, [&](SectionChunk *sc) {
    sc->eqClass[0] = xxh3_64bits(getContents(sc));
  });

  // Combine the hashes of the sections referenced by each section into its
  // hash.
  for (unsigned cnt = 0; cnt != 2; ++cnt) {
    parallelForEach(chunks, [&](SectionChunk *sc) {
      uint32_t hash = sc->eqClass[cnt % 2];
      // A record's spans name labels of its own function.
      if (!unwind)
        for (Symbol *b : sc->symbols())
          if (auto *sym = dyn_cast_or_null<DefinedRegular>(b))
            hash += sym->getChunk()->eqClass[cnt % 2];
      // Set MSB to 1 to avoid collisions with non-hash classes.
      sc->eqClass[(cnt + 1) % 2] = hash | (1U << 31);
    });
  }

  // From now on, sections in Chunks are ordered so that sections in
  // the same group are consecutive in the vector.
  SmallVector<uint64_t, 0> keys(chunks.size());
  parallelFor(0, chunks.size(), [&](size_t i) {
    keys[i] = uint64_t(chunks[i]->eqClass[0]) << 32 | i;
  });
  parallelSort(keys.begin(), keys.end());
  std::vector<SectionChunk *> sorted;
  sorted.reserve(keys.size());
  for (uint64_t k : keys)
    sorted.push_back(chunks[uint32_t(k)]);
  chunks = std::move(sorted);

  // Compare static contents and assign unique IDs for each static content.
  forEachClass([&](size_t begin, size_t end) { segregate<true>(begin, end); });

  // Split groups by comparing relocations until convergence is obtained.
  do {
    repeat = false;
    forEachClass(
        [&](size_t begin, size_t end) { segregate<false>(begin, end); });
  } while (repeat);

  Log(ctx) << "ICF needed " << Twine(cnt) << " iterations";

  // Merge sections in the same classes.
  std::atomic<size_t> removed = 0;
  forEachClass([&](size_t begin, size_t end) {
    if (end - begin == 1)
      return;

    Log(ctx) << "Selected " << chunks[begin]->getDebugName();
    for (size_t i = begin + 1; i < end; ++i) {
      Log(ctx) << "  Removed " << chunks[i]->getDebugName();
      chunks[begin]->replace(chunks[i]);
    }
    removed += end - begin - 1;
  });
  return removed;
}

// Entry point to ICF.
void doICF(COFFLinkerContext &ctx) { ICF(ctx, /*unwind=*/false).run(); }

size_t mergeUnwindRecords(COFFLinkerContext &ctx) {
  return ctx.config.doICF == ICFLevel::None ? 0
                                            : ICF(ctx, /*unwind=*/true).run();
}
} // namespace lld::coff
