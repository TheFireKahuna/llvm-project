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

MachineTypes OverrideCellFillChunk::getMachine() const {
  return ctx.config.machine;
}

// The x86-64 fill writes each replaced cell with base + rva, read from the
// executable's table; each AArch64 cell takes three more instructions for the
// store address. Both return unless called for process attach, then read the
// executable's image base through the PEB and compare the table's magic, so a
// foreign executable with no table leaves every cell the loader's value.
size_t OverrideCellFillChunk::getSize() const {
  if (ctx.config.machine == ARM64) {
    // cmp w1,#1; b.ne .ret; ldr x16,[x18,#0x60]; ldr x16,[x16,#0x10];
    // ldr w17,[x16,#0x80]; movz/movk w9,#magic; cmp w17,w9; b.ne .ret (nine
    // words), then per cell ldr w17,[x16,#off]; movz/movk w9,#tag; cmp; b.ne;
    // ldr w17,[x16,#off+4]; add x17,x16,x17; adrp x9; add x9; str x17,[x9]
    // (ten words), then ret.
    return 36 + 40 * cells.size() + 4;
  }
  // cmp edx, 1; jne .ret; PEB, base, cmp magic (disp32), jne .ret
  size_t size = 3 + 6 + 9 + 4 + 11 + 6;
  for (const OverrideCell &c : cells) {
    bool wide = overrideSlotOffset(c.slot) >= 0x80;
    size += (wide ? 11 : 8) + 2 + (wide ? 7 : 4) + 3 + 7;
  }
  return size + 1; // ret
}

void OverrideCellFillChunk::writeTo(uint8_t *buf) const {
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    auto movImm = [&](uint32_t reg, uint32_t value) {
      put(0x52800000 | reg | (value & 0xFFFF) << 5);       // movz w, #lo
      put(0x72A00000 | reg | (value >> 16 & 0xFFFF) << 5); // movk w, #hi, lsl 16
    };
    put(0x7100043F);         // cmp w1, #1   the reason: process attach
    uint32_t attachBranch = off;
    put(0x54000001);         // b.ne .ret
    put(0xF9403010);         // ldr x16, [x18, #0x60]   TEB->PEB
    put(0xF9400A10);         // ldr x16, [x16, #0x10]   PEB->ImageBaseAddress
    put(0xB9408211);         // ldr w17, [x16, #0x80]   the table's magic
    movImm(9, overrideTableMagic);
    put(0x6B09023F);         // cmp w17, w9
    uint32_t retBranch = off; // patched to branch to the ret
    put(0x54000001);         // b.ne .ret
    for (const OverrideCell &c : cells) {
      uint32_t slotOff = overrideSlotOffset(c.slot);
      put(0xB9400011 | slotOff / 4 << 10); // ldr w17, [x16, #slotOff]  the tag
      movImm(9, overrideTag(c.slot));
      put(0x6B09023F);                     // cmp w17, w9
      uint32_t skip = off;
      put(0x54000001);                     // b.ne .next
      put(0xB9400011 | (slotOff + 4) / 4 << 10); // ldr w17, [x16, #slotOff+4]
      put(0x8B110211);                           // add x17, x16, x17
      uint64_t cell = c.chunk->getRVA() + c.offset;
      write32le(buf + off, 0x90000009); // adrp x9, cell
      write32le(buf + off + 4, 0x91000129);                // add x9, x9, :lo12:
      applyArm64Addr(buf + off, cell, rva + off, 12);
      applyArm64Imm(buf + off + 4, cell & 0xFFF, 0);
      off += 8;
      put(0xF9000131);                           // str x17, [x9]
      write32le(buf + skip, 0x54000001 | (off - skip) / 4 << 5); // b.ne .next
    }
    write32le(buf + attachBranch, 0x54000001 | (off - attachBranch) / 4 << 5);
    write32le(buf + retBranch, 0x54000001 | (off - retBranch) / 4 << 5);
    put(0xD65F03C0); // ret
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  // The RIP-relative displacement to target of the instruction just put.
  auto disp = [&](uint64_t target) {
    write32le(buf + off - 4, target - (rva + off));
  };
  put({0x83, 0xFA, 0x01});       // cmp edx, 1   the reason: process attach
  put({0x0F, 0x85, 0, 0, 0, 0}); // jne .ret
  uint32_t attachFixup = off - 4;
  put({0x65, 0x4C, 0x8B, 0x1C, 0x25, 0x60, 0, 0, 0}); // mov r11, gs:[0x60]
  put({0x4D, 0x8B, 0x5B, 0x10});                       // mov r11, [r11 + 0x10]
  // The magic is at 0x80, past the reach of a signed disp8, so a disp32.
  put({0x41, 0x81, 0xBB, 0x80, 0, 0, 0, 0, 0, 0, 0}); // cmp [r11 + 0x80], magic
  write32le(buf + off - 4, overrideTableMagic);
  put({0x0F, 0x85, 0, 0, 0, 0}); // jne .ret
  uint32_t retFixup = off - 4;
  for (const OverrideCell &c : cells) {
    uint32_t slotOff = overrideSlotOffset(c.slot);
    bool wide = slotOff >= 0x80;
    // cmp dword ptr [r11 + slotOff], tag
    if (wide) {
      put({0x41, 0x81, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0});
      write32le(buf + off - 8, slotOff);
      write32le(buf + off - 4, overrideTag(c.slot));
    } else {
      put({0x41, 0x81, 0x7B, uint8_t(slotOff), 0, 0, 0, 0});
      write32le(buf + off - 4, overrideTag(c.slot));
    }
    put({0x75, 0}); // jne .next
    uint32_t skipFixup = off - 1;
    // mov eax, dword ptr [r11 + slotOff + 4]   the slot's RVA
    if (wide) {
      put({0x41, 0x8B, 0x83, 0, 0, 0, 0});
      write32le(buf + off - 4, slotOff + 4);
    } else {
      put({0x41, 0x8B, 0x43, uint8_t(slotOff + 4)});
    }
    put({0x4C, 0x01, 0xD8});             // add rax, r11   base + rva
    put({0x48, 0x89, 0x05, 0, 0, 0, 0}); // mov [rip + cell], rax
    disp(c.chunk->getRVA() + c.offset);
    buf[skipFixup] = off - (skipFixup + 1);
  }
  write32le(buf + attachFixup, off - (attachFixup + 4));
  write32le(buf + retFixup, off - (retFixup + 4)); // jne .ret -> the ret below
  put({0xC3});                                     // ret
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
