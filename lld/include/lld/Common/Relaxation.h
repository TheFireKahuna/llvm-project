//===- Relaxation.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The target-independent parts of linker relaxation that deletes bytes at
// relocations. For a section whose relocations are sorted by offset, `aux` has
//   anchors      the symbol anchors in the section, sorted by offset;
//   relocDeltas  for relocation i, the bytes removed up to and including it;
//   relocTypes   for relocation i, its new type, or zero to keep its type.
// A point at a relocation's offset lies before the bytes it removes, unless
// `follows(i)` says that points at relocation i's offset lie after them.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_RELAXATION_H
#define LLD_COMMON_RELAXATION_H

#include "llvm/ADT/ArrayRef.h"
#include <cstring>

namespace lld {
struct NeverFollows {
  bool operator()(size_t) const { return false; }
};

// Run one relaxation pass over a section. `remove(i, r, delta)` returns the
// number of bytes that relocation i removes when `delta` bytes are removed
// before it. Set `delta` to the number of bytes removed from the section and
// return true if a relocDeltas element changed.
template <class AuxT, class RelocT, class RemoveFn,
          class FollowsFn = NeverFollows>
bool relaxSection(AuxT &aux, llvm::MutableArrayRef<RelocT> relocs,
                  int64_t &delta, RemoveFn remove, FollowsFn follows = {}) {
  bool changed = false;
  llvm::ArrayRef sa(aux.anchors);
  delta = 0;
  for (size_t i = 0, e = relocs.size(); i != e; ++i) {
    RelocT &r = relocs[i];
    auto removed = remove(i, r, delta);

    // For all anchors whose offsets are <= r.offset, they are preceded by
    // the previous relocation whose `relocDeltas` value equals `delta`.
    for (; sa.size() && (sa[0].offset < r.offset ||
                         (sa[0].offset == r.offset && !follows(i)));
         sa = sa.slice(1))
      sa[0].apply(sa[0].offset - delta);
    delta += removed;
    if (delta != aux.relocDeltas[i]) {
      aux.relocDeltas[i] = delta;
      changed = true;
    }
  }
  for (const auto &a : sa)
    a.apply(a.offset - delta);
  return changed;
}

// Write the relaxed content of a section, whose content was `old`, to `p` and
// update its relocations. `write(p, i, r, remove)` is called for a relocation
// that removes bytes or has a new type; it writes the bytes that replace the
// ones at the relocation and returns their number.
template <class AuxT, class RelocT, class WriteFn>
void rewriteRelaxedSection(AuxT &aux, llvm::MutableArrayRef<RelocT> rels,
                           llvm::ArrayRef<uint8_t> old, uint8_t *p,
                           WriteFn write) {
  uint64_t offset = 0;
  int64_t delta = 0;
  for (size_t i = 0, e = rels.size(); i != e; ++i) {
    int64_t remove = aux.relocDeltas[i] - delta;
    delta = aux.relocDeltas[i];
    if (remove == 0 && !aux.relocTypes[i])
      continue;

    // Copy from last location to the current relocated location.
    RelocT &r = rels[i];
    uint64_t size = r.offset - offset;
    memcpy(p, old.data() + offset, size);
    p += size;

    int64_t skip = write(p, i, r, remove);
    p += skip;
    offset = r.offset + skip + remove;
  }
  memcpy(p, old.data() + offset, old.size() - offset);

  // Subtract the previous relocDeltas value from the relocation offset.
  // Relocations with the same offset decrease their offset by the same delta.
  delta = 0;
  for (size_t i = 0, e = rels.size(); i != e;) {
    uint64_t cur = rels[i].offset;
    do {
      rels[i].offset -= delta;
      if (aux.relocTypes[i])
        rels[i].type = aux.relocTypes[i];
    } while (++i != e && rels[i].offset == cur);
    delta = aux.relocDeltas[i - 1];
  }
}
} // namespace lld

#endif
