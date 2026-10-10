//===- LinkPins.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pins ask for an object's address modulo a power of two. This file resolves
// the pins the objects carry into one per chunk, and plans the layout of
// .rdata that fills the padding the pins and wide alignments leave.
//
//===----------------------------------------------------------------------===//

#include "LinkPins.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <deque>
#include <map>

using namespace llvm;
using namespace llvm::COFF;
using namespace lld;
using namespace lld::coff;

// Gives each chunk the address its objects' pins ask for. Required pins of
// one chunk must agree, modulo the smaller of their moduli, or the link
// fails; pins that are not required apply only if they all agree with each
// other and with the required ones, and are otherwise left out with a
// warning. The pin with the largest modulus gives the chunk its residue. The
// outcome does not depend on the order of the inputs.
void lld::coff::resolveLinkPins(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Resolve link pins");
  struct Given {
    ObjFile *file;
    const ObjFile::LinkPin *pin;
    uint64_t residue;
  };
  auto describe = [&](const Given &g) {
    return g.pin->sym ? toString(ctx, *g.pin->sym)
                      : g.pin->chunk->getSectionName().str();
  };
  auto report = [&](bool required, const Given &g, const Twine &msg) {
    (required ? Err(ctx) : Warn(ctx))
        << g.file << ": pin of " << describe(g) << " " << msg;
  };

  MapVector<SectionChunk *, SmallVector<Given, 1>> given;
  for (ObjFile *file : ctx.objFileInstances) {
    for (const ObjFile::LinkPin &pin : file->getLinkPins()) {
      SectionChunk *sc = pin.chunk;
      uint64_t residue = pin.residue;
      // A symbol that resolves to an import or to nothing in the image is
      // placed by the image that defines it.
      if (pin.sym) {
        auto *d = dyn_cast<DefinedRegular>(pin.sym);
        if (!d)
          continue;
        sc = d->getChunk();
        residue -= d->getValue();
      }
      if (!sc || !sc->live)
        continue;
      Given g{file, &pin, residue & maskTrailingOnes<uint64_t>(pin.log2)};
      // Code has placement rules of its own, such as keeping functions with
      // type prefixes out of a page's first bytes, which a pin would undo.
      if (sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE) {
        report(pin.required, g, "is in executable code, which is not pinned");
        continue;
      }
      // Bits 0 to 11 of an address are those of its RVA, since the image
      // base is page-aligned.
      if (pin.log2 > 12) {
        report(pin.required, g, "asks for a modulus larger than a page");
        continue;
      }
      if (g.residue % std::min<uint64_t>(sc->getAlignment(), 1ULL << pin.log2)) {
        report(pin.required, g, "conflicts with the alignment of its section");
        continue;
      }
      given[sc].push_back(g);
    }
  }

  auto agree = [](const Given &a, const Given &b) {
    uint64_t mask =
        maskTrailingOnes<uint64_t>(std::min(a.pin->log2, b.pin->log2));
    return (a.residue & mask) == (b.residue & mask);
  };
  for (auto &[sc, pins] : given) {
    // Each pin is checked against the one with the largest modulus so far,
    // which agrees with every pin before it.
    const Given *strongest = nullptr;
    for (const Given &g : pins) {
      if (!g.pin->required)
        continue;
      if (strongest && !agree(*strongest, g)) {
        Err(ctx) << g.file << ": pin of " << describe(g)
                 << " conflicts with the pin of " << describe(*strongest)
                 << " in " << strongest->file;
        continue;
      }
      if (!strongest || g.pin->log2 > strongest->pin->log2)
        strongest = &g;
    }
    bool required = strongest;
    const Given *advisory = strongest;
    for (const Given &g : pins) {
      if (g.pin->required)
        continue;
      if (advisory && !agree(*advisory, g)) {
        Warn(ctx) << g.file << ": pin of " << describe(g)
                  << " conflicts with the pin of " << describe(*advisory)
                  << " in " << advisory->file
                  << "; the pins that are not required are left out";
        advisory = strongest;
        break;
      }
      if (!advisory || g.pin->log2 > advisory->pin->log2)
        advisory = &g;
    }
    if (advisory)
      ctx.chunkPins[sc] = {advisory->residue, advisory->pin->log2, required};
  }
  if (!ctx.chunkPins.empty() && ctx.config.imageBase % 4096)
    Err(ctx) << "/base: an image with pinned sections must be based at a "
                "multiple of 4096";
}

uint64_t PackItem::startAt(uint64_t pos) const {
  pos = alignTo(pos, align);
  if (pin)
    pos = alignTo(pos, uint64_t(1) << pin->log2, pin->residue);
  return pos;
}

void RdataPacker::fill(uint64_t &pos, uint64_t end, std::vector<size_t> &out) {
  while (pos < end) {
    auto it = fillers.upper_bound({end - pos, SIZE_MAX});
    bool placed = false;
    // An item's alignment can keep it from a gap its size fits; a few
    // smaller ones are tried before the gap is left as it is.
    for (int tries = 0; it != fillers.begin() && tries != 4; ++tries) {
      --it;
      size_t i = SIZE_MAX - it->second;
      uint64_t start = alignTo(pos, items[i].align);
      if (start + items[i].size > end)
        continue;
      out.push_back(i);
      pos = start + items[i].size;
      fillers.erase(it);
      placed = true;
      break;
    }
    if (!placed)
      return;
  }
}

std::vector<size_t> RdataPacker::pack(size_t begin, size_t end, uint64_t pos) {
  std::vector<size_t> out, pinned;
  std::map<uint32_t, std::deque<size_t>> aligned;
  for (size_t i = begin; i != end; ++i) {
    if (items[i].pin)
      pinned.push_back(i);
    else if (items[i].isPlaced())
      aligned[items[i].align].push_back(i);
  }
  for (;;) {
    size_t best = SIZE_MAX;
    uint64_t bestStart = 0;
    auto consider = [&](size_t i) {
      uint64_t start = items[i].startAt(pos);
      if (best == SIZE_MAX || start < bestStart ||
          (start == bestStart && i < best)) {
        best = i;
        bestStart = start;
      }
    };
    for (size_t i : pinned)
      consider(i);
    // An aligned item goes first only if it ends before the next pinned one
    // would start, so that it never pushes a pin a page further on.
    uint64_t pinStart = best == SIZE_MAX ? UINT64_MAX : bestStart;
    for (auto &[align, queue] : aligned)
      if (!queue.empty() &&
          (pinStart == UINT64_MAX ||
           items[queue.front()].startAt(pos) + items[queue.front()].size <=
               pinStart))
        consider(queue.front());
    if (best == SIZE_MAX)
      return out;
    fill(pos, bestStart, out);
    out.push_back(best);
    pos = bestStart + items[best].size;
    if (items[best].pin)
      llvm::erase(pinned, best);
    else
      aligned[items[best].align].pop_front();
  }
}
