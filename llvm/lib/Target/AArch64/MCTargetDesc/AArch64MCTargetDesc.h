//===-- AArch64MCTargetDesc.h - AArch64 Target Descriptions -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides AArch64 specific target descriptions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_MCTARGETDESC_AARCH64MCTARGETDESC_H
#define LLVM_LIB_TARGET_AARCH64_MCTARGETDESC_AARCH64MCTARGETDESC_H

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/Support/DataTypes.h"

#include <memory>

namespace llvm {
class formatted_raw_ostream;
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInst;
class MCInstrInfo;
class MCInstPrinter;
class MCRegisterInfo;
class MCObjectTargetWriter;
class MCStreamer;
class MCSubtargetInfo;
class MCTargetOptions;
class MCTargetStreamer;
class Target;
class Triple;

MCCodeEmitter *createAArch64MCCodeEmitter(const MCInstrInfo &MCII,
                                          MCContext &Ctx);
MCAsmBackend *createAArch64leAsmBackend(const Target &T,
                                        const MCSubtargetInfo &STI,
                                        const MCRegisterInfo &MRI,
                                        const MCTargetOptions &Options);
MCAsmBackend *createAArch64beAsmBackend(const Target &T,
                                        const MCSubtargetInfo &STI,
                                        const MCRegisterInfo &MRI,
                                        const MCTargetOptions &Options);

std::unique_ptr<MCObjectTargetWriter>
createAArch64ELFObjectWriter(uint8_t OSABI, bool IsILP32);

std::unique_ptr<MCObjectTargetWriter>
createAArch64MachObjectWriter(uint32_t CPUType, uint32_t CPUSubtype,
                              bool IsILP32);

std::unique_ptr<MCObjectTargetWriter>
createAArch64WinCOFFObjectWriter(const Triple &TheTriple);

MCTargetStreamer *createAArch64AsmTargetStreamer(MCStreamer &S,
                                                 formatted_raw_ostream &OS,
                                                 MCInstPrinter *InstPrint);

namespace AArch64_MC {
void initLLVMToCVRegMapping(MCRegisterInfo *MRI);
bool isHForm(const MCInst &MI, const MCInstrInfo *MCII);
bool isQForm(const MCInst &MI, const MCInstrInfo *MCII);
bool isFpOrNEON(const MCInst &MI, const MCInstrInfo *MCII);
} // namespace AArch64_MC

namespace AArch64 {
enum OperandType {
  OPERAND_IMPLICIT_IMM_0 = MCOI::OPERAND_FIRST_TARGET,
  OPERAND_SHIFT_MSL,
  OPERAND_SHIFTED_REGISTER,
  OPERAND_SHIFTED_IMMEDIATE,
  OPERAND_IMM_UINT1,
  OPERAND_IMM_UINT4plus1,
  OPERAND_IMM_UINT5,
  OPERAND_IMM_UINT8,
};

// MCInst flag: a b that no Windows unwind frame covers.
enum : unsigned { TailBranchFlag = 1 };
} // namespace AArch64

} // namespace llvm

// Defines symbolic names for AArch64 registers.  This defines a mapping from
// register name to register number.
//
#define GET_REGINFO_ENUM
#include "AArch64GenRegisterInfo.inc"

// Defines symbolic names for the AArch64 instructions.
//
#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "AArch64GenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "AArch64GenSubtargetInfo.inc"

namespace llvm {
namespace AArch64_MC {
/// How \p I, an MCInst or a MachineInstr, reads a register that holds a
/// pointer, in a form that an object for Windows Itanium or NT-POSIX states as
/// a fused site, with the operand that reads it in \p RegOp. That no other
/// operand of \p I reads the register is the caller's to check.
template <class InstT>
COFF::FusedUse getFusedUse(const InstT &I, unsigned &RegOp) {
  RegOp = 0;
  switch (I.getOpcode()) {
  case AArch64::ORRXrs:
    RegOp = 2;
    return I.getOperand(1).getReg() == AArch64::XZR && !I.getOperand(3).getImm()
               ? COFF::FusedUse::Copy
               : COFF::FusedUse::None;
  // A load or a store at the register plus an offset that is a number.
  case AArch64::LDRBBui:
  case AArch64::LDRHHui:
  case AArch64::LDRWui:
  case AArch64::LDRXui:
  case AArch64::LDRSBWui:
  case AArch64::LDRSBXui:
  case AArch64::LDRSHWui:
  case AArch64::LDRSHXui:
  case AArch64::LDRSWui:
  case AArch64::LDRBui:
  case AArch64::LDRHui:
  case AArch64::LDRSui:
  case AArch64::LDRDui:
  case AArch64::LDRQui:
  case AArch64::STRBBui:
  case AArch64::STRHHui:
  case AArch64::STRWui:
  case AArch64::STRXui:
  case AArch64::STRBui:
  case AArch64::STRHui:
  case AArch64::STRSui:
  case AArch64::STRDui:
  case AArch64::STRQui:
    RegOp = 1;
    return I.getOperand(2).isImm() ? COFF::FusedUse::Access
                                   : COFF::FusedUse::None;
  case AArch64::LDPWi:
  case AArch64::LDPXi:
  case AArch64::LDPSWi:
  case AArch64::LDPSi:
  case AArch64::LDPDi:
  case AArch64::LDPQi:
  case AArch64::STPWi:
  case AArch64::STPXi:
  case AArch64::STPSi:
  case AArch64::STPDi:
  case AArch64::STPQi:
    RegOp = 2;
    return I.getOperand(3).isImm() ? COFF::FusedUse::Access
                                   : COFF::FusedUse::None;
  case AArch64::ADDXri:
    RegOp = 1;
    return I.getOperand(2).isImm() && !I.getOperand(3).getImm()
               ? COFF::FusedUse::Add
               : COFF::FusedUse::None;
  case AArch64::CBZX:
  case AArch64::CBNZX:
    return COFF::FusedUse::Test;
  case AArch64::BLR:
    return COFF::FusedUse::Call;
  case AArch64::BR:
  case AArch64::TCRETURNri:
  case AArch64::TCRETURNrix16x17:
  case AArch64::TCRETURNrix17:
  case AArch64::TCRETURNrinotx16:
    return COFF::FusedUse::Jump;
  default:
    return COFF::FusedUse::None;
  }
}
} // namespace AArch64_MC
} // namespace llvm

#endif
