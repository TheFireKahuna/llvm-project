//===- TypePrefix.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TypePrefix.h"
#include "COFFLinkerContext.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {
// The eight bytes from 0F 1F 80 to B8 of a prefix of ours; its marker encodes
// the type definition these targets fix (pointers generalized, xxHash64).
static uint64_t getPattern() {
  static const uint64_t pattern = getTypePrefixPattern(getTypePrefixMarker(
      /*NormalizeIntegers=*/false, /*GeneralizePointers=*/true,
      KCFIHashAlgorithm::xxHash64));
  return pattern;
}

// The size of the type words and marker of the KCFI prefix a static __cfi_
// label at off in sc begins -- 12 or 16 bytes -- or 0 without our marker.
static uint32_t getTypePrefixSize(SectionChunk *sc, uint32_t off) {
  ArrayRef<uint8_t> data = sc->getContents();
  auto hasMarker = [&](uint32_t at) {
    return at + 12 <= data.size() &&
           endian::read64le(data.data() + at) == getPattern();
  };
  return hasMarker(off) ? 12 : hasMarker(off + 4) ? 16 : 0;
}

// Finds the marked KCFI prefixes file holds (cached). A prefix belongs to the
// first function following it in its chunk; one that none follows is malformed.
static TypePrefixLabels &findFileTypePrefixes(ObjFile *file) {
  TypePrefixLabels *&cached = file->symtab.ctx.typePrefixLabels[file];
  if (cached)
    return *cached;
  cached = make<TypePrefixLabels>();
  TypePrefixLabels &found = *cached;
  SmallVector<DefinedRegular *, 0> labels, entries;
  for (Symbol *s : file->getSymbols()) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    if (!d || d->file != file || !d->getChunk() ||
        !(d->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
      continue;
    // An empty section, e.g. the .text assemblers emit, holds no code.
    found.definesCode |= d->getChunk()->getSize() != 0;
    if (d->getCOFFSymbol().getComplexType() == IMAGE_SYM_DTYPE_FUNCTION)
      entries.push_back(d);
    else if (!d->getCOFFSymbol().isExternal() &&
             d->getName().starts_with("__cfi_"))
      labels.push_back(d);
  }
  auto byLocation = [](DefinedRegular *a, DefinedRegular *b) {
    return std::make_pair(a->getChunk(), a->getValue()) <
           std::make_pair(b->getChunk(), b->getValue());
  };
  llvm::sort(entries, byLocation);
  for (DefinedRegular *label : labels) {
    SectionChunk *sc = label->getChunk();
    uint32_t off = label->getValue();
    // A prefix without the marker, such as upstream KCFI's, is not ours.
    uint32_t size = getTypePrefixSize(sc, off);
    if (size == 0)
      continue;
    auto it = llvm::upper_bound(entries, label, byLocation);
    uint32_t entry = 0;
    if (it != entries.end() && (*it)->getChunk() == sc &&
        (*it)->getValue() >= off + size)
      entry = (*it)->getValue();
    found.labels.push_back({label, size, entry});
  }
  return found;
}

ArrayRef<TypePrefixLabel> getTypePrefixes(ObjFile *file) {
  return findFileTypePrefixes(file).labels;
}

bool hasTypePrefixes(ObjFile *file) {
  TypePrefixLabels &found = findFileTypePrefixes(file);
  return !found.labels.empty() || !found.definesCode;
}

// Finds the marked KCFI prefix of every function the link keeps, keyed by the
// chunk that survives identical code folding, and the foreign objects.
void TypePrefixContents::find() {
  DenseSet<std::pair<SectionChunk *, uint32_t>> seen;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!hasTypePrefixes(file))
      kcfiForeignFiles.insert(file);
    for (const TypePrefixLabel &l : getTypePrefixes(file)) {
      SectionChunk *sc = l.label->getChunk();
      uint32_t off = l.label->getValue();
      if (!sc->live)
        continue;
      if (!l.entry) {
        Err(ctx) << file << ": no function follows the KCFI prefix at "
                 << l.label->getName();
        continue;
      }
      if (!seen.insert({sc, off}).second)
        continue;
      kcfiEntries[sc].push_back(typePrefixes.size());
      typePrefixes.push_back({sc, off, l.size, l.entry});
    }
  }
}

// The SectionChunk::elidedPrefix value that elides the bytes before an entry at
// offset entry in a chunk aligned to align, or 0 if none does.
static uint8_t getElidedPrefixKind(uint32_t entry, uint32_t align) {
  if (entry == 12)
    return 1;
  if (entry == 16)
    return 2;
  return entry == align ? 3 : 0;
}

