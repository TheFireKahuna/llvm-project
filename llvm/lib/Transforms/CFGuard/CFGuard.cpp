//===-- CFGuard.cpp - Control Flow Guard checks -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the IR transform to add Microsoft's Control Flow Guard
/// checks on Windows targets.
///
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/CFGuard.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/AttributeMask.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

using OperandBundleDef = OperandBundleDefT<Value *>;

#define DEBUG_TYPE "cfguard"

STATISTIC(CFGuardCounter, "Number of Control Flow Guard checks added");
STATISTIC(CFGuardProvenCounter,
          "Number of indirect calls whose target is proven");

constexpr StringRef GuardCheckFunctionName = "__guard_check_icall_fptr";
constexpr StringRef GuardDispatchFunctionName = "__guard_dispatch_icall_fptr";

namespace {

/// Adds Control Flow Guard (CFG) checks on indirect function calls/invokes.
/// These checks ensure that the target address corresponds to the start of an
/// address-taken function.
class CFGuardImpl {
public:
  using Mechanism = CFGuardPass::Mechanism;

  /// Inserts a Control Flow Guard (CFG) check on an indirect call using the CFG
  /// check mechanism. When the image is loaded, the loader puts the appropriate
  /// guard check function pointer in the __guard_check_icall_fptr global
  /// symbol. This checks that the target address is a valid address-taken
  /// function. The address of the target function is passed to the guard check
  /// function in an architecture-specific register (e.g. ECX on 32-bit X86,
  /// X15 on Aarch64, and R0 on ARM). The guard check function has no return
  /// value (if the target is invalid, the guard check funtion will raise an
  /// error).
  ///
  /// For example, the following LLVM IR:
  /// \code
  ///   %func_ptr = alloca i32 ()*, align 8
  ///   store i32 ()* @target_func, i32 ()** %func_ptr, align 8
  ///   %0 = load i32 ()*, i32 ()** %func_ptr, align 8
  ///   %1 = call i32 %0()
  /// \endcode
  ///
  /// is transformed to:
  /// \code
  ///   %func_ptr = alloca i32 ()*, align 8
  ///   store i32 ()* @target_func, i32 ()** %func_ptr, align 8
  ///   %0 = load i32 ()*, i32 ()** %func_ptr, align 8
  ///   %1 = load void (i8*)*, void (i8*)** @__guard_check_icall_fptr
  ///   %2 = bitcast i32 ()* %0 to i8*
  ///   call cfguard_checkcc void %1(i8* %2)
  ///   %3 = call i32 %0()
  /// \endcode
  ///
  /// For example, the following X86 assembly code:
  /// \code
  ///   movl  $_target_func, %eax
  ///   calll *%eax
  /// \endcode
  ///
  /// is transformed to:
  /// \code
  /// 	movl	$_target_func, %ecx
  /// 	calll	*___guard_check_icall_fptr
  /// 	calll	*%ecx
  /// \endcode
  ///
  /// \param CB indirect call to instrument.
  void insertCFGuardCheck(CallBase *CB);

  /// Inserts a Control Flow Guard (CFG) check on an indirect call using the CFG
  /// dispatch mechanism. When the image is loaded, the loader puts the
  /// appropriate guard check function pointer in the
  /// __guard_dispatch_icall_fptr global symbol. This checks that the target
  /// address is a valid address-taken function and, if so, tail calls the
  /// target. The target address is passed in an architecture-specific register
  /// (e.g. RAX on X86_64), with all other arguments for the target function
  /// passed as usual.
  ///
  /// For example, the following LLVM IR:
  /// \code
  ///   %func_ptr = alloca i32 ()*, align 8
  ///   store i32 ()* @target_func, i32 ()** %func_ptr, align 8
  ///   %0 = load i32 ()*, i32 ()** %func_ptr, align 8
  ///   %1 = call i32 %0()
  /// \endcode
  ///
  /// is transformed to:
  /// \code
  ///   %func_ptr = alloca i32 ()*, align 8
  ///   store i32 ()* @target_func, i32 ()** %func_ptr, align 8
  ///   %0 = load i32 ()*, i32 ()** %func_ptr, align 8
  ///   %1 = load i32 ()*, i32 ()** @__guard_dispatch_icall_fptr
  ///   %2 = call i32 %1() [ "cfguardtarget"(i32 ()* %0) ]
  /// \endcode
  ///
  /// For example, the following X86_64 assembly code:
  /// \code
  ///   leaq   target_func(%rip), %rax
  ///	  callq  *%rax
  /// \endcode
  ///
  /// is transformed to:
  /// \code
  ///   leaq   target_func(%rip), %rax
  ///   callq  *__guard_dispatch_icall_fptr(%rip)
  /// \endcode
  ///
  /// An indirect musttail call on AArch64 under the check mechanism instead
  /// becomes a call of GuardTailCallThunkName with the same bundle.
  ///
  /// \param CB indirect call to instrument.
  /// \param Dispatch the function the call goes to instead.
  void insertCFGuardDispatch(CallBase *CB, Value *Dispatch);

