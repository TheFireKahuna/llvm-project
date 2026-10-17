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

#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachinePassManager.h"

namespace llvm {

class MachineInstr;
class Register;
class TargetInstrInfo;
class TargetRegisterInfo;

class MachineKCFIPass : public RequiredPassInfoMixin<MachineKCFIPass> {
public:
  LLVM_ABI PreservedAnalyses run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM);
};

/// Returns the load that defines \p Reg where \p Call reads it if the load's
/// memory operand carries \p ProvenFlag, the mark a target gives the load of
/// an indirect call's target that the CFGuard pass proved, and the value
/// stayed in registers from the load to \p Call: only full copies define the
/// register in between, no call other than \p Check, which must preserve it,
/// intervenes, and no register that the load's address uses was reloaded from
/// a stack slot in the block. Otherwise returns null.
LLVM_ABI MachineInstr *findProvenCallTarget(MachineInstr &Call, Register Reg,
                                            MachineMemOperand::Flags ProvenFlag,
                                            const MachineInstr *Check,
                                            const TargetInstrInfo &TII,
                                            const TargetRegisterInfo &TRI);

} // namespace llvm

#endif // LLVM_CODEGEN_KCFI_H