// Seals an image whose objects have KCFI prefixes: a function the guard table
// omits has its KCFI type overwritten, so a KCFI check never accepts a function
// CFG would reject. Folding can key several functions to one chunk, so a folded
// function stays unsealed if any folded into it is listed. A suppressed
// function keeps its type (a pointer the process later makes valid is valid),
// so its chunk is kept outside the code range KCFI thunks match in directly.
void TypePrefixContents::seal(const SymbolRVASet &addressTakenSyms,
                              const SymbolRVAFlags &suppressed,
                              const SymbolRVASet &foreignTakenSyms) {
  for (TypePrefix &p : typePrefixes) {
    p.sealed = !addressTakenSyms.contains({p.chunk, p.entry});
    if (suppressed.contains({p.chunk, p.entry}))
      kcfiSuppressedChunks.insert(p.chunk);
  }
  sealedPrefixes = true;
  // CFG accepts any address in the 16-byte granule of an entry listed off a
  // boundary, but only the boundary of one on it. Eliding an off-boundary
  // entry's prefix would share its granule with the chunk's predecessor, and
  // vice versa, letting the guard accept addresses it otherwise rejects.
  bool openGranules = false;
  DenseSet<const Chunk *> unalignedTargetChunks;
  for (const ChunkAndOffset &c : addressTakenSyms) {
    if (c.offset % 16 == 0)
      continue;
    unalignedTargetChunks.insert(c.inputChunk);
    openGranules |= alignDown(c.offset, 16) + 16 > c.inputChunk->getSize();
  }
  // No call reads a sealed prefix as a match, so where only padding precedes it
  // the chunk may start early over the end of its predecessor. No relocation or
  // resize edit may lie in the elided bytes; the prefix's label is static.
  auto changesBefore = [&](const SectionChunk *sc, uint32_t entry) {
    if (!sc->relaxed)
      return false;
    const Relaxation &r = ctx.relaxations.find(sc)->second;
    return !r.edits.empty() && r.edits.front().offset < entry;
  };
  for (const auto &[c, indices] : kcfiEntries) {
    const TypePrefix &p = typePrefixes[indices[0]];
    uint32_t align = p.chunk->getAlignment();
    bool alignedEntry = align >= 16 && p.entry % 16 == 0;
    if (indices.size() == 1 && p.sealed && p.offset + p.size == p.entry &&
        !unalignedTargetChunks.contains(p.chunk) &&
        (alignedEntry || !openGranules) &&
        llvm::none_of(p.chunk->getRelocs(),
                      [&](const coff_relocation &r) {
                        return r.VirtualAddress < p.entry;
                      }) &&
        !changesBefore(p.chunk, p.entry))
      p.chunk->elidedPrefix = getElidedPrefixKind(p.entry, align);
  }
  kcfiUnprefixedTargets =
      llvm::any_of(foreignTakenSyms, [&](const ChunkAndOffset &c) {
        auto it = kcfiEntries.find(c.inputChunk);
        return it == kcfiEntries.end() ||
               llvm::none_of(it->second, [&](uint32_t i) {
                 return typePrefixes[i].entry == c.offset;
               });
      });
}

// A KCFI check outside the code range reads no prefix before a target in a
// page's first bytes (which may follow an unmapped page) and treats it as
// foreign, so no prefixed entry goes within the first PowerOf2Ceil of its
// prefix's bytes into a page. A chunk elides its prefix to start early, or
// pads; where neither works, only a function no pointer can reach may stay.
uint64_t TypePrefixContents::place(Chunk *c, uint64_t secRVA,
                                   uint64_t off) const {
  if (uint32_t elided = c->getElidedSize()) {
    if (off != 0)
      return off > elided ? alignTo(off - elided, c->getAlignment()) : 0;
    c->elidedPrefix = 0;
  }
  off = alignTo(off, c->getAlignment());
  auto it = kcfiEntries.find(c);
  if (it == kcfiEntries.end())
    return off;
  uint64_t rva = secRVA + off;
  uint64_t padding = 0;
  auto inPageStart = [&](bool all) {
    return llvm::any_of(it->second, [&](uint32_t i) {
      const TypePrefix &p = typePrefixes[i];
      return (all || !p.sealed) && (rva + padding + p.entry) % 4096 <
                                       PowerOf2Ceil(p.entry - p.offset);
    });
  };
  for (uint32_t pad = c->getAlignment();
       pad < 4096 && inPageStart(/*all=*/true); pad += c->getAlignment())
    padding += c->getAlignment();
  if (inPageStart(/*all=*/false))
    Err(ctx) << "cannot place " << c->getDebugName()
             << " so that its functions with a KCFI prefix start past the "
                "first bytes of a page";
  return off + padding;
}

// Overwrites the type words of each sealed KCFI prefix with the type no call
// expects. Where a prefix is elided, its predecessor's bytes must not read as
// one of ours (as the marker's end, compared by a type check, or its start, by
// a second-type check), or a KCFI check would accept the function. A function
// run into by another shares that function's entry and prefix.
void TypePrefixContents::write(uint8_t *buf) const {
  llvm::TimeTraceScope timeScope("Write type prefixes");
  for (const TypePrefix &p : typePrefixes) {
    if (!p.sealed)
      continue;
    OutputSection *sec = ctx.getOutputSection(p.chunk);
    uint8_t *loc = buf + sec->getFileOff() + p.chunk->getRVA() - sec->getRVA() +
                   p.chunk->getOutputOffset(p.offset);
    if (p.chunk->getElidedSize() && !ctx.tailJumpTargets.contains(p.chunk)) {
      if (read32le(loc + p.size - 8) == getPattern() >> 32 ||
          read32le(loc + p.size - 12) == uint32_t(getPattern()))
        Err(ctx) << "cannot leave out the KCFI prefix of "
                 << p.chunk->getDebugName()
                 << ": the bytes before its entry read as one";
      continue;
    }
    if (p.size == 16)
      write32le(loc, COFF::SealedTypeId);
    write32le(loc + p.size - 4, COFF::SealedTypeId);
  }
}
} // namespace lld::coff
