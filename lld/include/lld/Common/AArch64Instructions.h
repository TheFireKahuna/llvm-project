//===- AArch64Instructions.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Decoders and encoders of the AArch64 instructions that compute an address
// from a page and an offset in it, and of the loads that read through one, with
// the rewrites of such a sequence into a shorter one where the address is close
// enough.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_AARCH64INSTRUCTIONS_H
#define LLD_COMMON_AARCH64INSTRUCTIONS_H

#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <cassert>
#include <cstdint>

namespace lld::aarch64 {
using llvm::support::endian::read32le;
using llvm::support::endian::write32le;

struct Adrp {
  uint32_t destRegister;
  int64_t addend;
};

struct Add {
  uint8_t destRegister;
  uint8_t srcRegister;
  uint32_t addend;
};

enum ExtendType { ZeroExtend = 1, Sign64 = 2, Sign32 = 3 };

struct Ldr {
  uint8_t destRegister;
  uint8_t baseRegister;
  uint8_t p2Size;
  bool isFloat;
  ExtendType extendType;
  int64_t offset;
};

inline bool parseAdrp(uint32_t insn, Adrp &adrp) {
  if ((insn & 0x9f000000) != 0x90000000)
    return false;
  adrp.destRegister = insn & 0x1f;
  uint64_t immHi = (insn >> 5) & 0x7ffff;
  uint64_t immLo = (insn >> 29) & 0x3;
  adrp.addend = llvm::SignExtend64<21>(immLo | (immHi << 2)) * 4096;
  return true;
}

inline bool parseAdd(uint32_t insn, Add &add) {
  if ((insn & 0xffc00000) != 0x91000000)
    return false;
  add.destRegister = insn & 0x1f;
  add.srcRegister = (insn >> 5) & 0x1f;
  add.addend = (insn >> 10) & 0xfff;
  return true;
}

inline bool parseLdr(uint32_t insn, Ldr &ldr) {
  ldr.destRegister = insn & 0x1f;
  ldr.baseRegister = (insn >> 5) & 0x1f;
  uint8_t size = insn >> 30;
  uint8_t opc = (insn >> 22) & 3;

  if ((insn & 0x3fc00000) == 0x39400000) {
    // LDR (immediate), LDRB (immediate), LDRH (immediate)
    ldr.p2Size = size;
    ldr.extendType = ZeroExtend;
    ldr.isFloat = false;
  } else if ((insn & 0x3f800000) == 0x39800000) {
    // LDRSB (immediate), LDRSH (immediate), LDRSW (immediate)
    ldr.p2Size = size;
    ldr.extendType = static_cast<ExtendType>(opc);
    ldr.isFloat = false;
  } else if ((insn & 0x3f400000) == 0x3d400000) {
    // LDR (immediate, SIMD&FP)
    ldr.extendType = ZeroExtend;
    ldr.isFloat = true;
    if (opc == 1)
      ldr.p2Size = size;
    else if (size == 0 && opc == 3)
      ldr.p2Size = 4;
    else
      return false;
  } else {
    return false;
  }
  ldr.offset = ((insn >> 10) & 0xfff) << ldr.p2Size;
  return true;
}

inline bool isValidAdrOffset(int64_t delta) { return llvm::isInt<21>(delta); }

inline void writeAdr(void *loc, uint32_t dest, int32_t delta) {
  assert(isValidAdrOffset(delta));
  uint32_t opcode = 0x10000000;
  uint32_t immHi = (delta & 0x001ffffc) << 3;
  uint32_t immLo = (delta & 0x00000003) << 29;
  write32le(loc, opcode | immHi | immLo | dest);
}

inline void writeNop(void *loc) { write32le(loc, 0xd503201f); }

inline bool isLiteralLdrEligible(const Ldr &ldr) {
  return ldr.p2Size > 1 && llvm::isShiftedInt<19, 2>(ldr.offset);
}

inline void writeLiteralLdr(void *loc, const Ldr &ldr) {
  assert(isLiteralLdrEligible(ldr));
  uint32_t imm19 = (ldr.offset / 4 & llvm::maskTrailingOnes<uint32_t>(19)) << 5;
  uint32_t opcode;
  switch (ldr.p2Size) {
  case 2:
    if (ldr.isFloat)
      opcode = 0x1c000000;
    else
      opcode = ldr.extendType == Sign64 ? 0x98000000 : 0x18000000;
    break;
  case 3:
    opcode = ldr.isFloat ? 0x5c000000 : 0x58000000;
    break;
  case 4:
    opcode = 0x9c000000;
    break;
  default:
    llvm_unreachable("Invalid literal ldr size");
  }
  write32le(loc, opcode | imm19 | ldr.destRegister);
}

inline bool isImmediateLdrEligible(const Ldr &ldr) {
  // Note: We deviate from ld64's behavior, which converts to immediate loads
  // only if ldr.offset < 4096, even though the offset is divided by the load's
  // size in the 12-bit immediate operand. Only the unsigned offset variant is
  // supported.

  uint32_t size = 1 << ldr.p2Size;
  return ldr.offset >= 0 && (ldr.offset % size) == 0 &&
         llvm::isUInt<12>(ldr.offset >> ldr.p2Size);
}

inline void writeImmediateLdr(void *loc, const Ldr &ldr) {
  assert(isImmediateLdrEligible(ldr));
  uint32_t opcode = 0x39000000;
  if (ldr.isFloat) {
    opcode |= 0x04000000;
    assert(ldr.extendType == ZeroExtend);
  }
  opcode |= ldr.destRegister;
  opcode |= ldr.baseRegister << 5;
  uint8_t size, opc;
  if (ldr.p2Size == 4) {
    size = 0;
    opc = 3;
  } else {
    opc = ldr.extendType;
    size = ldr.p2Size;
  }
  uint32_t immBits = ldr.offset >> ldr.p2Size;
  write32le(loc, opcode | (immBits << 10) | (opc << 22) | (size << 30));
}

inline uint64_t pageBits(uint64_t address) { return address & ~0xfffull; }

// The rewrites below take the offsets of the instructions in buf, which holds
// the contents of a section whose address is va.

// Transforms a pair of adrp+add instructions into an adr instruction if the
// target is within the +/- 1 MiB range allowed by the adr's 21 bit signed
// immediate offset.
//
//   adrp xN, _foo@PAGE
//   add  xM, xN, _foo@PAGEOFF
// ->
//   adr  xM, _foo
//   nop
inline bool applyAdrpAdd(uint8_t *buf, uint64_t va, uint64_t offset1,
                         uint64_t offset2) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  Adrp adrp;
  Add add;
  if (!parseAdrp(ins1, adrp) || !parseAdd(ins2, add))
    return false;
  if (adrp.destRegister != add.srcRegister)
    return false;

