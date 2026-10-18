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

// The KCFI prefix, with a marker, of a function the link keeps. From its __cfi_
// symbol: an optional second type word, then 0F 1F 80, the marker and B8, then
// the type, before any patchable prefix and the entry.
struct TypePrefix {
  SectionChunk *chunk;
  // Offset of the __cfi_ symbol in chunk, and the size of the type words and
  // marker from there, 12 or 16 bytes.
  uint32_t offset;
  uint32_t size;
  // The offset in chunk of the function's entry.
  uint32_t entry;
  // Whether the guard table omits the function (type words then overwritten).
  bool sealed = false;
};

// A marked KCFI prefix an object holds: its static __cfi_ symbol, the size of
// its type words and marker, and the offset of the following function, or 0.
struct TypePrefixLabel {
  DefinedRegular *label;
  uint32_t size;
  uint32_t entry;
};

// The marked KCFI prefixes an object holds, and whether it defines code.
struct TypePrefixLabels {
  std::vector<TypePrefixLabel> labels;
  bool definesCode = false;
};

// The KCFI prefixes with a marker that file holds.
ArrayRef<TypePrefixLabel> getTypePrefixes(ObjFile *file);

// Whether file was built by clang or holds no code: it has a KCFI prefix with
// the marker in some section (as clang emits for every external function, even
// in dropped sections), or defines no code. Otherwise it is foreign.
bool hasTypePrefixes(ObjFile *file);

// The KCFI prefixes with a marker of the functions a sealed image keeps. The
// writer finds them before the guard tables, seals them once known, keeps them
// out of a page's first bytes, and overwrites them once the image is written.
class TypePrefixContents {
public:
  TypePrefixContents(COFFLinkerContext &ctx) : ctx(ctx) {}
  void find();
  void seal(const SymbolRVASet &addressTakenSyms,
            const SymbolRVAFlags &suppressed,
            const SymbolRVASet &foreignTakenSyms);
  // The offset in c's output section (starting at secRVA) at which c starts,
  // given that what precedes it ends at off: aligned for c, earlier where c
  // elides its prefix, later to keep prefixed entries out of a page's start.
  uint64_t place(Chunk *c, uint64_t secRVA, uint64_t off) const;
  void write(uint8_t *buf) const;

  ArrayRef<TypePrefix> get() const { return typePrefixes; }
  // Whether the image is sealed.
  bool isSealed() const { return sealedPrefixes; }
  // Whether the guard table lists an unprefixed function a foreign object does.
  bool hasUnprefixedTargets() const { return kcfiUnprefixedTargets; }
  // Whether c holds a prefix of a function that is export-suppressed.
  bool isSuppressed(const Chunk *c) const {
    return kcfiSuppressedChunks.contains(c);
  }
  bool isForeign(ObjFile *file) const {
    return kcfiForeignFiles.contains(file);
  }

private:
  // The KCFI prefixes with a marker, and by chunk indices of those it holds.
  std::vector<TypePrefix> typePrefixes;
  // The objects that define code but no marked KCFI prefix: the foreign ones.
  llvm::DenseSet<ObjFile *> kcfiForeignFiles;
  llvm::DenseMap<const Chunk *, llvm::SmallVector<uint32_t, 1>> kcfiEntries;
  bool sealedPrefixes = false;
  bool kcfiUnprefixedTargets = false;
  llvm::DenseSet<const Chunk *> kcfiSuppressedChunks;
  COFFLinkerContext &ctx;
};
} // namespace lld::coff

#endif
