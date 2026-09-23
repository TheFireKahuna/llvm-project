//===- COFFABIRequirements.h - COFF ABI witnesses ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_COFFABIREQUIREMENTS_H
#define LLVM_TRANSFORMS_UTILS_COFFABIREQUIREMENTS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class GlobalObject;

// A dependency exposed by analysis before its replacement appears in IR.
// The source's original contracts constrain the surviving consumer.
struct COFFABIRequirementEdge {
  GlobalObject *Source;
  GlobalObject *Consumer;
};

LLVM_ABI bool
propagateCOFFABIRequirements(Module &M,
                             ArrayRef<COFFABIRequirementEdge> Inferred = {});

// Requirements describe the assumptions under which a contribution was built,
// not a particular load. Propagate them before folding can remove that load or
// a call to a function whose result depends on it. Metadata creates no runtime
// operations and does not root the consumer in linker GC.
class COFFABIRequirementsPass : public PassInfoMixin<COFFABIRequirementsPass> {
  bool Prune;

public:
  explicit COFFABIRequirementsPass(bool Prune = false) : Prune(Prune) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &);
  static bool isRequired() { return true; }
};

// Also used by transformations that move code between contributions outside
// the standard module pipelines, such as explicit inlining and outlining.
LLVM_ABI void mergeCOFFABIRequirements(GlobalObject &To,
                                       const GlobalObject &From);

// Release temporary optimizer retention and keep only the requirements of
// surviving contributions before native emission. Independent used-list
// entries are never removed.
LLVM_ABI bool finalizeCOFFABIRequirements(Module &M, bool ForEmission = true);
} // namespace llvm

#endif
