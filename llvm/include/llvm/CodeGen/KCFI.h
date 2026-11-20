//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declaration of the MachineKCFI class, which is a
/// Machine Pass that implements kernel control flow integrity.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_KCFI_H
#define LLVM_CODEGEN_KCFI_H

#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/Register.h"

namespace llvm {

class MachineInstr;

class MachineKCFIPass : public RequiredPassInfoMixin<MachineKCFIPass> {
public:
  LLVM_ABI PreservedAnalyses run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM);
};

/// Before register allocation, computes again in front of its use each
/// constant that the address of a proven call target's load is computed from,
/// such as a table's address that a loop keeps in one register across its
/// calls, so that findProvenCallTarget finds it computed since the last call.
class CFGuardProvenAddressPass
    : public OptionalPassInfoMixin<CFGuardProvenAddressPass> {
public:
  LLVM_ABI PreservedAnalyses run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM);
};

/// Returns the load that defines \p Reg, the target of the indirect call
/// \p Call, if the CFGuard pass proved the value loaded (the load's memory
/// operand is MOCFGuardProven) and the proof still holds where \p Call reads
/// it. The load is in the call's block and only full copies define the
/// register in between, so the target stayed in registers. The one call that
/// may lie between them is a Control Flow Guard check, which preserves the
/// register and is returned in \p Check; any other callee could have spilled
/// the target. No value the load's address is computed from comes from a
/// stack slot, was live across a call or entered a block with several
/// predecessors. Otherwise returns null.
LLVM_ABI MachineInstr *findProvenCallTarget(MachineInstr &Call, Register Reg,
                                            MachineInstr *&Check);

/// If findProvenCallTarget proves \p Reg, the target of the indirect call
/// \p Call, and the Control Flow Guard check before the call tested the same
/// load in \p CheckReg, erases the check with the instructions that only fed
/// it, clears the call's KCFI type and returns the load. Otherwise returns
/// null.
LLVM_ABI MachineInstr *dropProvenCallCheck(MachineInstr &Call, Register Reg,
                                           Register CheckReg);

} // namespace llvm

#endif // LLVM_CODEGEN_KCFI_H
