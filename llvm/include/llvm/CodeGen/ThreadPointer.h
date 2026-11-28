//===- ThreadPointer.h - Pass and derive the thread pointer -*- C++ -*-//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_THREADPOINTER_H
#define LLVM_CODEGEN_THREADPOINTER_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class TargetMachine;

class ThreadPointerPass
    : public RequiredPassInfoMixin<ThreadPointerPass> {
  const TargetMachine *TM;

public:
  explicit ThreadPointerPass(const TargetMachine &TM) : TM(&TM) {}
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif // LLVM_CODEGEN_THREADPOINTER_H