  bool doInitialization(Module &M);
  bool runOnFunction(Function &F);

private:
  Constant *getGuardFnGlobal(Module &M, StringRef Name);

  // Only add checks if the module has them enabled.
  ControlFlowGuardMode CFGuardModuleFlag = ControlFlowGuardMode::Disabled;
  Mechanism GuardMechanism = Mechanism::Check;
  FunctionType *GuardFnType = nullptr;
  PointerType *GuardFnPtrType = nullptr;
};

class CFGuard : public FunctionPass {
  CFGuardImpl Impl;

public:
  static char ID;

  // Default constructor required for the INITIALIZE_PASS macro.
  CFGuard() : FunctionPass(ID) {}

  bool doInitialization(Module &M) override { return Impl.doInitialization(M); }
  bool runOnFunction(Function &F) override { return Impl.runOnFunction(F); }
};

} // end anonymous namespace

void CFGuardImpl::insertCFGuardCheck(CallBase *CB) {
  assert(CB->getModule()->getTargetTriple().isOSWindows() &&
         "Only applicable for Windows targets");
  assert(CB->isIndirectCall() &&
         "Control Flow Guard checks can only be added to indirect calls");

  IRBuilder<> B(CB);
  Value *CalledOperand = CB->getCalledOperand();

  // If the indirect call is called within catchpad or cleanuppad,
  // we need to copy "funclet" bundle of the call.
  SmallVector<llvm::OperandBundleDef, 1> Bundles;
  if (auto Bundle = CB->getOperandBundle(LLVMContext::OB_funclet))
    Bundles.push_back(OperandBundleDef(*Bundle));

  // Load the global symbol as a pointer to the check function.
  LoadInst *GuardCheckLoad =
      B.CreateLoad(GuardFnPtrType,
                   getGuardFnGlobal(*CB->getModule(), GuardCheckFunctionName));

  // Create new call instruction. The CFGuard check should always be a call,
  // even if the original CallBase is an Invoke or CallBr instruction.
  CallInst *GuardCheck =
      B.CreateCall(GuardFnType, GuardCheckLoad, {CalledOperand}, Bundles);

  // Ensure that the first argument is passed in the correct register
  // (e.g. ECX on 32-bit X86 targets).
  GuardCheck->setCallingConv(CallingConv::CFGuard_Check);
}

void CFGuardImpl::insertCFGuardDispatch(CallBase *CB, Value *Dispatch) {
  assert(CB->getModule()->getTargetTriple().isOSWindows() &&
         "Only applicable for Windows targets");
  assert(CB->isIndirectCall() &&
         "Control Flow Guard checks can only be added to indirect calls");

  Value *CalledOperand = CB->getCalledOperand();

  // Add the original call target as a cfguardtarget operand bundle.
  SmallVector<llvm::OperandBundleDef, 1> Bundles;
  CB->getOperandBundlesAsDefs(Bundles);
  Bundles.emplace_back("cfguardtarget", CalledOperand);

  // Create a copy of the call/invoke instruction and add the new bundle.
  assert((isa<CallInst>(CB) || isa<InvokeInst>(CB)) &&
         "Unknown indirect call type");
  CallBase *NewCB = CallBase::Create(CB, Bundles, CB->getIterator());

  // Change the target of the call to be the guard dispatch function.
  NewCB->setCalledOperand(Dispatch);

  // Replace the original call/invoke with the new instruction.
  CB->replaceAllUsesWith(NewCB);

  // Delete the original call/invoke.
  CB->eraseFromParent();
}