  uint64_t addr1 = va + offset1;
  uint64_t referent = pageBits(addr1) + adrp.addend + add.addend;
  int64_t delta = referent - addr1;
  if (!isValidAdrOffset(delta))
    return false;

  writeAdr(buf + offset1, add.destRegister, delta);
  writeNop(buf + offset2);
  return true;
}

// Transforms two adrp instructions into a single adrp if their referent
// addresses are located on the same 4096 byte page.
//
//   adrp xN, _foo@PAGE
//   adrp xN, _bar@PAGE
// ->
//   adrp xN, _foo@PAGE
//   nop
inline void applyAdrpAdrp(uint8_t *buf, uint64_t va, uint64_t offset1,
                          uint64_t offset2) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  Adrp adrp1, adrp2;
  if (!parseAdrp(ins1, adrp1) || !parseAdrp(ins2, adrp2))
    return;
  if (adrp1.destRegister != adrp2.destRegister)
    return;

  uint64_t page1 = pageBits(offset1 + va) + adrp1.addend;
  uint64_t page2 = pageBits(offset2 + va) + adrp2.addend;
  if (page1 != page2)
    return;

  writeNop(buf + offset2);
}

// Transforms a pair of adrp+ldr (immediate) instructions into an ldr (literal)
// load from a PC-relative address if it is 4-byte aligned and within +/- 1 MiB,
// as ldr can encode a signed 19-bit offset that gets multiplied by 4.
//
//   adrp xN, _foo@PAGE
//   ldr  xM, [xN, _foo@PAGEOFF]
// ->
//   nop
//   ldr  xM, _foo
inline void applyAdrpLdr(uint8_t *buf, uint64_t va, uint64_t offset1,
                         uint64_t offset2) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  Adrp adrp;
  Ldr ldr;
  if (!parseAdrp(ins1, adrp) || !parseLdr(ins2, ldr))
    return;
  if (adrp.destRegister != ldr.baseRegister)
    return;

  uint64_t addr1 = va + offset1;
  uint64_t addr2 = va + offset2;
  uint64_t referent = pageBits(addr1) + adrp.addend + ldr.offset;
  ldr.offset = referent - addr2;
  if (!isLiteralLdrEligible(ldr))
    return;

  writeNop(buf + offset1);
  writeLiteralLdr(buf + offset2, ldr);
}

