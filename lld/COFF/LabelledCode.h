//===- LabelledCode.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_LABELLEDCODE_H
#define LLD_COFF_LABELLEDCODE_H

#include "Chunks.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <cassert>
#include <cstdint>

namespace lld::coff {
// A routine of fixed shape whose branches name labels bound before or after
// them, patched once the routine is written; with no buffer, only measured.
// An x86-64 jump takes an 8-bit displacement unless longJumps lists it.
class LabelledCode {
public:
  LabelledCode(uint8_t *buf, uint64_t rva, const llvm::BitVector &longJumps)
      : buf(buf), rva(rva), longJumps(longJumps) {}

  // Writes the routine emit puts at buf, or only measures it with no buffer.
  // Jumps start 8-bit and widen to 32 only where those do not reach (once
  // widened, staying so, bounding the loop); the layout uses no address, so
  // measuring and writing agree.
  static uint32_t layOut(uint8_t *buf, uint64_t rva,
                         llvm::function_ref<void(LabelledCode &)> emit) {
    llvm::BitVector longJumps;
    for (;;) {
      LabelledCode a(nullptr, rva, longJumps);
      emit(a);
      if (!a.lengthenJumps(longJumps)) {
        if (!buf)
          return a.off;
        break;
      }
    }
    LabelledCode a(buf, rva, longJumps);
    emit(a);
    return a.finish();
  }

  uint32_t newLabel() {
    labels.push_back(UINT32_MAX);
    return labels.size() - 1;
  }
  void bind(uint32_t label) { labels[label] = off; }
  void put(ArrayRef<uint8_t> bytes) {
    if (buf)
      memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  }
  void put32(uint32_t insn) {
    if (buf)
      llvm::support::endian::write32le(buf + off, insn);
    off += 4;
  }
  // The last four bytes put: a 32-bit immediate, or the RIP-relative
  // displacement to target of the instruction that ends with them.
  void imm32(uint32_t value) { at32(4, value); }
  void disp(uint64_t target) { at32(4, target - (rva + off)); }
  // The 32-bit or 64-bit field that ends back bytes before the end.
  void at32(uint32_t back, uint32_t value) {
    if (buf)
      llvm::support::endian::write32le(buf + off - back, value);
  }
  // x86-64: jcc to label; cc is the conditional jump's second opcode byte.
  void jcc(uint8_t cc, uint32_t label) {
    if (isLong(jumps))
      put({0x0F, cc, 0, 0, 0, 0});
    else
      put({uint8_t(cc - 0x10), 0});
    addJump(label);
  }
  // x86-64: jcc rel32 to target, an RVA outside the routine.
  void jccTo(uint8_t cc, uint64_t target) {
    put({0x0F, cc, 0, 0, 0, 0});
    disp(target);
  }
  // AArch64: an instruction with a 19-bit offset to label (b.cond, cbz, cbnz).
  void branch19(uint32_t insn, uint32_t label) {
    fixups.push_back({off, label, Imm19});
    put32(insn);
  }
  // adrp xN, target; add xN, xN, :lo12:target
  void address(uint32_t reg, uint64_t target) {
    if (!buf) {
      off += 8;
      return;
    }
    llvm::support::endian::write32le(buf + off, 0x90000000 | reg);
    llvm::support::endian::write32le(buf + off + 4,
                                     0x91000000 | reg << 5 | reg);
    applyArm64Addr(buf + off, target, rva + off, 12);
    applyArm64Imm(buf + off + 4, target & 0xfff, 0);
    off += 8;
  }
  // adrp xN, target; ldr xN, [xN, :lo12:target], or another load, ldr.
  void loadImport(uint32_t reg, uint64_t target, uint32_t ldr = 0xF9400000) {
    if (!buf) {
      off += 8;
      return;
    }
    llvm::support::endian::write32le(buf + off, 0x90000000 | reg);
    llvm::support::endian::write32le(buf + off + 4, ldr | reg << 5 | reg);
    applyArm64Addr(buf + off, target, rva + off, 12);
    applyArm64Ldr(buf + off + 4, target & 0xfff);
    off += 8;
  }
  uint32_t finish() {
    if (!buf)
      return off;
    for (const Fixup &f : fixups) {
      uint32_t target = labels[f.label];
      assert(target != UINT32_MAX && "unbound label");
      int64_t delta = int64_t(target) - int64_t(f.at);
      switch (f.kind) {
      case Rel8:
        buf[f.at] = uint8_t(delta - 1);
        break;
      case Rel32:
        llvm::support::endian::write32le(buf + f.at, uint32_t(delta - 4));
        break;
      case Imm19:
        llvm::support::endian::write32le(
            buf + f.at, llvm::support::endian::read32le(buf + f.at) |
                            uint32_t(delta / 4 & 0x7FFFF) << 5);
        break;
      }
    }
    return off;
  }

  uint8_t *buf;
  uint64_t rva;
  uint32_t off = 0;

private:
  enum Kind { Rel8, Rel32, Imm19 };
  struct Fixup {
    uint32_t at;
    uint32_t label;
    Kind kind;
    // The number of the jump, counted in the order the routine puts them.
    uint32_t jump = 0;
  };
  bool isLong(uint32_t jump) const {
    return jump < longJumps.size() && longJumps[jump];
  }
  // Records the displacement of the jump just put.
  void addJump(uint32_t label) {
    bool wide = isLong(jumps);
    fixups.push_back(
        {off - (wide ? 4 : 1), label, wide ? Rel32 : Rel8, jumps++});
  }
  // Adds to longJumps each jump whose 8-bit displacement does not reach its
  // label, and returns whether there was one.
  bool lengthenJumps(llvm::BitVector &longJumps) const {
    bool lengthened = false;
    for (const Fixup &f : fixups) {
      if (f.kind != Rel8 ||
          llvm::isInt<8>(int64_t(labels[f.label]) - int64_t(f.at + 1)))
        continue;
      if (longJumps.size() <= f.jump)
        longJumps.resize(jumps);
      longJumps.set(f.jump);
      lengthened = true;
    }
    return lengthened;
  }
  const llvm::BitVector &longJumps;
  uint32_t jumps = 0;
  SmallVector<uint32_t, 16> labels;
  SmallVector<Fixup, 32> fixups;
};
} // namespace lld::coff

#endif