bool CFGuardImpl::doInitialization(Module &M) {
  // Check if this module has the cfguard flag and read its value.
  CFGuardModuleFlag = M.getControlFlowGuardMode();

  // Skip modules for which CFGuard checks have been disabled.
  if (CFGuardModuleFlag != ControlFlowGuardMode::Enabled)
    return false;

  // Determine the guard mechanism to use.
  ControlFlowGuardMechanism MechanismOverride =
      ControlFlowGuardMechanism::Automatic;
  if (auto *CI = mdconst::dyn_extract_or_null<ConstantInt>(
          M.getModuleFlag("cfguard-mechanism")))
    MechanismOverride =
        static_cast<ControlFlowGuardMechanism>(CI->getZExtValue());
  switch (MechanismOverride) {
  case ControlFlowGuardMechanism::Check:
    GuardMechanism = Mechanism::Check;
    break;
  case ControlFlowGuardMechanism::Dispatch:
    GuardMechanism = Mechanism::Dispatch;
    break;
  default:
    // X86_64 uses dispatch; all other architectures use check.
    GuardMechanism =
        M.getTargetTriple().isX86_64() ? Mechanism::Dispatch : Mechanism::Check;
    break;
  }

  // Set up prototypes for the guard check and dispatch functions.
  GuardFnType =
      FunctionType::get(Type::getVoidTy(M.getContext()),
                        {PointerType::getUnqual(M.getContext())}, false);
  GuardFnPtrType = PointerType::get(M.getContext(), 0);
  return true;
}

Constant *CFGuardImpl::getGuardFnGlobal(Module &M, StringRef Name) {
  return M.getOrInsertGlobal(Name, GuardFnPtrType, [&] {
    auto *Var =
        new GlobalVariable(M, GuardFnPtrType, false,
                           GlobalVariable::ExternalLinkage, nullptr, Name);
    Var->setDSOLocal(true);
    return Var;
  });
}

// Whether the dispatch function can guard CB. On x86-64 it takes the target in
// RAX and uses R10 and R11 as scratch, so the call must lower as a Win64 or
// System V C call that passes nothing else in RAX: not a System V variadic
// call's vector count in AL, nor a System V Swift call's sret pointer.
static bool canUseDispatch(const CallBase &CB) {
  const Triple &TT = CB.getModule()->getTargetTriple();
  if (!TT.isX86_64())
    return true;
  CallingConv::ID CC = CB.getCallingConv();
  switch (CC) {
  case CallingConv::Win64:
    return true;
  case CallingConv::C:
  case CallingConv::Fast:
  case CallingConv::Tail:
  case CallingConv::Swift:
  case CallingConv::SwiftTail:
  case CallingConv::X86_FastCall:
  case CallingConv::X86_StdCall:
  case CallingConv::X86_ThisCall:
  case CallingConv::X86_VectorCall:
    if (TT.hasDefaultMSABI())
      return true;
    [[fallthrough]];
  case CallingConv::X86_64_SysV:
    return !CB.getFunctionType()->isVarArg() &&
           !((CC == CallingConv::Swift || CC == CallingConv::SwiftTail) &&
             CB.hasStructRetAttr());
  default:
    return false;
  }
}

// Returns true if I may call a function. A callee may spill a value that is
// live across the call to its own writable frame.
static bool mayCall(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  return isa<CallBase>(I) && !(II && II->isAssumeLikeIntrinsic());
}

// Returns true if I, without its flags, computes a value from every value of
// its operands: no operand makes it poison or undefined behaviour.
static bool isTotal(const Instruction &I) {
  using namespace PatternMatch;
  const APInt *C;
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
  case Instruction::GetElementPtr:
  case Instruction::ICmp:
  case Instruction::Select:
  case Instruction::PHI:
  case Instruction::Freeze:
    return true;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    return match(I.getOperand(1), m_APInt(C)) &&
           C->ult(I.getType()->getScalarSizeInBits());
  case Instruction::UDiv:
  case Instruction::URem:
    return match(I.getOperand(1), m_APInt(C)) && !C->isZero();
  default:
    return false;
  }
}

