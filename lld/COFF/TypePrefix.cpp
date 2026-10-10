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
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {
// The eight bytes from 0F 1F 80 to B8 of a prefix of ours. The marker folds in
// the definition of the types, which these targets fix: pointer types
// generalized, xxHash64. A prefix with any other marker, of an object built
// with other options, is not ours.
static uint64_t getPattern() {
  static const uint64_t pattern = getTypePrefixPattern(getTypePrefixMarker(
      /*NormalizeIntegers=*/false, /*GeneralizePointers=*/true,
      KCFIHashAlgorithm::xxHash64));
  return pattern;
}

// The size of the type words and the marker of the KCFI prefix that a static
// __cfi_ label at off in sc begins, 12 or 16 bytes, or 0 without the marker
// of the type definition these targets fix.
static uint32_t getTypePrefixSize(SectionChunk *sc, uint32_t off) {
  ArrayRef<uint8_t> data = sc->getContents();
  auto hasMarker = [&](uint32_t at) {
    return at + 12 <= data.size() &&
           endian::read64le(data.data() + at) == getPattern();
  };
  return hasMarker(off) ? 12 : hasMarker(off + 4) ? 16 : 0;
}

// Finds the KCFI prefixes with a marker that file holds, the first time it is
// asked. The function a prefix belongs to is the first that follows it in its
// chunk; a prefix with the marker that no function follows is malformed.
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
    // An empty section, such as the .text an assembler always emits, holds no
    // code.
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

// Finds the KCFI prefix with a marker of every function the link keeps, keyed
// by the chunk that remains after identical code folding, and the foreign
// objects, which have none.
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

// An image whose objects have KCFI prefixes is sealed: a function the table
// omits has its KCFI type overwritten, so that a KCFI check never accepts a
// function that Control Flow Guard would reject. Identical code folding can
// make one chunk the definition of several functions, and the table and the
// prefixes are both keyed by the chunk that remains, so a folded function
// stays unsealed if any function folded into it is listed.
//
// A function without a prefix that a foreign object lists, such as one of
// its own or an import thunk, is one that foreign code can hand to ours.
//
// A suppressed function, one the table marks export-suppressed or suppressed,
// keeps its type, since a pointer to it that GetProcAddress returns or the
// process otherwise makes valid is valid, but the guard function accepts it
// only after that, so its chunk is kept outside the code range that KCFI's
// thunks accept a match in directly.
void TypePrefixContents::seal(const SymbolRVASet &addressTakenSyms,
                              const SymbolRVAFlags &suppressed,
                              const SymbolRVASet &foreignTakenSyms) {
  for (TypePrefix &p : typePrefixes) {
    p.sealed = !addressTakenSyms.contains({p.chunk, p.entry});
    if (suppressed.contains({p.chunk, p.entry}))
      kcfiSuppressedChunks.insert(p.chunk);
  }
  sealedPrefixes = true;
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
// page's first bytes, which may follow an unmapped page, and treats it as
// foreign, so no entry of a prefixed function goes there: none in a page's
// first PowerOf2Ceil of the bytes from its prefix's start, 12, or 16 with a
// second type word, and any patchable prefix. Padding by less than a page
// tries every place the alignment allows. Where none works, a function no
// pointer may reach can stay; any other could not be called through a pointer
// of a closed type.
uint64_t TypePrefixContents::place(Chunk *c, uint64_t secRVA,
                                   uint64_t off) const {
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
void TypePrefixContents::write(uint8_t *buf) const {
  for (const TypePrefix &p : typePrefixes) {
    if (!p.sealed)
      continue;
    OutputSection *sec = ctx.getOutputSection(p.chunk);
    uint8_t *loc =
        buf + sec->getFileOff() + p.chunk->getRVA() - sec->getRVA() + p.offset;
    if (p.size == 16)
      write32le(loc, COFF::SealedTypeId);
    write32le(loc + p.size - 4, COFF::SealedTypeId);
  }
}
} // namespace lld::coff
