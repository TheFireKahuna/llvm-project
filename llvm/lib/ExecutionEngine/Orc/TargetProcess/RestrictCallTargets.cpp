//===- RestrictCallTargets.cpp - Restrict JIT'd code's call targets -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcess/RestrictCallTargets.h"
#include "llvm/ExecutionEngine/Orc/Shared/SimplePackedSerialization.h"
#include "llvm/Support/Memory.h"

using namespace llvm;
using namespace llvm::orc;
using namespace llvm::orc::shared;

static Error restrictCallTargetsWrapper(ExecutorAddrRange Code,
                                        std::vector<ExecutorAddr> Entries) {
  std::vector<uintptr_t> Addrs;
  Addrs.reserve(Entries.size());
  for (ExecutorAddr E : Entries)
    Addrs.push_back(E.getValue());
  if (std::error_code EC = sys::Memory::restrictCallTargets(
          sys::MemoryBlock(Code.Start.toPtr<void *>(), Code.size()), Addrs))
    return errorCodeToError(EC);
  return Error::success();
}

extern "C" CWrapperFunctionBuffer
llvm_orc_restrictCallTargetsAllocAction(const char *ArgData, size_t ArgSize) {
  return WrapperFunction<SPSError(SPSExecutorAddrRange,
                                  SPSSequence<SPSExecutorAddr>)>::
      handle(ArgData, ArgSize, restrictCallTargetsWrapper)
          .release();
}