// Returns the range of the index V where BB uses it. Each step follows only
// what an operation computes, never a poison-generating flag, metadata, an
// attribute or an assumption: an index that breaks such a promise is exactly
// what an attacker supplies, and ValueTracking and LazyValueInfo read them
// whatever the query asks. Only a value computed after LastCall, the last call
// before the use, is followed, since a callee may spill an older one. With no
// such call, a compare on the only edge into BB bounds the value too, and is
// added to Compares.
static ConstantRange getIndexRange(Value *V, BasicBlock *BB,
                                   const Instruction *LastCall,
                                   SmallVectorImpl<Value *> &Compares,
                                   unsigned Depth = 6) {
  using namespace PatternMatch;
  const APInt *C;
  if (match(V, m_APInt(C)))
    return ConstantRange(*C);
  unsigned BitWidth = V->getType()->getScalarSizeInBits();
  ConstantRange CR = ConstantRange::getFull(BitWidth);

  // The compare must be in the predecessor with no call after it, so that BB
  // uses the value it tested.
  BasicBlock *Pred = BB->getSinglePredecessor();
  auto *BI =
      Pred && !LastCall ? dyn_cast<CondBrInst>(Pred->getTerminator()) : nullptr;
  auto *Cmp = BI ? dyn_cast<ICmpInst>(BI->getCondition()) : nullptr;
  if (Cmp && Cmp->getParent() == Pred &&
      match(Cmp, m_ICmp(m_Specific(V), m_APInt(C))) &&
      none_of(make_range(Cmp->getIterator(), BI->getIterator()), mayCall)) {
    CR = ConstantRange::makeExactICmpRegion(BI->getSuccessor(0) == BB
                                                ? Cmp->getPredicate()
                                                : Cmp->getInversePredicate(),
                                            *C);
    Compares.push_back(Cmp);
  }

  auto *I = dyn_cast<Instruction>(V);
  if (!I || !Depth || I->getParent() != BB ||
      (LastCall && I->comesBefore(LastCall)) || !isTotal(*I))
    return CR;
  auto Operand = [&] {
    return getIndexRange(I->getOperand(0), BB, LastCall, Compares, Depth - 1);
  };
  if (isa<ZExtInst, SExtInst, TruncInst>(I))
    return CR.intersectWith(
        Operand().castOp(cast<CastInst>(I)->getOpcode(), BitWidth));
  if (match(I, m_BinOp(m_Value(), m_APInt(C))))
    return CR.intersectWith(Operand().binaryOp(
        cast<BinaryOperator>(I)->getOpcode(), ConstantRange(*C)));
  return CR;
}

// The proof reads no promise whose breach is undefined behaviour, but the
// backend would: an nneg zext may become a sext, a samesign compare a signed
// one, and a !range may delete the mask that bounds the index. Drops every
// such promise from Values and from all they are computed from, so that the
// backend computes what the proof read. Returns false if one cannot be
// dropped. A loaded value and a call's result are what the machine holds,
// whatever was promised of them.
static bool dropPromises(SmallVectorImpl<Value *> &Values) {
  AttributeMask Promises = AttributeFuncs::getUBImplyingAttributes();
  Promises.addAttribute(Attribute::Range)
      .addAttribute(Attribute::NonNull)
      .addAttribute(Attribute::Alignment)
      .addAttribute(Attribute::ZExt)
      .addAttribute(Attribute::SExt);
  SmallPtrSet<Value *, 16> Seen;
  while (!Values.empty()) {
    Value *V = Values.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    // Without zeroext or signext, the function extends the argument itself.
    if (auto *Arg = dyn_cast<Argument>(V))
      Arg->getParent()->removeParamAttrs(Arg->getArgNo(), Promises);
    auto *I = dyn_cast<Instruction>(V);
    if (!I)
      continue;
    I->dropPoisonGeneratingAnnotations();
    I->dropUBImplyingAttrsAndMetadata();
    if (auto *CB = dyn_cast<CallBase>(I)) {
      // An intrinsic is a computation, and none is known to be total. Without
      // zeroext or signext the caller extends the result itself, but what the
      // callee's declaration promises cannot be dropped here.
      CB->removeRetAttrs(Promises);
      if (isa<IntrinsicInst>(CB) || CB->getRange() ||
          CB->hasRetAttr(Attribute::ZExt) || CB->hasRetAttr(Attribute::SExt))
        return false;
    } else if (isa<LoadInst>(I)) {
      // Instruction selection may forward a value stored in the block.
      for (Instruction &Prev :
           make_range(I->getParent()->begin(), I->getIterator()))
        if (auto *Store = dyn_cast<StoreInst>(&Prev))
          Values.push_back(Store->getValueOperand());
    } else if (isTotal(*I)) {
      Values.append(I->op_begin(), I->op_end());
    } else {
      return false;
    }
  }
  return true;
}