// GOT loads are emitted by the compiler as a pair of adrp and ldr instructions,
// but they may be changed to adrp+add by relaxGotLoad(). This hint performs
// the AdrpLdr or AdrpAdd transformation depending on whether it was relaxed.
inline void applyAdrpLdrGot(uint8_t *buf, uint64_t va, uint64_t offset1,
                            uint64_t offset2) {
  uint32_t ins2 = read32le(buf + offset2);
  Add add;
  Ldr ldr;
  if (parseAdd(ins2, add))
    applyAdrpAdd(buf, va, offset1, offset2);
  else if (parseLdr(ins2, ldr))
    applyAdrpLdr(buf, va, offset1, offset2);
}

// Optimizes an adrp+add+ldr sequence used for loading from a local symbol's
// address by loading directly if it's close enough, or to an adrp(p)+ldr
// sequence if it's not.
//
//   adrp x0, _foo@PAGE
//   add  x1, x0, _foo@PAGEOFF
//   ldr  x2, [x1, #off]
inline void applyAdrpAddLdr(uint8_t *buf, uint64_t va, uint64_t offset1,
                            uint64_t offset2, uint64_t offset3) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  uint32_t ins3 = read32le(buf + offset3);
  Adrp adrp;
  Add add;
  Ldr ldr;
  if (!parseAdrp(ins1, adrp) || !parseAdd(ins2, add) || !parseLdr(ins3, ldr))
    return;
  if (adrp.destRegister != add.srcRegister)
    return;
  if (add.destRegister != ldr.baseRegister)
    return;

  // Load from the target address directly.
  //   nop
  //   nop
  //   ldr x2, [_foo + #off]
  uint64_t addr1 = va + offset1;
  uint64_t addr3 = va + offset3;
  uint64_t referent = pageBits(addr1) + adrp.addend + add.addend;
  Ldr literalLdr = ldr;
  literalLdr.offset += referent - addr3;
  if (isLiteralLdrEligible(literalLdr)) {
    writeNop(buf + offset1);
    writeNop(buf + offset2);
    writeLiteralLdr(buf + offset3, literalLdr);
    return;
  }

  if (applyAdrpAdd(buf, va, offset1, offset2))
    return;

  // Move the target's page offset into the ldr's immediate offset.
  //   adrp x0, _foo@PAGE
  //   nop
  //   ldr x2, [x0, _foo@PAGEOFF + #off]
  Ldr immediateLdr = ldr;
  immediateLdr.baseRegister = adrp.destRegister;
  immediateLdr.offset += add.addend;
  if (isImmediateLdrEligible(immediateLdr)) {
    writeNop(buf + offset2);
    writeImmediateLdr(buf + offset3, immediateLdr);
    return;
  }
}

// Relaxes a GOT-indirect load.
// If the referenced symbol is external and its GOT entry is within +/- 1 MiB,
// the GOT entry can be loaded with a single literal ldr instruction.
// If the referenced symbol is local and thus has been relaxed to adrp+add+ldr,
// we perform the AdrpAddLdr transformation.
inline void applyAdrpLdrGotLdr(uint8_t *buf, uint64_t va, uint64_t offset1,
                               uint64_t offset2, uint64_t offset3) {
  uint32_t ins2 = read32le(buf + offset2);
  Add add;
  Ldr ldr2;

  if (parseAdd(ins2, add)) {
    applyAdrpAddLdr(buf, va, offset1, offset2, offset3);
  } else if (parseLdr(ins2, ldr2)) {
    // adrp x1, _foo@GOTPAGE
    // ldr  x2, [x1, _foo@GOTPAGEOFF]
    // ldr  x3, [x2, #off]
    uint32_t ins3 = read32le(buf + offset3);
    Ldr ldr3;
    if (!parseLdr(ins3, ldr3))
      return;
    if (ldr3.baseRegister != ldr2.destRegister)
      return;
    applyAdrpLdr(buf, va, offset1, offset2);
  }
}
} // namespace lld::aarch64

#endif
