//===- COFFBinding.h - COFF semantic binding records --------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BINARYFORMAT_COFFBINDING_H
#define LLVM_BINARYFORMAT_COFFBINDING_H

#include <cstdint>

namespace llvm::COFF {

// .llvm.bind is link-only, little-endian data. Its header contains the format
// version and RTTI ABI version, followed by packed (uint32_t COFF symbol index,
// uint8_t flags) pairs. Records have no alignment padding.
// Symbol indices use the existing .symidx fixup: records need neither COFF
// relocations nor auxiliary symbols and do not keep their targets alive.
inline constexpr uint32_t BindingVersion = 1;
inline constexpr uint32_t RTTIABI = 2;
inline constexpr uint32_t BindingHeaderSize = 8;
inline constexpr uint32_t BindingRecordSize = 5;

// .llvm.part contains a version followed by groups of a NUL-terminated
// partition name, ULEB128 symbol count and that many uint32_t symbol indices.
// One string per partition, no relocations, padding or per-root sections.
// .llvm.place uses the same encoding for frozen LTO placements, allowing an
// empty name for the main image. Its entries do not create exports or GC roots.
inline constexpr uint32_t PartitionVersion = 1;

// .llvm.extent describes actual emitted data objects, excluding inter-object
// alignment. A u32 version precedes (u32 symbol index, ULEB128 byte size) rows.
// It supplies bounds for demand-driven exact exports, not GC roots or aliases.
inline constexpr uint32_t ObjectExtentVersion = 1;

// .llvm.abi is a sparse contract table. A u32 version precedes groups of
// ULEB128 byte length, canonical contract bytes, ULEB128 symbol count, and
// packed (u32 symbol index, u8 kind) entries. Kinds 0/1 are declarations and
// definitions. Kind 2 additionally carries a u32 consumer symbol index: its
// requirement stays live with that contribution even without a relocation.
// Equal contracts are emitted once per object. There are no relocations,
// unconditional roots, per-use sections or digest copies.
inline constexpr uint32_t ABIContractSectionVersion = 2;

enum BindingFlags : uint8_t {
  BindingCanonical = 1,
  BindingRTTI = 2,
  BindingName = 4,
};

constexpr bool isValidBindingFlags(uint32_t Flags) {
  return !(Flags & ~(BindingCanonical | BindingRTTI | BindingName)) &&
         ((Flags & (BindingRTTI | BindingName)) == BindingRTTI ||
          (Flags & (BindingRTTI | BindingName)) == BindingName);
}

} // namespace llvm::COFF

#endif