// Returns true if every word of C is null or the address of a function that
// this image defines or reaches through its own thunk and that carries Type,
// the type a KCFI check of the call would test, if there is one: leaving the
// call unchecked must accept no target the check would reject.
static bool holdsOnlyFunctions(const Constant *C, const ConstantInt *Type) {
  if (isa<ConstantPointerNull, ConstantAggregateZero>(C))
    return true;
  if (auto *F = dyn_cast<Function>(C)) {
    const MDNode *MD = F->getMetadata(LLVMContext::MD_kcfi_type);
    return !F->hasDLLImportStorageClass() &&
           (!Type ||
            (MD && mdconst::extract<ConstantInt>(MD->getOperand(0)) == Type));
  }
  return isa<ConstantArray, ConstantStruct>(C) &&
         all_of(C->operands(), [&](const Use &Op) {
           return holdsOnlyFunctions(cast<Constant>(Op), Type);
         });
}

// Returns the load of CB's target if the target is provably one of a fixed
// set of functions and so needs no check: loaded from a table that is
// read-only once the image is loaded and holds only functions, at an index
// proven to select one of its words. The proof must not rest on inbounds,
// which only makes an out-of-range address poison, and the promises it does
// not rest on are dropped.
static LoadInst *proveTarget(CallBase &CB) {
  BasicBlock *BB = CB.getParent();
  auto *Load = dyn_cast<LoadInst>(CB.getCalledOperand());
  auto *GEP =
      Load ? dyn_cast<GetElementPtrInst>(Load->getPointerOperand()) : nullptr;
  // The mark is on the load, so no other call may be made through it.
  if (!GEP || Load->getParent() != BB || GEP->getParent() != BB ||
      any_of(Load->users(), [&](const User *U) {
        auto *Call = dyn_cast<CallBase>(U);
        return Call && Call != &CB && Call->getCalledOperand() == Load;
      }))
    return nullptr;

  const DataLayout &DL = CB.getDataLayout();
  unsigned IndexWidth = DL.getIndexTypeSizeInBits(GEP->getType());
  APInt Offset(IndexWidth, 0);
  auto *GV = dyn_cast<GlobalVariable>(
      GEP->getPointerOperand()->stripAndAccumulateConstantOffsets(
          DL, Offset, /*AllowNonInbounds=*/true));
  std::optional<OperandBundleUse> KCFI =
      CB.getOperandBundle(LLVMContext::OB_kcfi);
  // A thread-local or explicitly placed constant may be writable.
  if (!GV || !GV->isConstant() || !GV->hasDefinitiveInitializer() ||
      GV->isDeclarationForLinker() || GV->isThreadLocal() || GV->hasSection() ||
      !holdsOnlyFunctions(GV->getInitializer(),
                          KCFI ? cast<ConstantInt>(KCFI->Inputs[0]) : nullptr))
    return nullptr;

  // Every offset must select a whole word of the table.
  SmallMapVector<Value *, APInt, 4> VariableOffsets;
  uint64_t WordSize = DL.getTypeStoreSize(Load->getType());
  uint64_t TableSize = DL.getTypeAllocSize(GV->getValueType());
  if (!GEP->collectOffset(DL, IndexWidth, VariableOffsets, Offset) ||
      VariableOffsets.size() != 1 || TableSize < WordSize ||
      VariableOffsets.front().second.urem(WordSize) || Offset.urem(WordSize))
    return nullptr;

  // The proof holds for values in registers. Requiring them to be computed
  // after the last call keeps them out of a callee's frame; the backend checks
  // that register allocation kept them out of this function's.
  const Instruction *LastCall = CB.getPrevNode();
  while (LastCall && !mayCall(*LastCall))
    LastCall = LastCall->getPrevNode();
  if (LastCall && GEP->comesBefore(LastCall))
    return nullptr;

  const auto &[Index, Scale] = VariableOffsets.front();
  SmallVector<Value *, 8> Values = {Index};
  ConstantRange Offsets = getIndexRange(Index, BB, LastCall, Values)
                              .sextOrTrunc(IndexWidth)
                              .multiply(ConstantRange(Scale))
                              .add(ConstantRange(Offset));
  return ConstantRange(APInt(IndexWidth, 0),
                       APInt(IndexWidth, TableSize - WordSize + 1))
                     .contains(Offsets) &&
                 dropPromises(Values)
             ? Load
             : nullptr;
}

