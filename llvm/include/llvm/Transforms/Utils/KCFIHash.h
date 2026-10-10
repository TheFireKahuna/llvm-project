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
#include <optional>

namespace llvm {

class Module;

enum class KCFIHashAlgorithm { xxHash64, FNV1a };

/// The operands of the !kcfi_thunk metadata the CFGuard pass attaches to a
/// per-type thunk's declaration, which the backend reads instead of parsing
/// the thunk's name. Operand 0 is the kind, operand 1 the type, operand 2 the
/// flags. A member thunk also carries its tags in !kcfi_member_tags.
enum KCFIThunkKind { KCFIThunkDispatch, KCFIThunkCheck };
enum KCFIThunkFlag {
  KCFIThunkLocal = 1 << 0,
  KCFIThunkVfn = 1 << 1,
  KCFIThunkMember = 1 << 2,
};

/// Parse a KCFI hash algorithm name.
/// Returns xxHash64 if the name is not recognized.
LLVM_ABI KCFIHashAlgorithm parseKCFIHashAlgorithm(StringRef Name);

/// Convert a KCFI hash algorithm enum to its string representation.
LLVM_ABI StringRef stringifyKCFIHashAlgorithm(KCFIHashAlgorithm Algorithm);

/// Compute KCFI type ID from mangled type name.
/// The algorithm can be xxHash64 or FNV-1a.
LLVM_ABI uint32_t getKCFITypeID(StringRef MangledTypeName,
                                KCFIHashAlgorithm Algorithm);

/// Returns a KCFI type as x86 stores it in a function's prefix. A type that
/// would spell an ENDBR64 or ENDBR32 instruction, as the immediate of the
/// prefix's move or, negated, of a check's compare, is stored plus one.
LLVM_ABI uint32_t getX86KCFIType(uint32_t Type);

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

/// Returns true if the CFGuard pass routes the KCFI checks of a module through
/// per-type thunks, which the backend emits: on COFF x86-64 and AArch64,
/// except Arm64EC, when the prefixes carry a marker.
LLVM_ABI bool hasKCFIThunks(const Module &M);

/// Returns true if Word can be the four bytes that precede the marker of a
/// KCFI prefix without a second type on x86: the padding emitted there, nops
/// or int3s. The word before a marker, a second type or a membership tag, is
/// never compared with one.
LLVM_ABI bool isX86TypePrefixPadding(uint32_t Word);

/// Returns a membership tag from a 64-bit hash of the members that carry it, or
/// std::nullopt if the tag cannot be told from the word that precedes the
/// marker of a function without one: zero, the sealed type, and the padding
/// x86 emits there.
LLVM_ABI std::optional<uint32_t> getKCFIMemberTag(uint64_t Hash);

/// Returns true if, in a module with KCFI thunks, a call to llvm.kcfi.check at
/// Offset goes through a per-type check thunk: the type word at offset 4, or
/// at offset 16 the second type that a function which can occupy a vtable
/// slot carries before its marker.
inline bool isKCFICheckThunkOffset(uint64_t Offset) {
  return Offset == 4 || Offset == 16;
}

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_KCFIHASH_H
