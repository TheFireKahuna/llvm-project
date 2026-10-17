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
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/BinaryFormat/COFF.h"
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

// Gathers the classes the live records name, keeping one vtable per class. A
// chunk that holds more than one class's address point is a merged global,
// whose classes are left out, since its vtables do not each have a slot.
static void gatherClasses(COFFLinkerContext &ctx,
                          MapVector<StringRef, ClassNode> &classes) {
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
// size, which is contiguous in pre-order.
static void walk(StringRef name, MapVector<StringRef, ClassNode> &classes,
                 DenseMap<StringRef, std::vector<StringRef>> &children,
                 DenseSet<StringRef> &visited, VTableRun &run) {
  if (!visited.insert(name).second)
    return;
  const ClassNode &node = classes.find(name)->second;
  unsigned first = run.slots.size();
  run.classes.push_back({name.str(), first, 0});
  unsigned classIndex = run.classes.size() - 1;
  run.slots.push_back({node.vtable->getChunk(), node.apoff});
  std::vector<StringRef> &kids = children[name];
  llvm::sort(kids);
  for (StringRef kid : kids)
    walk(kid, classes, children, visited, run);
  run.classes[classIndex].count = run.slots.size() - first;
}

void lld::coff::registerVTableRunSymbols(COFFLinkerContext &ctx) {
  ctx.symtab.forEachSymbol([&](Symbol *s) {
    if (!isa<Undefined>(s))
      return;
    StringRef name = s->getName();
    if (name.starts_with(VTableRangeStartPrefix) ||
        name.starts_with(VTableRangeEndPrefix) ||
        name.starts_with(VTableRangeMaskPrefix))
      replaceSymbol<DefinedSynthetic>(s, name, nullptr);
  });
}

void lld::coff::resolveVTableRuns(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Resolve vtable runs");
  MapVector<StringRef, ClassNode> classes;
  gatherClasses(ctx, classes);
  if (classes.empty())
    return;

  // A class whose vtable is too large for a slot is left out; its children
  // then head their own runs. The exclusion repeats until every run's slot
  // fits, which the first pass almost always reaches.
  DenseSet<StringRef> excluded;
  for (;;) {
    // Each non-excluded class is a child of its primary base when that base is
    // present too, and otherwise heads a run.
    DenseMap<StringRef, std::vector<StringRef>> children;
    std::vector<StringRef> roots;
    for (auto &[name, node] : classes) {
      if (excluded.contains(name))
        continue;
      if (!node.base.empty() && classes.count(node.base) &&
          !excluded.contains(node.base))
        children[node.base].push_back(name);
      else
        roots.push_back(name);
    }
    llvm::sort(roots);

    ctx.vtableRuns.clear();
    ctx.runChunkRun.clear();
    DenseSet<StringRef> visited;
    bool dropped = false;
    for (StringRef root : roots) {
      VTableRun run;
      walk(root, classes, children, visited, run);
      // The slot covers the largest vtable, with its address point, so that no
      // slot boundary but an address point falls in the run.
      uint64_t widest = 0;
      StringRef widestClass;
      for (unsigned i = 0; i != run.slots.size(); ++i) {
        uint64_t span = run.slots[i].chunk->getSize() + run.slots[i].apoff;
        if (span > widest) {
          widest = span;
          widestClass = run.classes[i].name;
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

  // The writer places a run's vtables itself, so their advisory line pins,
  // which would fix them one at a time, are dropped.
  for (auto &[chunk, run] : ctx.runChunkRun)
    ctx.chunkPins.erase(chunk);
}

void lld::coff::placeVTableRuns(COFFLinkerContext &ctx,
                                OutputSection *rdataSec) {
  if (ctx.vtableRuns.empty())
    return;
  for (const VTableRun &run : ctx.vtableRuns)
    rdataSec->addChunk(run.maskChunk);
  std::vector<Chunk *> &chunks = rdataSec->chunks;
  for (const VTableRun &run : ctx.vtableRuns) {
    DenseSet<const Chunk *> members;
    for (const VTableRun::Slot &slot : run.slots)
      members.insert(slot.chunk);
    // The run's vtables go where the first of them now is, in slot order, with
    // nothing between, so assignAddresses can space them by the stride.
    size_t at = chunks.size();
    for (size_t i = 0; i != chunks.size(); ++i)
      if (members.contains(chunks[i])) {
        at = i;
        break;
      }
    if (at == chunks.size())
      continue;
    llvm::erase_if(chunks, [&](Chunk *c) { return members.contains(c); });
    std::vector<Chunk *> ordered;
    for (const VTableRun::Slot &slot : run.slots)
      ordered.push_back(slot.chunk);
    chunks.insert(chunks.begin() + std::min(at, chunks.size()), ordered.begin(),
                  ordered.end());
  }
}

void lld::coff::defineVTableRuns(COFFLinkerContext &ctx) {
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
      } else {
        define(VTableRangeStartPrefix, nullptr, 0);
        define(VTableRangeEndPrefix, nullptr, 0);
        define(VTableRangeMaskPrefix, nullptr, 0);
      }
    }
  }

  // A range symbol whose class has no run keeps the empty range that
  // registerVTableRunSymbols gave it, so every vtable pointer misses it.
}
