//===----- DefaultHostBootstrapValues.cpp - Defaults for host process -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcess/DefaultHostBootstrapValues.h"
#include "llvm/ADT/SmallVectorExtras.h"
#include "llvm/ExecutionEngine/Orc/Shared/Mangler.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/Support/Memory.h"

#ifdef __APPLE__
#include <dlfcn.h>
#endif // __APPLE__

namespace llvm::orc {

#ifdef _WIN32
static shared::CWrapperFunctionBuffer
restrictCallTargetsAllocAction(const char *ArgData, size_t ArgSize) {
  using namespace shared;
  return WrapperFunction<SPSError(SPSSequence<SPSExecutorAddrRange>,
                                  SPSSequence<SPSExecutorAddr>)>::
      handle(ArgData, ArgSize,
             [](std::vector<ExecutorAddrRange> Code,
                std::vector<ExecutorAddr> Entries) -> Error {
               auto Addrs = map_to_vector(Entries, [](ExecutorAddr A) {
                 return uintptr_t(A.getValue());
               });
               for (ExecutorAddrRange &R : Code)
                 if (auto EC = sys::Memory::restrictCallTargets(
                         {R.Start.toPtr<void *>(), size_t(R.size())}, Addrs))
                   return errorCodeToError(EC);
               return Error::success();
             })
          .release();
}
#endif // _WIN32

void addDefaultBootstrapValuesForHostProcess(
    StringMap<std::vector<char>> &BootstrapMap,
    StringMap<ExecutorAddr> &BootstrapSymbols) {
  Mangler Mangle{Triple(sys::getProcessTriple())};

  // FIXME: We probably shouldn't set these on Windows?
  BootstrapSymbols[Mangle.mangledCopy(
      rt::RegisterEHFrameSectionAllocActionName)] =
      ExecutorAddr::fromPtr(&llvm_orc_registerEHFrameSectionAllocAction);
  BootstrapSymbols[Mangle.mangledCopy(
      rt::DeregisterEHFrameSectionAllocActionName)] =
      ExecutorAddr::fromPtr(&llvm_orc_deregisterEHFrameSectionAllocAction);

  BootstrapSymbols[Mangle.mangledCopy(
      rt::RegisterJITLoaderGDBAllocActionName)] =
      ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBAllocAction);

#ifdef _WIN32
  BootstrapSymbols[Mangle.mangledCopy(rt::RestrictCallTargetsAllocActionName)] =
      ExecutorAddr::fromPtr(&restrictCallTargetsAllocAction);
#endif // _WIN32

  // JIT'd code looks up the C runtime's functions in this process.
  registerInlineCRTFunctions();

#ifdef __APPLE__
  if (!dlsym(RTLD_DEFAULT, "__unw_add_find_dynamic_unwind_sections"))
    BootstrapMap["darwin-use-ehframes-only"].push_back(1);
#endif // __APPLE__
}

} // namespace llvm::orc
