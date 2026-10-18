//===- LinkPins.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_LINKPINS_H
#define LLD_COFF_LINKPINS_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace lld::coff {
class Chunk;
class COFFLinkerContext;
struct ChunkPin;
struct VTableRun;

// Resolves each chunk's pins into its address, in COFFLinkerContext::chunkPins.
void resolveLinkPins(COFFLinkerContext &ctx);

// A unit of .rdata layout: one chunk, or several that in-place import slots
// cross and so bind together with no padding. Its align and pin are the first
// chunk's.
struct PackItem {
  SmallVector<Chunk *, 1> chunks;
  uint64_t size = 0;
  uint32_t align = 1;
  const ChunkPin *pin = nullptr;
  // A vtable in a run, at slot. The first slot's item stands for the whole run;
  // later slots follow at the run's stride, with other items in the gaps.
  const VTableRun *run = nullptr;
  unsigned slot = 0;
  // A tagged vtable, offered the gaps of its primary base's run.
  const VTableRun *baseRun = nullptr;
  bool isPlaced() const { return pin || align >= 64 || run; }
  // The first offset at or after pos that the item may start at.
  uint64_t startAt(uint64_t pos) const;
};

// Lays out pinned and 64-byte-aligned items (vtables, in practice) so that the
// padding before each holds other items rather than zeros: items are placed
// least-padding-first, each gap filled with the largest items that fit.
class RdataPacker {
public:
  explicit RdataPacker(ArrayRef<PackItem> items) : items(items) {}

  // Makes item i available to fill gaps.
  void addFiller(size_t i) { fillers.insert(fillerKey(i)); }
  // Withdraws item i, returning whether no gap took it.
  bool takeFiller(size_t i) { return fillers.erase(fillerKey(i)); }

  // Packs the placed items of [begin, end) from offset pos, returning them and
  // the items that went into their gaps in layout order.
  std::vector<size_t> pack(size_t begin, size_t end, uint64_t pos);

private:
  // Orders items by size, then reverse index, so the search down from the
  // largest that fits meets equal sizes in input order.
  std::pair<uint64_t, size_t> fillerKey(size_t i) const {
    return std::make_pair(items[i].size, SIZE_MAX - i);
  }
  void fill(uint64_t &pos, uint64_t end, std::vector<size_t> &out);
  uint64_t layOutRun(const VTableRun &run, ArrayRef<size_t> slots,
                     uint64_t start, std::vector<size_t> &pinned,
                     std::vector<size_t> &out);

  ArrayRef<PackItem> items;
  std::set<std::pair<uint64_t, size_t>> fillers;
};

} // namespace lld::coff

#endif
