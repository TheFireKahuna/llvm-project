//===- COFFABIRequirements.cpp - COFF ABI requirements
//----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/COFFABIRequirements.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

static bool retainRequirements(Module &M, ArrayRef<GlobalValue *> Targets) {
  // A separate root distinguishes our temporary retention from llvm.used or
  // llvm.compiler.used entries owned by the frontend, inline assembly, or
  // other passes. Directly retaining each target loses that distinction when
  // independently compiled modules are merged during LTO.
  SmallSetVector<Constant *, 16> Values;
  GlobalVariable *Old = M.getNamedGlobal("llvm.coff.abi.keep");
  if (Old) {
    if (!Old->hasPrivateLinkage() || !Old->getMetadata("coff.abi.keep") ||
        !Old->hasInitializer() || !isa<ConstantArray>(Old->getInitializer()))
      reportFatalUsageError("invalid COFF ABI retention object");
    for (Value *V : Old->getInitializer()->operand_values())
      Values.insert(cast<Constant>(V));
  }
  Type *Pointer = PointerType::getUnqual(M.getContext());
  for (GlobalValue *Target : Targets)
    Values.insert(
        ConstantExpr::getPointerBitCastOrAddrSpaceCast(Target, Pointer));
  if (Old && Old->getInitializer()->getNumOperands() == Values.size())
    return false;
  auto *ArrayTy = ArrayType::get(Pointer, Values.size());
  auto *Keep = new GlobalVariable(
      M, ArrayTy, true, GlobalValue::PrivateLinkage,
      ConstantArray::get(ArrayTy, Values.getArrayRef()), "llvm.coff.abi.keep");
  Keep->setMetadata("coff.abi.keep", MDNode::get(M.getContext(), {}));
  Keep->setSection("llvm.metadata");
  if (Old) {
    Keep->takeName(Old);
    Old->replaceAllUsesWith(Keep);
    Old->eraseFromParent();
  } else {
    appendToCompilerUsed(M, {Keep});
  }
  return true;
}

static bool mergeRequirements(GlobalObject &To, ArrayRef<Metadata *> Added,
                              SmallVectorImpl<Metadata *> *New = nullptr) {
  SetVector<Metadata *> Uses;
  if (MDNode *Existing = To.getMetadata("coff.abi.uses"))
    for (const MDOperand &Use : Existing->operands())
      Uses.insert(Use.get());
  size_t OldSize = Uses.size();
  for (Metadata *Use : Added)
    if (Uses.insert(Use) && New)
      New->push_back(Use);
  if (Uses.size() == OldSize)
    return false;
  To.setMetadata("coff.abi.uses",
                 MDNode::get(To.getContext(), Uses.getArrayRef()));
  return true;
}

void llvm::mergeCOFFABIRequirements(GlobalObject &To,
                                    const GlobalObject &From) {
  if (!From.hasMetadata() ||
      !From.getParent()->getTargetTriple().isOSBinFormatCOFF())
    return;
  if (MDNode *Uses = From.getMetadata("coff.abi.uses")) {
    SmallVector<Metadata *, 4> Added;
    for (const MDOperand &Use : Uses->operands())
      Added.push_back(Use.get());
    mergeRequirements(To, Added);
  }
}

PreservedAnalyses COFFABIRequirementsPass::run(Module &M,
                                               ModuleAnalysisManager &) {
  if (!M.getTargetTriple().isOSBinFormatCOFF())
    return PreservedAnalyses::all();
  if (Prune)
    return finalizeCOFFABIRequirements(M, /*ForEmission=*/false)
               ? PreservedAnalyses::none()
               : PreservedAnalyses::all();

  return propagateCOFFABIRequirements(M) ? PreservedAnalyses::none()
                                         : PreservedAnalyses::all();
}