bool CFGuardImpl::runOnFunction(Function &F) {
  // Skip modules for which CFGuard checks have been disabled.
  if (CFGuardModuleFlag != ControlFlowGuardMode::Enabled)
    return false;

  // A proven call is guarded like any other. The backend alone can drop the
  // guard, once it knows that the marked target stayed in its register.
  bool MarkProven =
      F.getParent()->getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment();
  SmallVector<CallBase *, 8> IndirectCalls;

  // Iterate over the instructions to find all indirect call/invoke/callbr
  // instructions. Make a separate list of pointers to indirect
  // call/invoke/callbr instructions because the original instructions will be
  // deleted as the checks are added.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (CB && CB->isIndirectCall() && !CB->hasFnAttr("guard_nocf")) {
        IndirectCalls.push_back(CB);
        CFGuardCounter++;
        if (LoadInst *Load = MarkProven ? proveTarget(*CB) : nullptr) {
          Load->setMetadata(LLVMContext::MD_cfguard_proven,
                            MDNode::get(F.getContext(), {}));
          CFGuardProvenCounter++;
        }
      }
    }
  }

  // If no checks are needed, return early.
  if (IndirectCalls.empty())
    return false;

  // For each indirect call/invoke, add the appropriate dispatch or check.
  Module &M = *F.getParent();
  for (CallBase *CB : IndirectCalls) {
    if (GuardMechanism == Mechanism::Dispatch && canUseDispatch(*CB)) {
      insertCFGuardDispatch(
          CB, IRBuilder<>(CB).CreateLoad(
                  CB->getCalledOperand()->getType(),
                  getGuardFnGlobal(M, GuardDispatchFunctionName)));
    } else if (CB->isMustTailCall() && M.getTargetTriple().isAArch64()) {
      FunctionCallee Thunk = M.getOrInsertFunction(
          GuardTailCallThunkName, Type::getVoidTy(M.getContext()));
      cast<Function>(Thunk.getCallee())->setDSOLocal(true);
      insertCFGuardDispatch(CB, Thunk.getCallee());
    } else {
      insertCFGuardCheck(CB);
    }
  }

  return true;
}

PreservedAnalyses CFGuardPass::run(Function &F, FunctionAnalysisManager &FAM) {
  CFGuardImpl Impl;
  bool Changed = Impl.doInitialization(*F.getParent());
  Changed |= Impl.runOnFunction(F);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

char CFGuard::ID = 0;

FunctionPass *llvm::createCFGuardPass() { return new CFGuard(); }

bool llvm::isCFGuardCall(const CallBase *CB) {
  return CB->getCallingConv() == CallingConv::CFGuard_Check ||
         CB->countOperandBundlesOfType(LLVMContext::OB_cfguardtarget);
}

bool llvm::isCFGuardFunction(const GlobalValue *GV) {
  if (GV->getLinkage() != GlobalValue::ExternalLinkage)
    return false;

  StringRef Name = GV->getName();
  return Name == GuardCheckFunctionName || Name == GuardDispatchFunctionName;
}
