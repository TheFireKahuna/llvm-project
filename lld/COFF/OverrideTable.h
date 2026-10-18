//===- OverrideTable.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Under -import-slots, an executable that replaces an overridable default
// carries a table in its header page (read by the C++ runtime through the PEB)
// and, beside it, an anchor the runtime validates at load. This file holds the
// version-1 layout and the anchor chunk the linker writes.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_OVERRIDETABLE_H
#define LLD_COFF_OVERRIDETABLE_H

#include "Chunks.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <cstdint>

namespace lld::coff {
class COFFLinkerContext;
class Defined;
class SymbolTable;

// The version-1 table, at a fixed offset from the image base in the header
// page: slots of {tag u32, rva u32} (overrideSlotOffset) and a header at 0x80
// (magic, version, slot count, anchor RVA at 0x88, extension RVA at 0x8C, a
// reserved u64 at 0x90). Slots are never reused or renumbered, so programs and
// runtimes built at different times agree on every slot they share.
inline constexpr uint32_t overrideTableVersion = 1;
inline constexpr uint32_t overrideTableMagic = 0x7B3AC2D5;
inline constexpr uint32_t overrideTagBase = 0xC5E1A7F3;
inline constexpr uint32_t overrideTableBase = 0x40;
inline constexpr uint32_t overrideHeaderOffset = 0x80;

// The table's block, at a fixed offset in the header page, holds the
// name-replaced defaults with no fixed slot and the vague entities whose owner
// the executable picks. Header: magic u32, entry count u16, overflow RVA and
// length u32, two reserved u64; then the buckets of {key u64, rva u32, info
// u32}, info bit 0 set where rva names a word holding the address (a vague
// entity). An entry takes the first free bucket from its home, so a free
// bucket ends a search; when all are taken, entries spill to an overflow list
// in the anchor's section.
inline constexpr uint32_t overrideBlockOffset = 0x7E0;
inline constexpr uint32_t overrideBucketsOffset = 0x800;
inline constexpr uint32_t overrideBucketCount = 64;
inline constexpr uint32_t overrideBlockEnd = 0xC00;
inline constexpr uint32_t overrideBlockMagic = 0x2F1B8E57;
inline constexpr uint32_t overrideBlockEntrySize = 16;

inline uint64_t overrideNameKey(StringRef name) {
  return llvm::xxh3_64bits(name) & ~uint64_t(1);
}
inline uint32_t overrideHomeBucket(uint64_t key) { return key >> 58; }

// Prefix a DLL exports its strong definition of an overridable default under,
// so no importer binds it as the ABI symbol: it is an offer, not a replacement.
inline constexpr llvm::StringLiteral overrideOfferPrefix = "__ovr_offer$";

// The tag that marks slot k in use.
inline uint32_t overrideTag(uint32_t k) { return overrideTagBase ^ k; }

// The offset of slot k from the image base.
inline uint32_t overrideSlotOffset(uint32_t k) {
  return k < 8 ? overrideTableBase + 8 * k : 0x98 + 8 * (k - 8);
}

// End of a count-slot table: past its last slot, but never before the header.
inline uint32_t overrideTableEnd(uint32_t count) {
  return std::max<uint32_t>(0x98, overrideSlotOffset(count - 1) + 8);
}

// The anchor's header: its table's magic, version and slot count, the mask of
// slots naming a replacement (not a forward), and the block's entry count.
inline constexpr uint32_t overrideAnchorHeaderSize = 16;

// The anchor beside the table: its header, then each slot's address as an
// image-relocated pointer (or null), then each block entry's key and address,
// then the block's overflow list. The runtime validates every slot and entry
// against it; its pointers make each body address-taken, so the guard function
// table lists them.
class OverrideAnchorChunk : public NonSectionChunk {
public:
  explicit OverrideAnchorChunk(COFFLinkerContext &ctx, SymbolTable &symtab)
      : ctx(ctx), symtab(symtab) {
    setAlignment(8);
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  void getBaserels(std::vector<Baserel> *res) override;
  // The offsets of the block's entries and of its overflow list.
  uint32_t getEntriesOffset() const;
  uint32_t getOverflowOffset() const;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }
  StringRef getSectionName() const override { return ".ovra"; }

private:
  COFFLinkerContext &ctx;
  SymbolTable &symtab;
};

// The thunk a forwarded slot names when its target drops the default's second
// argument (as sized aligned delete forwards to the unsized): moves the third
// argument into the second and jumps to the target. No stack move, so no
// unwind entry; no kcfi prefix, as only the replacing tail reaches it.
class OverrideThunkChunk : public NonSectionCodeChunk {
public:
  OverrideThunkChunk(COFFLinkerContext &ctx, Defined *target)
      : ctx(ctx), target(target) {
    setAlignment(16);
  }
  size_t getSize() const override { return 8; }
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override;

private:
  COFFLinkerContext &ctx;
  Defined *target;
};

// Writes the override table into the header page at base (the image base):
// the slots, the header (magic, version, slot count, anchor RVA, block RVA or
// zero, reserved word), then the block if any.
void writeOverrideTable(COFFLinkerContext &ctx, SymbolTable &symtab,
                        uint8_t *base);

} // namespace lld::coff

#endif
