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

/// Returns the 8 bytes that precede the type ID in a KCFI prefix with a
/// marker, read as a little-endian integer: 0F 1F 80, the marker, and B8, which
/// x86 decodes as a nopl whose displacement is the marker and the opcode of a
/// move of the type ID.
inline uint64_t getTypePrefixPattern(uint32_t Marker) {
  return 0xB8000000'00801F0FULL | uint64_t(Marker) << 24;
}

/// Returns the marker of a KCFI prefix, which tells prefixes whose type IDs
/// follow one definition from those of any other: whether integer types are
/// normalized, whether pointer types are generalized, and the hash algorithm.
LLVM_ABI uint32_t getTypePrefixMarker(bool NormalizeIntegers,
                                      bool GeneralizePointers,
                                      KCFIHashAlgorithm Algorithm);

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_KCFIHASH_H
