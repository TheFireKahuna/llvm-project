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

// Gives each chunk the address its objects' pins ask for, in
// COFFLinkerContext::chunkPins.
void resolveLinkPins(COFFLinkerContext &ctx);

// What the layout of .rdata places: a chunk, or chunks that a run of in-place
// import slots crossing from one to the next binds together with no padding
// between them. Its alignment and pin are its first chunk's.
struct PackItem {
  SmallVector<Chunk *, 1> chunks;
  uint64_t size = 0;
  uint32_t align = 1;
  const ChunkPin *pin = nullptr;
  bool isPlaced() const { return pin || align >= 64; }
  // The first offset at or after pos that the item may start at.
  uint64_t startAt(uint64_t pos) const;
};

// Plans the layout of pinned and 64-byte-aligned items, vtables in practice,
// so that the padding before each holds other items rather than zeros. Each in
// turn is the one that needs the least padding from where the layout has
// reached, and its padding is filled with the largest available items that
// fit.
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
  // The available items are ordered by size and then by reverse order, so
  // that the search down from the largest that fits meets equal sizes in
  // input order.
  std::pair<uint64_t, size_t> fillerKey(size_t i) const {
    return std::make_pair(items[i].size, SIZE_MAX - i);
  }
  void fill(uint64_t &pos, uint64_t end, std::vector<size_t> &out);

  ArrayRef<PackItem> items;
  std::set<std::pair<uint64_t, size_t>> fillers;
};

} // namespace lld::coff

#endif
