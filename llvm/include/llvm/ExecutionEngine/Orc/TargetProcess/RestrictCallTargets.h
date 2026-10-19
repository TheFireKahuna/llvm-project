//===- RestrictCallTargets.h - Restrict JIT'd code's call targets -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An allocation action that restricts the targets of indirect calls in JIT'd
// code to its functions, where the process checks such calls.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_RESTRICTCALLTARGETS_H
#define LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_RESTRICTCALLTARGETS_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/Support/Compiler.h"

/// Takes an ExecutorAddrRange of code and the ExecutorAddrs of its entries, in
/// ascending order.
extern "C" LLVM_ABI llvm::orc::shared::CWrapperFunctionBuffer
llvm_orc_restrictCallTargetsAllocAction(const char *ArgData, size_t ArgSize);

#endif // LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_RESTRICTCALLTARGETS_H
