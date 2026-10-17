//===- TypePrefix.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_TYPEPREFIX_H
#define LLD_COFF_TYPEPREFIX_H

#include "Chunks.h"
#include "InputFiles.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <vector>

namespace lld::coff {
class COFFLinkerContext;
class ObjFile;

// The KCFI prefix, with a marker, of a function the link keeps. From its
// __cfi_ symbol it holds an optional second type word, then 0F 1F 80, the
// marker and B8, then the type, before any patchable prefix and the entry.
struct TypePrefix {
  SectionChunk *chunk;
  // The offset in chunk of the __cfi_ symbol, and the size of the type words
  // and the marker from there, 12 or 16 bytes.
  uint32_t offset;
  uint32_t size;
  // The offset in chunk of the function's entry.
  uint32_t entry;
  // Whether the guard function table omits the function, whose type words
  // are then overwritten.
  bool sealed = false;
};

// A KCFI prefix with a marker that an object holds: its static __cfi_ symbol,
// the size of its type words and marker, and the offset of the first function
// to follow it in its chunk, or 0 if none does.
struct TypePrefixLabel {
  DefinedRegular *label;
  uint32_t size;
  uint32_t entry;
};

// The KCFI prefixes with a marker that an object holds, and whether it defines
// code, which only links that ask about them find.
struct TypePrefixLabels {
  std::vector<TypePrefixLabel> labels;
  bool definesCode = false;
};

// The KCFI prefixes with a marker that file holds.
ArrayRef<TypePrefixLabel> getTypePrefixes(ObjFile *file);

// Whether file was built by clang or holds no code: it holds a KCFI prefix
// with the marker in some section, including those the link drops, as clang's
// objects do for every external function, or defines no code. An object for
// which this is false is foreign.
bool hasTypePrefixes(ObjFile *file);

// In a link whose objects say they have them, the KCFI prefixes with a marker
// of the functions the image keeps, which the writer finds before the guard
// tables, seals once they are known, keeps out of a page's first bytes, and
// writes over once the image is written.
class TypePrefixContents {
public:
  TypePrefixContents(COFFLinkerContext &ctx) : ctx(ctx) {}
  void find();
  void seal(const SymbolRVASet &addressTakenSyms,
            const SymbolRVAFlags &suppressed,
            const SymbolRVASet &foreignTakenSyms);
  // The offset in its output section, which starts at secRVA, at which c
  // starts, where what precedes it ends at off: aligned for c, and later so
  // that no entry of a function with a prefix in it starts in a page's first
  // bytes.
  uint64_t place(Chunk *c, uint64_t secRVA, uint64_t off) const;
  void write(uint8_t *buf) const;

  ArrayRef<TypePrefix> get() const { return typePrefixes; }
  // Whether the image is sealed.
  bool isSealed() const { return sealedPrefixes; }
  // Whether the guard function table lists a function without a prefix that a
  // foreign object lists.
  bool hasUnprefixedTargets() const { return kcfiUnprefixedTargets; }
  // Whether c holds a prefix of a function that is export-suppressed.
  bool isSuppressed(const Chunk *c) const {
    return kcfiSuppressedChunks.contains(c);
  }
  bool isForeign(ObjFile *file) const {
    return kcfiForeignFiles.contains(file);
  }
  const llvm::DenseSet<ObjFile *> &getForeignFiles() const {
    return kcfiForeignFiles;
  }

private:
  // The KCFI prefixes with a marker; by chunk, the indices of those it holds;
  // whether the image is sealed; the chunks holding a prefix of a function
  // that is export-suppressed.
  std::vector<TypePrefix> typePrefixes;
  // The objects that define code and no KCFI prefix with a marker, which are
  // foreign.
  llvm::DenseSet<ObjFile *> kcfiForeignFiles;
  llvm::DenseMap<const Chunk *, llvm::SmallVector<uint32_t, 1>> kcfiEntries;
  bool sealedPrefixes = false;
  bool kcfiUnprefixedTargets = false;
  llvm::DenseSet<const Chunk *> kcfiSuppressedChunks;
  COFFLinkerContext &ctx;
};
} // namespace lld::coff

#endif
