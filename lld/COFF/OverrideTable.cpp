//===- OverrideTable.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OverrideTable.h"
#include "COFFLinkerContext.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::support::endian;

namespace lld::coff {

size_t OverrideAnchorChunk::getSize() const {
  return overrideAnchorHeaderSize + 8 * symtab.overrideSlots.size();
}

void OverrideAnchorChunk::writeTo(uint8_t *buf) const {
  uint32_t count = symtab.overrideSlots.size();
  write32le(buf, overrideTableMagic);
  write16le(buf + 4, overrideTableVersion);
  write16le(buf + 6, count);
  write32le(buf + 8, symtab.overrideMask);
  write32le(buf + 12, 0);
  for (uint32_t k = 0; k != count; ++k) {
    const auto &slot = symtab.overrideSlots[k];
    write64le(buf + overrideAnchorHeaderSize + 8 * k,
              slot.target ? ctx.config.imageBase + slot.target->getRVA() : 0);
  }
}

void OverrideAnchorChunk::getBaserels(std::vector<Baserel> *res) {
  uint32_t count = symtab.overrideSlots.size();
  for (uint32_t k = 0; k != count; ++k)
    if (symtab.overrideSlots[k].target)
      res->emplace_back(rva + overrideAnchorHeaderSize + 8 * k,
                        ctx.config.machine);
}

MachineTypes OverrideThunkChunk::getMachine() const {
  return ctx.config.machine;
}

void OverrideThunkChunk::writeTo(uint8_t *buf) const {
  if (ctx.config.machine == ARM64) {
    write32le(buf, 0xAA0203E1);                       // mov x1, x2
    write32le(buf + 4, 0x14000000 |                   // b target
                           ((target->getRVA() - rva - 4) >> 2 & 0x3FFFFFF));
    return;
  }
  // MS x64, where the deallocation's arguments are the pointer, the size and
  // the alignment in rcx, rdx and r8; dropping the size moves r8 into rdx.
  static const uint8_t thunk[] = {0x4C, 0x89, 0xC2,          // mov rdx, r8
                                  0xE9, 0, 0, 0, 0};         // jmp target
  memcpy(buf, thunk, sizeof(thunk));
  write32le(buf + 4, target->getRVA() - rva - getSize());
}

void writeOverrideTable(COFFLinkerContext &ctx, SymbolTable &symtab,
                        uint8_t *base) {
  uint32_t count = symtab.overrideSlots.size();
  for (uint32_t k = 0; k != count; ++k) {
    const auto &slot = symtab.overrideSlots[k];
    uint8_t *entry = base + overrideSlotOffset(k);
    write32le(entry, slot.target ? overrideTag(k) : 0);
    write32le(entry + 4, slot.target ? slot.target->getRVA() : 0);
  }
  uint8_t *header = base + overrideHeaderOffset;
  write32le(header, overrideTableMagic);
  write16le(header + 4, overrideTableVersion);
  write16le(header + 6, count);
  write32le(header + 8, symtab.overrideAnchor->getRVA());
  write32le(header + 12, 0);
  write64le(header + 16, 0);
}

} // namespace lld::coff