bool llvm::propagateCOFFABIRequirements(
    Module &M, ArrayRef<COFFABIRequirementEdge> Inferred) {
  if (!M.getTargetTriple().isOSBinFormatCOFF())
    return false;

  SmallVector<GlobalObject *, 16> Pending;
  SmallSetVector<GlobalValue *, 16> Retained;
  DenseMap<GlobalObject *, SmallVector<Metadata *, 4>> Requirements;
  DenseMap<GlobalObject *, SmallVector<GlobalObject *, 2>> Consumers;
  for (const COFFABIRequirementEdge &Edge : Inferred) {
    assert(Edge.Source->getParent() == &M && Edge.Consumer->getParent() == &M &&
           "cross-module ABI dependency");
    Consumers[Edge.Source].push_back(Edge.Consumer);
  }
  for (GlobalObject &GO : M.global_objects()) {
    if (GO.getName().starts_with("llvm."))
      continue;
    if (!GO.getMetadata("coff.abi") && !GO.getMetadata("coff.abi.uses"))
      continue;
    auto &Uses = Requirements[&GO];
    if (MDNode *ABI = GO.getMetadata("coff.abi")) {
      if (ABI->getNumOperands() != 1 ||
          !isa_and_nonnull<MDString>(ABI->getOperand(0)))
        reportFatalUsageError("invalid COFF ABI metadata for " + GO.getName());
      Uses.push_back(MDNode::get(
          M.getContext(), {ValueAsMetadata::get(&GO), ABI->getOperand(0)}));
    }
    if (MDNode *Existing = GO.getMetadata("coff.abi.uses"))
      for (const MDOperand &Use : Existing->operands()) {
        auto *Entry = dyn_cast_or_null<MDNode>(Use);
        auto *Target =
            Entry && Entry->getNumOperands() == 2
                ? dyn_cast_or_null<ValueAsMetadata>(Entry->getOperand(0))
                : nullptr;
        auto *GV = Target ? dyn_cast<GlobalValue>(Target->getValue()) : nullptr;
        if (!GV || GV->getParent() != &M ||
            !isa_and_nonnull<MDString>(Entry->getOperand(1)))
          reportFatalUsageError("invalid COFF ABI use metadata for " +
                                GO.getName());
        Retained.insert(GV);
        Uses.push_back(Entry);
      }
    if (!Uses.empty())
      Pending.push_back(&GO);
  }

  bool Changed = false;
  while (!Pending.empty()) {
    GlobalObject *Source = Pending.pop_back_val();
    // Consume the pending delta before visiting users. Contributions reached
    // through several paths are queued once, and cycles propagate only newly
    // discovered requirements, not their entire accumulated contract set.
    SmallVector<Metadata *, 4> Added = std::move(Requirements[Source]);
    Requirements.erase(Source);
    SmallVector<User *, 16> Users(Source->user_begin(), Source->user_end());
    if (auto It = Consumers.find(Source); It != Consumers.end())
      llvm::append_range(Users, It->second);
    SmallPtrSet<User *, 32> Seen;
    while (!Users.empty()) {
      User *U = Users.pop_back_val();
      if (!Seen.insert(U).second)
        continue;
      GlobalObject *Consumer = dyn_cast<GlobalObject>(U);
      if (auto *I = dyn_cast<Instruction>(U))
        Consumer = I->getFunction();
      if (!Consumer) {
        if (isa<Constant>(U))
          llvm::append_range(Users, U->users());
        continue;
      }
      // Bound RTTI objects retain their initializer dependencies through the
      // ordinary native graph. Their consumers need the object's contract,
      // not another direct import for every name/base field it already binds.
      if (Consumer == Source || Consumer->getName().starts_with("llvm.") ||
          Consumer->getMetadata("coff.binding"))
        continue;
      SmallVector<Metadata *, 4> New;
      if (!mergeRequirements(*Consumer, Added, &New))
        continue;
      Changed = true;
      auto &Uses = Requirements[Consumer];
      bool WasEmpty = Uses.empty();
      for (Metadata *Use : New)
        if (!llvm::is_contained(Uses, Use))
          Uses.push_back(Use);
      if (WasEmpty)
        Pending.push_back(Consumer);
      for (Metadata *Use : New)
        Retained.insert(cast<GlobalValue>(
            cast<ValueAsMetadata>(cast<MDNode>(Use)->getOperand(0))
                ->getValue()));
    }
  }
  // Retain symbol emission, not final-image storage. The native use record is
  // conditional on its consumer's liveness; .llvm.abi itself is not a GC root.
  if (!Retained.empty())
    Changed |= retainRequirements(M, Retained.getArrayRef());
  return Changed;
}

