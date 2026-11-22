//===- WalkerTest.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Walker.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace lld::coff;

namespace {
bool decode(std::initializer_list<uint8_t> code, Insn &insn) {
  static std::unique_ptr<X86Decoder> decoder = X86Decoder::create();
  return decoder->decode(ArrayRef<uint8_t>(code.begin(), code.size()), 0x1000,
                         insn);
}

// Every instruction of LLVM's tables that takes a field is encoded with a
// symbol in each: a displacement from rip, a branch's offset, an immediate, an
// absolute address. Where the decoder admits the encoding, the fields it
// reports are those the encoder left to fixups, at their offsets and widths.
TEST(WalkerTest, FieldsOfEveryOpcode) {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86TargetMC();
  Triple triple("x86_64-pc-windows-msvc");
  std::string error;
  const Target *target = TargetRegistry::lookupTarget(triple, error);
  ASSERT_TRUE(target) << error;
  std::unique_ptr<MCRegisterInfo> mri(target->createMCRegInfo(triple));
  std::unique_ptr<MCAsmInfo> mai(
      target->createMCAsmInfo(*mri, triple, MCTargetOptions()));
  std::unique_ptr<MCSubtargetInfo> sti(
      target->createMCSubtargetInfo(triple, "", ""));
  std::unique_ptr<MCInstrInfo> mii(target->createMCInstrInfo());
  MCContext ctx(triple, *mai, *mri, *sti);
  std::unique_ptr<MCCodeEmitter> emitter(
      target->createMCCodeEmitter(*mii, ctx));
  std::unique_ptr<MCAsmBackend> backend(
      target->createMCAsmBackend(*sti, *mri, MCTargetOptions()));
  std::unique_ptr<X86Decoder> decoder = X86Decoder::create();
  ASSERT_TRUE(decoder);
  const MCExpr *symbol =
      MCSymbolRefExpr::create(ctx.getOrCreateSymbol("symbol"), ctx);

  unsigned relative = 0, branches = 0, immediates = 0, wide = 0, admitted = 0,
           withheld = 0;
  for (unsigned opcode = 1, e = mii->getNumOpcodes(); opcode != e; ++opcode) {
    const MCInstrDesc &desc = mii->get(opcode);
    uint64_t flags = desc.TSFlags;
    unsigned form = flags & X86II::FormMask;
    // A form that always has an index has no address relative to rip.
    if (desc.isPseudo() || form == X86II::Pseudo || form == X86II::PrefixByte ||
        form == X86II::MRMSrcMemFSIB || form == X86II::MRMDestMemFSIB)
      continue;
    int memory = X86II::getMemoryOperandNo(flags);
    if (memory >= 0)
      memory += X86II::getOperandBias(desc);
    else if (!X86II::hasImm(flags))
      continue;
    // The encoder takes an operand that the form leaves over as an
    // immediate, which a compiler's own instruction may not have.
    if (!X86II::hasImm(flags) && desc.getNumOperands() &&
        desc.operands().back().OperandType == MCOI::OPERAND_IMMEDIATE)
      continue;
    // The immediates that the encoder writes as fields end the operands.
    unsigned fields = desc.getNumOperands();
    if (X86II::hasImm(flags) && (flags & X86II::ImmMask) != X86II::Imm8Reg)
      while (fields &&
             desc.operands()[fields - 1].OperandType == MCOI::OPERAND_IMMEDIATE)
        --fields;

    MCInst inst;
    inst.setOpcode(opcode);
    bool known = true;
    for (unsigned i = 0, n = desc.getNumOperands(); i != n; ++i) {
      const MCOperandInfo &info = desc.operands()[i];
      int tied = desc.getOperandConstraint(i, MCOI::TIED_TO);
      if (int(i) == memory) {
        inst.addOperand(MCOperand::createReg(mri->getProgramCounter()));
        inst.addOperand(MCOperand::createImm(1));
        inst.addOperand(MCOperand::createReg(MCRegister()));
        inst.addOperand(MCOperand::createExpr(symbol));
        inst.addOperand(MCOperand::createReg(MCRegister()));
        i += 4;
      } else if (tied >= 0) {
        inst.addOperand(inst.getOperand(tied));
      } else if (info.OperandType == MCOI::OPERAND_PCREL ||
                 (form == X86II::RawFrmMemOffs && i == 0) || i >= fields) {
        inst.addOperand(MCOperand::createExpr(symbol));
      } else if (info.OperandType == MCOI::OPERAND_REGISTER &&
                 info.RegClass >= 0 && !info.isLookupRegClassByHwMode()) {
        inst.addOperand(
            MCOperand::createReg(*mri->getRegClass(info.RegClass).begin()));
      } else if (form == X86II::RawFrmMemOffs) {
        inst.addOperand(MCOperand::createReg(MCRegister()));
      } else if (info.OperandType == MCOI::OPERAND_MEMORY ||
                 info.OperandType == MCOI::OPERAND_REGISTER) {
        known = false;
      } else {
        inst.addOperand(MCOperand::createImm(
            info.OperandType == X86::OPERAND_COND_CODE ? 4 : 1));
      }
    }
    if (!known)
      continue;

    SmallString<16> code;
    SmallVector<MCFixup, 2> fixups;
    emitter->encodeInstruction(inst, code, fixups, *sti);
    // What is not a 64-bit instruction decodes as another, or as none.
    Insn insn;
    if (fixups.empty() ||
        !decoder->decode(arrayRefFromStringRef(code), 0x1000, insn) ||
        insn.size != code.size())
      continue;
    ++admitted;
    // An instruction whose fields the disassembler does not tell apart has
    // none, so a relocation on it is refused.
    if (insn.fields.empty()) {
      ++withheld;
      continue;
    }
    for (const MCFixup &fixup : fixups) {
      // The encoder writes the memory operand before any immediate.
      bool rip = memory >= 0 && &fixup == fixups.begin();
      unsigned size = backend->getFixupKindInfo(fixup.getKind()).TargetSize / 8;
      const InsnField *field = find_if(insn.fields, [&](const InsnField &f) {
        return f.offset == fixup.getOffset();
      });
      if (field == insn.fields.end()) {
        ADD_FAILURE() << mii->getName(opcode) << " has no field at "
                      << fixup.getOffset();
        continue;
      }
      EXPECT_EQ(field->size, size) << mii->getName(opcode);
      EXPECT_EQ(field->pcRelative, rip) << mii->getName(opcode);
      EXPECT_EQ(field->branch, fixup.isPCRel() && !rip) << mii->getName(opcode);
      relative += rip;
      branches += field->branch;
      immediates += !rip && !field->branch;
      wide += size == 8;
    }
  }
  // The tables hold thousands of forms with a memory operand, and tens with
  // a branch or a 64-bit field.
  EXPECT_GT(admitted, 6000u);
  EXPECT_GT(relative, 5000u);
  EXPECT_GT(immediates, 1500u);
  EXPECT_GT(branches, 5u);
  EXPECT_GT(wide, 4u);
  EXPECT_LT(withheld, 4u);
}

TEST(WalkerTest, Refused) {
  Insn insn;
  // An undefined opcode, and an instruction of more than 15 bytes.
  EXPECT_FALSE(decode({0x06}, insn));
  EXPECT_FALSE(decode({0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e,
                       0x2e, 0x2e, 0xe8, 0, 0, 0, 0},
                      insn));
  // A VEX prefix after 66, and one after REX.
  EXPECT_FALSE(decode({0x66, 0xc5, 0xf8, 0x77}, insn));
  EXPECT_FALSE(decode({0x48, 0xc5, 0xf8, 0x77}, insn));
  // An instruction that the encoder writes back as another: lods with an
  // override that names its own segment.
  EXPECT_FALSE(decode({0x3e, 0xad}, insn));
  // lock on an instruction that takes none, and on one whose destination is
  // a register.
  EXPECT_FALSE(decode({0xf0, 0xb8, 0, 0, 0, 0}, insn));
  EXPECT_FALSE(decode({0xf0, 0x01, 0xc0}, insn));
  EXPECT_FALSE(decode({0x66, 0xf0, 0x8b, 0x00}, insn));
  // REX2 before a prefix, which the encoder does not write back, and before
  // opcodes that are reserved with it.
  EXPECT_FALSE(decode({0xd5, 0x00, 0x66, 0x90}, insn));
  EXPECT_FALSE(decode({0xd5, 0x00, 0xe8, 0, 0, 0, 0}, insn));
  EXPECT_FALSE(decode({0xd5, 0x80, 0x84, 0, 0, 0, 0}, insn));
  // A load of cs.
  EXPECT_FALSE(decode({0x8e, 0xc8}, insn));
  // rep before an opcode of the second map that does not take it.
  EXPECT_FALSE(decode({0xf2, 0x0f, 0x14, 0xc1}, insn));
  // XOP after 66.
  EXPECT_FALSE(decode({0x66, 0x8f, 0xe9, 0x78, 0x01, 0xc8}, insn));
  // 66 before a branch: its offset has 32 bits on Intel processors and 16 on
  // AMD's.
  EXPECT_FALSE(decode({0x66, 0xe8, 0, 0, 0, 0}, insn));
  EXPECT_FALSE(decode({0x66, 0xe9, 0, 0, 0, 0}, insn));
  EXPECT_FALSE(decode({0x66, 0x0f, 0x84, 0, 0, 0, 0}, insn));
  EXPECT_FALSE(decode({0x66, 0x74, 0}, insn));
}

TEST(WalkerTest, Decoded) {
  Insn insn;
  // call falls through to the next instruction and names its target.
  ASSERT_TRUE(decode({0xe8, 0x10, 0, 0, 0}, insn));
  EXPECT_EQ(insn.size, 5);
  EXPECT_FALSE(insn.ends);
  EXPECT_EQ(insn.target, 0x1015u);
  ASSERT_EQ(insn.fields.size(), 1u);
  EXPECT_TRUE(insn.fields[0].branch);
  // jcc does too; jmp, ret, an indirect jump and the traps end their paths.
  ASSERT_TRUE(decode({0x74, 0xfe}, insn));
  EXPECT_FALSE(insn.ends);
  EXPECT_EQ(insn.target, 0x1000u);
  ASSERT_TRUE(decode({0xeb, 0x00}, insn));
  EXPECT_TRUE(insn.ends);
  EXPECT_EQ(insn.target, 0x1002u);
  for (std::initializer_list<uint8_t> code :
       {std::initializer_list<uint8_t>{0xc3},
        {0xff, 0xe0},
        {0xff, 0x25, 0, 0, 0, 0},
        {0xcc},
        {0xf4},
        {0x0f, 0x0b},
        {0xcd, 0x29}}) {
    ASSERT_TRUE(decode(code, insn));
    EXPECT_TRUE(insn.ends);
    EXPECT_FALSE(insn.target);
  }
  // lock on an instruction that writes memory, after another prefix too; a
  // REX2 instruction; a load of ds; rep as part of an opcode, and as bnd
  // before jcc; XOP.
  ASSERT_TRUE(decode({0xf0, 0x01, 0x00}, insn));
  ASSERT_TRUE(decode({0x66, 0xf0, 0x0f, 0xc1, 0x00}, insn));
  ASSERT_TRUE(decode({0xd5, 0x18, 0x8b, 0x00}, insn));
  ASSERT_TRUE(decode({0xd5, 0x80, 0x64, 0xc1}, insn));
  ASSERT_TRUE(decode({0x8e, 0xd8}, insn));
  ASSERT_TRUE(decode({0xf3, 0x0f, 0x10, 0xc1}, insn));
  ASSERT_TRUE(decode({0xf2, 0x0f, 0x84, 0, 0, 0, 0}, insn));
  ASSERT_TRUE(decode({0x8f, 0xe9, 0x78, 0x01, 0xc8}, insn));
  // A VEX prefix takes a segment before it, and REX before that.
  ASSERT_TRUE(decode({0x2e, 0xc5, 0xf8, 0x77}, insn));
  ASSERT_TRUE(decode({0x48, 0x2e, 0xc5, 0xf8, 0x77}, insn));
  // xbegin continues or aborts to its target; int with another vector
  // continues.
  ASSERT_TRUE(decode({0xc7, 0xf8, 0x10, 0, 0, 0}, insn));
  EXPECT_FALSE(insn.ends);
  EXPECT_EQ(insn.target, 0x1016u);
  ASSERT_TRUE(decode({0xcd, 0x2c}, insn));
  EXPECT_FALSE(insn.ends);
  // An encoding that the encoder does not choose is the same instruction:
  // a displacement of zero in 8 and in 32 bits.
  ASSERT_TRUE(decode({0x8b, 0x40, 0x00}, insn));
  ASSERT_TRUE(decode({0x8b, 0x80, 0, 0, 0, 0}, insn));
  ASSERT_EQ(insn.fields.size(), 1u);
  EXPECT_EQ(insn.fields[0].offset, 2);
  EXPECT_FALSE(insn.fields[0].pcRelative);
  // 67 makes the displacement relative to eip.
  ASSERT_TRUE(decode({0x67, 0x48, 0x8d, 0x05, 0, 0, 0, 0}, insn));
  EXPECT_TRUE(insn.fields[0].pcRelative);
  // lock, which the disassembler gives alone, is part of its instruction.
  ASSERT_TRUE(decode({0xf0, 0xff, 0x05, 0, 0, 0, 0}, insn));
  EXPECT_EQ(insn.size, 7);
  ASSERT_EQ(insn.fields.size(), 1u);
  EXPECT_EQ(insn.fields[0].offset, 3);
  EXPECT_TRUE(insn.fields[0].pcRelative);
  // The two immediates of enter have one offset in the disassembler, so the
  // instruction has no field that a relocation may fill.
  ASSERT_TRUE(decode({0xc8, 0x10, 0x00, 0x01}, insn));
  EXPECT_TRUE(insn.fields.empty());
}
} // namespace
