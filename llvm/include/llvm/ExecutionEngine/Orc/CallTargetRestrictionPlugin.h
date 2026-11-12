//===- CallTargetRestrictionPlugin.h - Restrict call targets ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Restrict the indirect call targets of JIT'd code to its entries.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_CALLTARGETRESTRICTIONPLUGIN_H
#define LLVM_EXECUTIONENGINE_ORC_CALLTARGETRESTRICTIONPLUGIN_H

#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Memory.h"

#include <memory>

namespace llvm {
namespace object {
class COFFObjectFile;
} // namespace object

namespace orc {

/// Adds an AllocationAction that, once the graph's code is executable,
/// leaves only its entries valid targets of indirect calls where the executor
/// checks them, as a Windows process under Control Flow Guard does. The
/// entries are those a static linker puts in an image's guard table: the
/// graph's global symbols, and the functions its .gfids$y section names if its
/// object was built with Control Flow Guard, or else the functions its edges
/// name outside unwind data and debug info.
class LLVM_ABI CallTargetRestrictionPlugin
    : public LinkGraphLinkingLayer::Plugin {
public:
  /// Returns null if the executor does not offer the action.
  static Expected<std::unique_ptr<CallTargetRestrictionPlugin>>
  Create(ExecutionSession &ES);

  CallTargetRestrictionPlugin(ExecutorAddr RestrictCallTargets)
      : RestrictCallTargets(RestrictCallTargets) {}

  void modifyPassConfig(MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &PassConfig) override;
  Error notifyFailed(MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyRemovingResources(JITDylib &JD, ResourceKey K) override {
    return Error::success();
  }
  void notifyTransferringResources(JITDylib &JD, ResourceKey DstKey,
                                   ResourceKey SrcKey) override {}

private:
  ExecutorAddr RestrictCallTargets;
};

/// Computes, by the same rule, the call targets of a COFF object RuntimeDyld
/// loaded in process: each code section with the stubs RuntimeDyld places
/// after it in \p Code, and its sorted entries in \p Entries.
LLVM_ABI Error getCallTargets(const object::COFFObjectFile &Obj,
                              const RuntimeDyld::LoadedObjectInfo &L,
                              SmallVectorImpl<sys::MemoryBlock> &Code,
                              std::vector<uintptr_t> &Entries);

} // namespace orc
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_CALLTARGETRESTRICTIONPLUGIN_H
