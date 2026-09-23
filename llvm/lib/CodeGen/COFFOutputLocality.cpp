//===- COFFOutputLocality.cpp - Lower resolved PE boundaries
//---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A final link can contain several independently relocated PEs. The linker
// records the chosen owner in GlobalValue::Partition before LTO. Inlining can
// move an access across that boundary, so lower surviving instruction uses
// after interprocedural optimization, but before instruction selection and
// register allocation. Initializer fields retain native COFF relocations;
// the linker places their loader-written slots without changing the object.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/COFFOutputLocality.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/COFFABIRequirements.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

namespace {
bool isAddressAcquisition(const Instruction &I) {
  if (I.isEHPad())
    return false;
  if (auto *Intrinsic = dyn_cast<IntrinsicInst>(&I))
    switch (Intrinsic->getIntrinsicID()) {
    case Intrinsic::eh_typeid_for:
    case Intrinsic::localrecover:
    case Intrinsic::eh_recoverfp:
      return false;
    default:
      break;
    }
  return true;
}

std::optional<StringRef> getOwner(const GlobalValue &GV) {
  if (GV.getName().starts_with("llvm."))
    return std::nullopt;
  if (const auto *Alias = dyn_cast<GlobalAlias>(&GV))
    if (const GlobalObject *Object = Alias->getAliaseeObject())
      return getOwner(*Object);
  StringRef Owner = GV.getPartition();
  if (Owner.empty())
    return std::nullopt;
  if (Owner.contains('\0') || (!Owner.starts_with("pe:") &&
                               !Owner.starts_with("dll:") && Owner != "image:"))
    reportFatalUsageError("invalid COFF output owner");
  return Owner;
}

GlobalVariable *getImportCell(GlobalValue &GV) {
  Module &M = *GV.getParent();
  SmallString<128> Symbol;
  Mangler Mang;
  Mang.getNameWithPrefix(Symbol, &GV, false);
  std::string Name = (Twine("\01__imp_") + Symbol).str();
  if (GlobalValue *Existing = M.getNamedValue(Name)) {
    auto *Cell = dyn_cast<GlobalVariable>(Existing);
    if (!Cell || Cell->getValueType() != GV.getType() ||
        Cell->isThreadLocal() || !Cell->isDeclaration())
      reportFatalUsageError("conflicting native import cell in final PE link");
    return Cell;
  }
  // __imp_ is an ordinary COFF symbol. The shared linker graph projects it
  // to this output's IAT, or a direct local binding when the owner is here.
  auto *Cell = new GlobalVariable(M, GV.getType(), /*isConstant=*/true,
                                  GlobalValue::ExternalLinkage, nullptr, Name);
  Cell->setDSOLocal(true);
  Cell->setAlignment(M.getDataLayout().getPointerABIAlignment(0));
  return Cell;
}

using ImportLoads =
    DenseMap<std::pair<GlobalVariable *, BasicBlock *>, LoadInst *>;

LoadInst *loadImportAddress(Use &U, GlobalVariable &Cell, DominatorTree &DT,
                           ImportLoads &Loads) {
  auto *I = cast<Instruction>(U.getUser());
  Instruction *Before = I;
  if (auto *Phi = dyn_cast<PHINode>(I)) {
    Before = Phi->getIncomingBlock(U)->getTerminator();
    // A catchswitch has no legal insertion point. Eager import contents can
    // instead be read at entry, before any use in this function.
    if (isa<CatchSwitchInst>(Before))
      Before = &*I->getFunction()->getEntryBlock().getFirstInsertionPt();
  }
  LoadInst *&Load = Loads[{&Cell, Before->getParent()}];
  if (!Load)
    if (DomTreeNode *Current = DT.getNode(Before->getParent()))
      for (DomTreeNode *Block = Current->getIDom(); Block;
           Block = Block->getIDom())
        if (LoadInst *Dominating = Loads.lookup({&Cell, Block->getBlock()})) {
          Load = Dominating;
          break;
        }
  if (!Load) {
    IRBuilder<> Builder(Before);
    Load = Builder.CreateLoad(Cell.getValueType(), &Cell, "import.addr");
    Load->setMetadata(LLVMContext::MD_invariant_load,
                      MDNode::get(Cell.getContext(), {}));
  } else if (Before->getParent() == Load->getParent() && Before != Load &&
             Before->comesBefore(Load)) {
    Load->moveBefore(Before->getIterator());
  }
  return Load;
}

// Normalize only bounded data addresses. Integer round trips, TLS, code and
// one-past addresses do not establish an exact native data binding.
Constant *getExactDataAddress(Value *V, const DataLayout &DL) {
  if (!V->getType()->isPointerTy() || V->getType()->getPointerAddressSpace())
    return nullptr;
  APInt Offset(DL.getIndexTypeSizeInBits(V->getType()), 0);
  auto *Base = dyn_cast<GlobalVariable>(V->stripAndAccumulateConstantOffsets(
      DL, Offset, /*AllowNonInbounds=*/true));
  if (!Base || Base->isThreadLocal() || Base->getAddressSpace() ||
      !Base->getValueType()->isSized() || Offset.isNonPositive() ||
      Offset.getActiveBits() > 64)
    return nullptr;
  TypeSize Size = DL.getTypeAllocSize(Base->getValueType());
  if (Size.isScalable() || Offset.getZExtValue() >= Size.getFixedValue())
    return nullptr;
  auto Owner = getOwner(*Base);
  if (!Owner || !Owner->starts_with("pe:"))
    return nullptr;
  return ConstantExpr::getGetElementPtr(
      Type::getInt8Ty(V->getContext()), Base,
      ConstantInt::get(V->getContext(), Offset));
}

// Borrow an already retained immutable address field in the consumer's image.
// The native graph will resolve its initializer to the exact provider offset.
// No new field, export demand, object record or runtime initialization is needed.
bool reuseExactAddressSlots(Module &M) {
  if (!M.getModuleFlag("coff.import-slots"))
    return false;
  const DataLayout &DL = M.getDataLayout();
  DenseMap<std::pair<Constant *, StringRef>, GlobalVariable *> Slots;
  for (GlobalVariable &GV : M.globals()) {
    // An export survives native GC independently of the added code reference.
    // A weak/interposable or externally initialized field cannot prove its
    // contents. Do not turn an otherwise dead field into permanent storage.
    if (!GV.hasDLLExportStorageClass() || !GV.isConstant() ||
        !GV.hasDefinitiveInitializer() || GV.isThreadLocal() ||
        GV.getAddressSpace() || !GV.getValueType()->isPointerTy() ||
        DL.getValueOrABITypeAlignment(GV.getAlign(), GV.getValueType()) <
            DL.getPointerABIAlignment(0))
      continue;
    auto Owner = getOwner(GV);
    if (!Owner || !Owner->starts_with("pe:"))
      continue;
    if (Constant *Address = getExactDataAddress(GV.getInitializer(), DL)) {
      auto *Base = cast<GlobalValue>(Address->getOperand(0));
      if (getOwner(*Base) != Owner)
        Slots.try_emplace({Address, *Owner}, &GV);
    }
  }
  if (Slots.empty())
    return false;

  bool Changed = false;
  SmallVector<WeakTrackingVH, 16> Dead;
  for (Function &F : M) {
    auto Owner = getOwner(F);
    if (F.isDeclaration() || !Owner || !Owner->starts_with("pe:"))
      continue;
    SmallVector<std::pair<Use *, GlobalVariable *>, 16> Uses;
    for (Instruction &I : instructions(F)) {
      if (!isAddressAcquisition(I))
        continue;
      for (Use &U : I.operands())
        if (Constant *Address = getExactDataAddress(U.get(), DL))
          if (GlobalVariable *Slot = Slots.lookup({Address, *Owner}))
            Uses.emplace_back(&U, Slot);
    }
    if (Uses.empty())
      continue;
    // Capture originating requirements before replacing their last IR address
    // use. The chosen field provides storage, never replacement ABI evidence.
    if (!Changed)
      propagateCOFFABIRequirements(M);
    Changed = true;
    DominatorTree DT(F);
    ImportLoads Loads;
    for (auto [U, Slot] : Uses) {
      // The PE loader supplies this field. Stop later constant folding from
      // reconstructing the base-plus-offset expression we just eliminated.
      Slot->setExternallyInitialized(true);
      LoadInst *Load = loadImportAddress(*U, *Slot, DT, Loads);
      if (auto *Old = dyn_cast<Instruction>(U->get()))
        Dead.push_back(Old);
      U->set(Load);
    }
  }
  for (WeakTrackingVH &V : Dead)
    if (V)
      RecursivelyDeleteTriviallyDeadInstructions(V);
  return Changed;
}

bool lowerCOFFOutputLocality(Module &M) {
  if (!M.getTargetTriple().isOSBinFormatCOFF())
    return false;
  bool Changed = false;
  if (M.getModuleFlag("coff.output-set"))
    Changed = reuseExactAddressSlots(M);
  Changed |= finalizeCOFFABIRequirements(M);
  if (!M.getModuleFlag("coff.output-set"))
    return Changed;
  SmallVector<GlobalValue *, 32> Targets;
  for (GlobalValue &GV : M.global_values())
    if (getOwner(GV))
      Targets.push_back(&GV);
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    std::optional<StringRef> Source = getOwner(F);
    SmallVector<Constant *, 16> Foreign;
    SmallPtrSet<Constant *, 32> Seen;
    SmallVector<Constant *, 16> Pending;
    for (Instruction &I : instructions(F))
      for (Value *V : I.operand_values())
        if (auto *C = dyn_cast<Constant>(V))
          Pending.push_back(C);
    while (!Pending.empty()) {
      Constant *C = Pending.pop_back_val();
      if (!Seen.insert(C).second)
        continue;
      if (auto *GV = dyn_cast<GlobalValue>(C)) {
        std::optional<StringRef> Destination = getOwner(*GV);
        if (GV != &F && Destination && *Destination != "image:" &&
            Destination != Source)
          Foreign.push_back(GV);
      } else if (!isa<BlockAddress>(C)) {
        for (Value *V : C->operand_values())
          if (auto *Operand = dyn_cast<Constant>(V))
            Pending.push_back(Operand);
      }
    }
    if (Foreign.empty())
      continue;
    Changed |= convertUsersOfConstantsToInstructions(Foreign, &F, true, false,
                                                     isAddressAcquisition);
    DenseMap<GlobalValue *, GlobalVariable *> Cells;
    for (Constant *C : Foreign)
      Cells.try_emplace(cast<GlobalValue>(C), nullptr);
    SmallVector<Use *, 32> Uses;
    DominatorTree DT(F);
    for (DomTreeNode *Block : depth_first(DT.getRootNode()))
      for (Instruction &I : *Block->getBlock())
        for (Use &U : I.operands())
          if (auto *GV = dyn_cast<GlobalValue>(U.get());
              GV && isAddressAcquisition(I))
            if (Cells.contains(GV))
              Uses.push_back(&U);
    for (BasicBlock &Block : F)
      if (!DT.isReachableFromEntry(&Block))
        for (Instruction &I : Block)
          for (Use &U : I.operands())
            if (auto *GV = dyn_cast<GlobalValue>(U.get());
                GV && isAddressAcquisition(I))
              if (Cells.contains(GV))
                Uses.push_back(&U);
    // Reuse dominating loads without loading on unrelated paths.
    // Ordinary CSE/loop optimization can further common invariant IAT reads.
    ImportLoads Loads;
    for (Use *U : Uses) {
      auto *GV = cast<GlobalValue>(U->get());
      if (GV->isThreadLocal())
        reportFatalUsageError(
            "thread-local object crosses a frozen PE boundary");
      GlobalVariable *&Cell = Cells[GV];
      if (!Cell)
        Cell = getImportCell(*GV);
      U->set(loadImportAddress(*U, *Cell, DT, Loads));
      Changed = true;
    }
  }
  // Locality on the surviving global now describes only direct local uses.
  // Foreign instruction uses have explicit native import loads. A symbol
  // assigned to an external provider still has no local defining storage.
  for (GlobalValue *GV : Targets)
    if (getOwner(*GV)->starts_with("pe:") || getOwner(*GV) == "image:") {
      if (GV->hasDLLImportStorageClass())
        GV->setDLLStorageClass(GlobalValue::DefaultStorageClass);
      GV->setDSOLocal(true);
      Changed = true;
    }
  return Changed;
}

class COFFOutputLocality final : public ModulePass {
public:
  static char ID;
  COFFOutputLocality() : ModulePass(ID) {
    initializeCOFFOutputLocalityPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(Module &M) override { return lowerCOFFOutputLocality(M); }
};
} // namespace

char COFFOutputLocality::ID = 0;
INITIALIZE_PASS(COFFOutputLocality, "coff-output-locality",
                "Lower resolved COFF output boundaries", false, false)

ModulePass *llvm::createCOFFOutputLocalityPass() {
  return new COFFOutputLocality();
}

PreservedAnalyses COFFOutputLocalityPass::run(Module &M,
                                              ModuleAnalysisManager &) {
  return lowerCOFFOutputLocality(M) ? PreservedAnalyses::none()
                                    : PreservedAnalyses::all();
}
