//===- OverrideTable.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Under -import-slots, an executable that replaces an overridable default
// carries one table in its header page, read by the C++ runtime's defaults
// through the PEB, and an anchor beside it, the second copy of every slot that
// the runtime validates at load. This file holds the version-1 layout both the
// runtime and this linker agree on, and the anchor chunk the linker writes.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_OVERRIDETABLE_H
#define LLD_COFF_OVERRIDETABLE_H

#include "Chunks.h"
#include <algorithm>
#include <cstdint>

namespace lld::coff {
class COFFLinkerContext;
class Defined;
class SymbolTable;

// The version-1 table, at a fixed offset from the image base in the header
// page. Slot k is {tag u32, rva u32} at 0x40 + 8k for the eight slots of the
// first cache line, 0x98 + 8(k - 8) after the header. The header holds the
// magic u32 and the version and slot count u16 at 0x80, the anchor's RVA u32
// at 0x88, the extension's RVA u32 at 0x8C, and a reserved u64 at 0x90, zero in
// version 1. Slots are never reused or renumbered, so a program and a runtime
// built at different times agree on every slot they both know.
inline constexpr uint32_t overrideTableVersion = 1;
inline constexpr uint32_t overrideTableMagic = 0x7B3AC2D5;
inline constexpr uint32_t overrideTagBase = 0xC5E1A7F3;
inline constexpr uint32_t overrideTableBase = 0x40;
inline constexpr uint32_t overrideHeaderOffset = 0x80;

// The tag that marks slot k in use.
inline uint32_t overrideTag(uint32_t k) { return overrideTagBase ^ k; }

// The offset of slot k from the image base.
inline uint32_t overrideSlotOffset(uint32_t k) {
  return k < 8 ? overrideTableBase + 8 * k : 0x98 + 8 * (k - 8);
}

// The end of a table of count slots: past its last slot, but never before the
// header and its fields, which a table of eight slots or fewer still holds.
inline uint32_t overrideTableEnd(uint32_t count) {
  return std::max<uint32_t>(0x98, overrideSlotOffset(count - 1) + 8);
}

// The anchor's header is the magic, version and slot count of its table, the
// mask of the slots that name a replacement rather than a forwarded one, and a
// reserved word; then come the slots' entries.
inline constexpr uint32_t overrideAnchorHeaderSize = 16;

// The anchor beside the table: its header, then for each slot the address its
// table entry names, as an image-relocated pointer, or null. The runtime
// reaches it through the table's anchor RVA and validates every slot against
// it, and its pointers make each body address-taken, so the guard function
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
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }
  StringRef getSectionName() const override { return ".ovra"; }

private:
  COFFLinkerContext &ctx;
  SymbolTable &symtab;
};

// The thunk a forwarded slot names when the default it forwards to drops the
// default's second argument, as a sized aligned deallocation forwards to the
// unsized one: it moves the third argument into the second and jumps to the
// target body. It moves no stack, so it needs no unwind entry, and carries no
// kcfi prefix, since only the replacing tail, built without kcfi, reaches it.
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

// Writes the override table into the header page, at base, which is the image
// base in the output: its slots, then its header with the magic, version,
// slot count, the anchor's RVA and a zero extension RVA and reserved word.
void writeOverrideTable(COFFLinkerContext &ctx, SymbolTable &symtab,
                        uint8_t *base);

} // namespace lld::coff

#endif