namespace {
struct IsRetentionObject {
  const SmallPtrSetImpl<Constant *> &Objects;
  bool operator()(Constant *C) const { return Objects.contains(C); }
};
} // namespace

bool llvm::finalizeCOFFABIRequirements(Module &M, bool ForEmission) {
  SmallPtrSet<Constant *, 4> Retention;
  SmallPtrSet<GlobalValue *, 32> Candidates;
  SmallVector<GlobalValue *, 16> Needed;
  for (GlobalObject &GO : M.global_objects()) {
    if (GO.getMetadata("coff.abi.keep")) {
      auto *GV = dyn_cast<GlobalVariable>(&GO);
      if (!GV || !GV->hasPrivateLinkage() || !GV->hasInitializer())
        reportFatalUsageError("invalid COFF ABI retention object");
      Retention.insert(GV);
      for (Value *V : GV->getInitializer()->operand_values())
        if (auto *Target = dyn_cast<GlobalValue>(V->stripPointerCasts()))
          Candidates.insert(Target);
      continue;
    }
    if (GO.isDeclarationForLinker())
      continue;
    if (MDNode *Uses = GO.getMetadata("coff.abi.uses"))
      for (const MDOperand &Operand : Uses->operands()) {
        auto *Use = dyn_cast_or_null<MDNode>(Operand);
        auto *Value =
            Use && Use->getNumOperands() == 2
                ? dyn_cast_or_null<ValueAsMetadata>(Use->getOperand(0))
                : nullptr;
        auto *Target =
            Value ? dyn_cast<GlobalValue>(Value->getValue()) : nullptr;
        if (!Target || !isa_and_nonnull<MDString>(Use->getOperand(1)))
          reportFatalUsageError("invalid COFF ABI use metadata for " +
                                GO.getName());
        Needed.push_back(Target);
      }
  }
  if (Retention.empty() && Needed.empty())
    return false;
  // These are the final emitted contributions. Keep their targets for symbol
  // emission; their native use records still impose conditional linker GC.
  if (ForEmission && !Needed.empty())
    appendToCompilerUsed(M, Needed);
  IsRetentionObject Remove{Retention};
  removeFromUsedLists(M, Remove);
  for (Constant *C : Retention) {
    C->removeDeadConstantUsers();
    if (!C->use_empty())
      reportFatalUsageError("COFF ABI retention object has a program use");
    cast<GlobalVariable>(C)->eraseFromParent();
  }
  // Pre-link bitcode still has another optimizer ahead of it. Keep temporary
  // retention separate there, but omit dead targets before archive discovery.
  if (!ForEmission && !Needed.empty())
    retainRequirements(M, Needed);

  // Removing a descriptor can make its name/base descriptors dead as well.
  // Limit cleanup to objects retained by this mechanism; independent roots,
  // exports and ordinary program references retain their original semantics.
  SmallVector<GlobalValue *, 16> Pending(Candidates.begin(), Candidates.end());
  while (!Pending.empty()) {
    GlobalValue *V = Pending.pop_back_val();
    if (!Candidates.contains(V))
      continue;
    auto *GV = dyn_cast<GlobalVariable>(V);
    if (!GV || (!GV->isDeclaration() && !GV->isDiscardableIfUnused()) ||
        GV->hasDLLExportStorageClass())
      continue;
    GV->removeDeadConstantUsers();
    if (!GV->use_empty())
      continue;
    SmallVector<Constant *, 8> Constants;
    SmallPtrSet<Constant *, 16> Seen;
    if (GV->hasInitializer())
      Constants.push_back(GV->getInitializer());
    while (!Constants.empty()) {
      Constant *C = Constants.pop_back_val();
      if (!Seen.insert(C).second)
        continue;
      if (auto *Dependency = dyn_cast<GlobalValue>(C)) {
        // Name/base objects may have been kept only by this initializer,
        // without needing a separate temporary witness root of their own.
        if (auto *Object = dyn_cast<GlobalVariable>(Dependency))
          if (Object->isConstant())
            Candidates.insert(Object);
        if (Candidates.contains(Dependency))
          Pending.push_back(Dependency);
      } else {
        for (Value *Operand : C->operand_values())
          Constants.push_back(cast<Constant>(Operand));
      }
    }
    Candidates.erase(GV);
    GV->eraseFromParent();
  }
  return true;
}
