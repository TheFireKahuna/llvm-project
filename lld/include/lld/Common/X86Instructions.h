//===- X86Instructions.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Encodings of the x86-64 instructions a linker writes or rewrites once it
// knows the layout: nops, instructions that read a GOT entry, and conditional
// jumps.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_X86INSTRUCTIONS_H
#define LLD_COMMON_X86INSTRUCTIONS_H

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <cassert>
#include <cstdint>
#include <optional>

namespace lld::x86 {
// The NOP instructions of 1 to 9 bytes: nops[n - 1] is n bytes long.
inline constexpr uint8_t nops[][9] = {
    {0x90},
    {0x66, 0x90},
    {0x0f, 0x1f, 0x00},
    {0x0f, 0x1f, 0x40, 0x00},
    {0x0f, 0x1f, 0x44, 0x00, 0x00},
    {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}};

// What an instruction that reads a GOT entry through a RIP-relative operand can
// become where the entry's value is known at link time.
enum class GotLoad {
  // mov, call and jmp, which address the symbol RIP-relatively instead.
  PcRelative,
  // test, adc, add, and, cmp, or, sbb, sub and xor, which take the symbol's
  // absolute address as an immediate. Only those with a REX or REX2 prefix can
  // be rewritten.
  Absolute,
};

// Classifies the instruction with opcode op and ModRM byte modRm.
inline GotLoad classifyGotLoad(uint8_t op, uint8_t modRm) {
  // FIXME: When PIC is disabled and foo is defined locally in the
  // lower 32 bit address space, memory operand in mov can be converted into
  // immediate operand. Otherwise, mov must be changed to lea. We support only
  // latter relaxation at this moment.
  if (op == 0x8b || (op == 0xff && (modRm == 0x15 || modRm == 0x25)))
    return GotLoad::PcRelative;
  return GotLoad::Absolute;
}

// A subset of relaxations can only be applied for no-PIC. This method
// handles such relaxations. Instructions encoding information was taken from:
// "Intel 64 and IA-32 Architectures Software Developer's Manual V2"
// (http://www.intel.com/content/dam/www/public/us/en/documents/manuals/
//    64-ia-32-architectures-software-developer-instruction-set-reference-manual-325383.pdf)
inline void relaxGotNoPic(uint8_t *loc, uint64_t val, uint8_t op, uint8_t modRm,
                          bool isRex2) {
  const uint8_t rex = loc[-3];
  // Convert "test %reg, foo@GOTPCREL(%rip)" to "test $foo, %reg".
  if (op == 0x85) {
    // See "TEST-Logical Compare" (4-428 Vol. 2B),
    // TEST r/m64, r64 uses "full" ModR / M byte (no opcode extension).

    // ModR/M byte has form XX YYY ZZZ, where
    // YYY is MODRM.reg(register 2), ZZZ is MODRM.rm(register 1).
    // XX has different meanings:
    // 00: The operand's memory address is in reg1.
    // 01: The operand's memory address is reg1 + a byte-sized displacement.
    // 10: The operand's memory address is reg1 + a word-sized displacement.
    // 11: The operand is reg1 itself.
    // If an instruction requires only one operand, the unused reg2 field
    // holds extra opcode bits rather than a register code
    // 0xC0 == 11 000 000 binary.
    // 0x38 == 00 111 000 binary.
    // We transfer reg2 to reg1 here as operand.
    // See "2.1.3 ModR/M and SIB Bytes" (Vol. 2A 2-3).
    loc[-1] = 0xc0 | (modRm & 0x38) >> 3; // ModR/M byte.

    // Change opcode from TEST r/m64, r64 to TEST r/m64, imm32
    // See "TEST-Logical Compare" (4-428 Vol. 2B).
    loc[-2] = 0xf7;

    // Move R bit to the B bit in REX/REX2 byte.
    // REX byte is encoded as 0100WRXB, where
    // 0100 is 4bit fixed pattern.
    // REX.W When 1, a 64-bit operand size is used. Otherwise, when 0, the
    //   default operand size is used (which is 32-bit for most but not all
    //   instructions).
    // REX.R This 1-bit value is an extension to the MODRM.reg field.
    // REX.X This 1-bit value is an extension to the SIB.index field.
    // REX.B This 1-bit value is an extension to the MODRM.rm field or the
    // SIB.base field.
    // See "2.2.1.2 More on REX Prefix Fields " (2-8 Vol. 2A).
    //
    // REX2 prefix is encoded as 0xd5|M|R2|X2|B2|WRXB, where
    // 0xd5 is 1byte fixed pattern.
    // REX2's [W,R,X,B] have the same meanings as REX's.
    // REX2.M encodes the map id.
    // R2/X2/B2 provides the fifth and most siginicant bits of the R/X/B
    // register identifiers, each of which can now address all 32 GPRs.
    if (isRex2)
      loc[-3] = (rex & ~0x44) | (rex & 0x44) >> 2;
    else
      loc[-3] = (rex & ~0x4) | (rex & 0x4) >> 2;
    llvm::support::endian::write32le(loc, val);
    return;
  }

  // If we are here then we need to relax the adc, add, and, cmp, or, sbb, sub
  // or xor operations.

  // Convert "binop foo@GOTPCREL(%rip), %reg" to "binop $foo, %reg".
  // Logic is close to one for test instruction above, but we also
  // write opcode extension here, see below for details.
  loc[-1] = 0xc0 | (modRm & 0x38) >> 3 | (op & 0x3c); // ModR/M byte.

  // Primary opcode is 0x81, opcode extension is one of:
  // 000b = ADD, 001b is OR, 010b is ADC, 011b is SBB,
  // 100b is AND, 101b is SUB, 110b is XOR, 111b is CMP.
  // This value was wrote to MODRM.reg in a line above.
  // See "3.2 INSTRUCTIONS (A-M)" (Vol. 2A 3-15),
  // "INSTRUCTION SET REFERENCE, N-Z" (Vol. 2B 4-1) for
  // descriptions about each operation.
  loc[-2] = 0x81;
  if (isRex2)
    loc[-3] = (rex & ~0x44) | (rex & 0x44) >> 2;
  else
    loc[-3] = (rex & ~0x4) | (rex & 0x4) >> 2;
  llvm::support::endian::write32le(loc, val);
}

// Rewrites "mov foo(%rip), %reg", a 64-bit load whose prefix is REX, or REX2
// when isRex2, into "mov $imm32, %reg" of the same length, whose immediate is
// then written at loc, the address of the displacement.
inline void relaxMovToImm(uint8_t *loc, bool isRex2) {
  const uint8_t rex = loc[-3];
  if (isRex2)
    loc[-3] = (rex & ~0x44) | (rex & 0x44) >> 2;
  else
    loc[-3] = (rex & ~0x4) | (rex & 0x4) >> 2;
  loc[-2] = 0xc7;
  loc[-1] = 0xc0 | loc[-1] >> 3;
}

// Rewrites an instruction that classifyGotLoad accepts so that it no longer
// reads the GOT entry. loc is the address of its 32-bit displacement and val
// the value to write there; isRex2 says that its prefix is REX2.
inline void relaxGot(uint8_t *loc, uint64_t val, bool isRex2) {
  assert(llvm::isInt<32>(val) &&
         "GOTPCRELX should not have been relaxed if it overflows");
  const uint8_t op = loc[-2];
  const uint8_t modRm = loc[-1];

  // Convert "mov foo@GOTPCREL(%rip),%reg" to "lea foo(%rip),%reg".
  if (op == 0x8b) {
    loc[-2] = 0x8d;
    llvm::support::endian::write32le(loc, val);
    return;
  }

  if (op != 0xff) {
    // We are relaxing a rip relative to an absolute, so compensate
    // for the old -4 addend.
    relaxGotNoPic(loc, val + 4, op, modRm, isRex2);
    return;
  }

  // Convert call/jmp instructions.
  if (modRm == 0x15) {
    // ABI says we can convert "call *foo@GOTPCREL(%rip)" to "nop; call foo".
    // Instead we convert to "addr32 call foo" where addr32 is an instruction
    // prefix. That makes result expression to be a single instruction.
    loc[-2] = 0x67; // addr32 prefix
    loc[-1] = 0xe8; // call
    llvm::support::endian::write32le(loc, val);
    return;
  }

  // Convert "jmp *foo@GOTPCREL(%rip)" to "jmp foo; nop".
  // jmp doesn't return, so it is fine to use nop here, it is just a stub.
  assert(modRm == 0x25);
  loc[-2] = 0xe9; // jmp
  loc[3] = 0x90;  // nop
  llvm::support::endian::write32le(loc - 1, val + 1);
}

// A conditional jump is "70+cc rel8" or "0f 80+cc rel32", where cc is one of
// the sixteen condition codes and cc ^ 1 is the opposite condition. The helpers
// below take the address of the displacement.

// Returns the condition code of a "jcc rel32", or std::nullopt for any other
// instruction.
inline std::optional<uint8_t> getJccCondition(const uint8_t *loc) {
  if (loc[-2] != 0x0f || (loc[-1] & 0xf0) != 0x80)
    return std::nullopt;
  return loc[-1] & 0xf;
}

inline uint8_t invertCondition(uint8_t cc) { return cc ^ 1; }

// Writes the opcode of a jcc whose displacement has 32 bits, or else 8.
inline void writeJcc(uint8_t *loc, uint8_t cc, bool isRel32) {
  if (isRel32) {
    loc[-2] = 0x0f;
    loc[-1] = 0x80 | cc;
  } else {
    loc[-1] = 0x70 | cc;
  }
}
} // namespace lld::x86

#endif
