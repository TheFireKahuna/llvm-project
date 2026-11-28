//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers for computing the 32-bit KCFI type ID from a mangled type name.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_KCFIHASH_H
#define LLVM_TRANSFORMS_UTILS_KCFIHASH_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace llvm {

enum class KCFIHashAlgorithm { xxHash64, FNV1a };

/// Parse a KCFI hash algorithm name.
/// Returns xxHash64 if the name is not recognized.
LLVM_ABI KCFIHashAlgorithm parseKCFIHashAlgorithm(StringRef Name);

/// Convert a KCFI hash algorithm enum to its string representation.
LLVM_ABI StringRef stringifyKCFIHashAlgorithm(KCFIHashAlgorithm Algorithm);

/// Compute KCFI type ID from mangled type name.
/// The algorithm can be xxHash64 or FNV-1a.
LLVM_ABI uint32_t getKCFITypeID(StringRef MangledTypeName,
                                KCFIHashAlgorithm Algorithm);

/// Returns a KCFI type as x86 stores it before a function: a type that would
/// spell an ENDBR64 or ENDBR32 instruction, as the immediate of the prefix's
/// move or, negated, of a check's compare, is stored plus one.
LLVM_ABI uint32_t getX86KCFIType(uint32_t Type);

/// Returns the marker of a function type prefix, which tells prefixes whose
/// type IDs follow one definition from those of any other: whether integer
/// types are normalized, whether pointer types are generalized, and the hash
/// algorithm.
LLVM_ABI uint32_t getTypePrefixMarker(bool NormalizeIntegers,
                                      bool GeneralizePointers,
                                      KCFIHashAlgorithm Algorithm);

/// Returns the type ID that a function type prefix stores for the type that
/// hashes to \p Hash: its low 31 bits, since the prefix of a function with an
/// entry that is passed the thread pointer sets the 32nd, and one more where
/// that is the sealed type or the tagged type would spell ENDBR64 or ENDBR32.
LLVM_ABI uint32_t getTypePrefixTypeId(uint32_t Hash);

/// Returns the 8 bytes that precede the type ID in a function type prefix, as
/// a little-endian integer: 0F 1F, then 80 with the set of
/// COFF::LinkFactPrefixWord that the prefix holds before them, the marker, and
/// B8, which x86 decodes as a nopl whose displacement is the marker and the
/// opcode of a move of the type ID. A check of a word before them compares it
/// together with the 4 bytes that follow, so it fails on a prefix that has
/// another set, whatever precedes that prefix.
inline uint64_t getTypePrefixPattern(uint32_t Marker, unsigned Words) {
  return 0xB8000000'00801F0FULL | uint64_t(Words) << 16 |
         uint64_t(Marker) << 24;
}

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_KCFIHASH_H
