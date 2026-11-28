//===- RegAllocCommon.h - Utilities shared between allocators ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_REGALLOCCOMMON_H
#define LLVM_CODEGEN_REGALLOCCOMMON_H

#include "llvm/CodeGen/Register.h"
#include "llvm/Support/Compiler.h"
#include <functional>

namespace llvm {

class MCRegisterClass;
using TargetRegisterClass = MCRegisterClass;
class TargetRegisterInfo;
class MachineFunction;
class MachineRegisterInfo;
class LiveIntervals;

/// Filter function for register classes during regalloc. Default register class
/// filter is nullptr, where all registers should be allocated.
typedef std::function<bool(const TargetRegisterInfo &TRI,
                           const MachineRegisterInfo &MRI, const Register Reg)>
    RegAllocFilterFunc;

/// After allocation, reads each LOAD_THREAD_POINTER of \p MF from the thread
/// pointer's register where every path from the entry leaves it unmodified,
/// and leaves each other for frame lowering to reload. Keeps \p LIS, if any.
LLVM_ABI void resolveThreadPointers(MachineFunction &MF, LiveIntervals *LIS);
}

#endif // LLVM_CODEGEN_REGALLOCCOMMON_H
