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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace llvm;

using OperandBundleDef = OperandBundleDefT<Value *>;

#define DEBUG_TYPE "cfguard"

STATISTIC(CFGuardCounter, "Number of Control Flow Guard checks added");
STATISTIC(CFGuardProvenCounter,
          "Number of indirect calls whose target is proven");

constexpr StringRef GuardCheckFunctionName = "__guard_check_icall_fptr";
constexpr StringRef GuardDispatchFunctionName = "__guard_dispatch_icall_fptr";
using COFF::KCFICheckThunkPrefix;
using COFF::KCFIDispatchThunkPrefix;
using COFF::KCFILocalCheckThunkPrefix;
using COFF::KCFILocalDispatchThunkPrefix;
using COFF::KCFIMemberCheckThunkPrefix;
using COFF::KCFIMemberDispatchThunkPrefix;
using COFF::KCFIMemberLocalCheckThunkPrefix;
using COFF::KCFIMemberLocalDispatchThunkPrefix;
using COFF::KCFIVfnCheckThunkPrefix;
constexpr unsigned MaxIndexRangeDepth = 6;

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
  /// \param CB indirect call to instrument.
  void insertCFGuardDispatch(CallBase *CB);

  /// Replaces a call to llvm.kcfi.check, at either type word of a prefix with
  /// a marker, with a call to the per-type check thunk that checks that word,
  /// which takes the target as the guard check function does.
  void insertKCFICheckThunk(IntrinsicInst *II);

  bool doInitialization(Module &M);
  bool runOnFunction(Function &F);

private:
  /// Returns the global holding the pointer to the guard function Name,
  /// declaring it in M if it is not already declared, and keeps it in Cache.
  Constant *getGuardFnGlobal(Module &M, StringRef Name, Constant *&Cache);

  /// Returns the per-type KCFI thunk that checks the type of the target of an
  /// indirect call with the kcfi bundle of CB and continues into the guard
  /// function, or null if CB is checked where it calls.
  Function *getKCFIThunk(CallBase &CB, StringRef Prefix);

  /// Returns the member thunk that checks the target Target of the call Call
  /// of KCFI type Type, if tests of the membership tags of Target dominate it,
  /// or null.
  Function *getKCFIMemberThunk(Instruction *Call, Value *Target, uint64_t Type,
                               bool Dispatch, bool Local);

  /// Replaces each test of membership tags. One whose target a call through a
  /// member thunk checks is true; any other becomes a call to the member
  /// check thunk, which fails fast where the tags do not match, or false if
  /// it lists no tag.
  void lowerKCFIMemberTests();

  // The tests of membership tags in the function, and those that a member
  // thunk checks with a call they dominate.
  SmallVector<IntrinsicInst *, 2> MemberTests;
  SmallPtrSet<IntrinsicInst *, 2> FusedMemberTests;
  std::optional<DominatorTree> DT;

  // Only add checks if the module has them enabled.
  ControlFlowGuardMode CFGuardModuleFlag = ControlFlowGuardMode::Disabled;
  // Whether calls with a kcfi bundle go through a per-type thunk, which the
  // backend emits, whether or not the module has checks enabled.
  bool UseKCFIThunks = false;
  // Whether calls to llvm.kcfi.check go through a per-type check thunk.
  bool UseKCFICheckThunks = false;
  // Whether the loads of targets proven to come from a constant table of
  // functions are marked for the backend, which leaves the call unchecked if
  // the target stays in its register.
  bool MarkProvenCalls = false;
  Mechanism GuardMechanism = Mechanism::Check;
  // The globals holding the pointers to the guard check and dispatch
  // functions, once a call needs them.
  Constant *GuardCheckFnGlobal = nullptr;
  Constant *GuardDispatchFnGlobal = nullptr;
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

  // Load the global symbol as a pointer to the check function, unless a
  // KCFI thunk checks the type and then continues into the check function.
  Function *KCFIThunk = getKCFIThunk(*CB, KCFICheckThunkPrefix);
  Value *GuardCheckFn = KCFIThunk;
  if (!KCFIThunk)
    GuardCheckFn =
        B.CreateLoad(GuardFnPtrType,
                     getGuardFnGlobal(*CB->getModule(), GuardCheckFunctionName,
                                      GuardCheckFnGlobal));

  // Create new call instruction. The CFGuard check should always be a call,
  // even if the original CallBase is an Invoke or CallBr instruction.
  CallInst *GuardCheck =
      B.CreateCall(GuardFnType, GuardCheckFn, {CalledOperand}, Bundles);

  // Ensure that the first argument is passed in the correct register
  // (e.g. ECX on 32-bit X86 targets).
  GuardCheck->setCallingConv(CallingConv::CFGuard_Check);

  // The thunk checked the type, so the call itself is not checked again.
  if (!KCFIThunk)
    return;
  CallBase *NewCB = CallBase::removeOperandBundle(CB, LLVMContext::OB_kcfi,
                                                  CB->getIterator());
  CB->replaceAllUsesWith(NewCB);
  CB->eraseFromParent();
}

void CFGuardImpl::insertCFGuardDispatch(CallBase *CB) {
  assert(CB->getModule()->getTargetTriple().isOSWindows() &&
         "Only applicable for Windows targets");
  assert(CB->isIndirectCall() &&
         "Control Flow Guard checks can only be added to indirect calls");

  IRBuilder<> B(CB);
  Value *CalledOperand = CB->getCalledOperand();
  Type *CalledOperandType = CalledOperand->getType();

  // Load the global as a pointer to a function of the same type, unless a
  // KCFI thunk checks the type of the target and then continues into the
  // dispatch function.
  Function *KCFIThunk = getKCFIThunk(*CB, KCFIDispatchThunkPrefix);
  Value *GuardDispatchFn = KCFIThunk;
  if (!KCFIThunk)
    GuardDispatchFn = B.CreateLoad(CalledOperandType,
                                   getGuardFnGlobal(*CB->getModule(),
                                                    GuardDispatchFunctionName,
                                                    GuardDispatchFnGlobal));

  // Add the original call target as a cfguardtarget operand bundle. A thunk
  // checks the type, so the call keeps no kcfi bundle.
  SmallVector<llvm::OperandBundleDef, 1> Bundles;
  CB->getOperandBundlesAsDefs(Bundles);
  if (KCFIThunk)
    llvm::erase_if(Bundles, [](const OperandBundleDef &Bundle) {
      return Bundle.getTag() == "kcfi";
    });
  Bundles.emplace_back("cfguardtarget", CalledOperand);

  // Create a copy of the call/invoke instruction and add the new bundle.
  assert((isa<CallInst>(CB) || isa<InvokeInst>(CB)) &&
         "Unknown indirect call type");
  CallBase *NewCB = CallBase::Create(CB, Bundles, CB->getIterator());

  // Change the target of the call to be the guard dispatch function.
  NewCB->setCalledOperand(GuardDispatchFn);

  // Replace the original call/invoke with the new instruction.
  CB->replaceAllUsesWith(NewCB);

  // Delete the original call/invoke.
  CB->eraseFromParent();
}

bool CFGuardImpl::doInitialization(Module &M) {
  // Check if this module has the cfguard flag and read its value.
  CFGuardModuleFlag = M.getControlFlowGuardMode();
  GuardCheckFnGlobal = nullptr;
  GuardDispatchFnGlobal = nullptr;

  // A module whose KCFI prefixes carry a marker checks each indirect call's
  // type in a per-type thunk, which continues into the guard function the
  // image defines, whether or not it has checks enabled. X86-64 emits the
  // thunks for both mechanisms, and AArch64 for the check mechanism. Both emit
  // the check thunks that calls to llvm.kcfi.check go through.
  const Triple &TT = M.getTargetTriple();
  bool HasKCFIMarker = hasKCFIThunks(M);

  // Skip modules for which CFGuard checks have been disabled.
  if (CFGuardModuleFlag != ControlFlowGuardMode::Enabled && !HasKCFIMarker)
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
  UseKCFIThunks =
      HasKCFIMarker && (TT.isX86_64() || GuardMechanism == Mechanism::Check);
  UseKCFICheckThunks = HasKCFIMarker;
  MarkProvenCalls = TT.isWindowsItaniumOrNTPOSIXEnvironment();

  // Set up prototypes for the guard check and dispatch functions.
  GuardFnType =
      FunctionType::get(Type::getVoidTy(M.getContext()),
                        {PointerType::getUnqual(M.getContext())}, false);
  GuardFnPtrType = PointerType::get(M.getContext(), 0);

  if (CFGuardModuleFlag == ControlFlowGuardMode::Enabled) {
    if (GuardMechanism == Mechanism::Check)
      getGuardFnGlobal(M, GuardCheckFunctionName, GuardCheckFnGlobal);
    else
      getGuardFnGlobal(M, GuardDispatchFunctionName, GuardDispatchFnGlobal);
  }

  return true;
}

Constant *CFGuardImpl::getGuardFnGlobal(Module &M, StringRef Name,
                                        Constant *&Cache) {
  if (!Cache)
    Cache = M.getOrInsertGlobal(Name, GuardFnPtrType, [&] {
      auto *Var =
          new GlobalVariable(M, GuardFnPtrType, false,
                             GlobalVariable::ExternalLinkage, nullptr, Name);
      Var->setDSOLocal(true);
      return Var;
    });
  return Cache;
}

// Attaches the !kcfi_thunk metadata the backend reads to find and describe a
// thunk, so that it parses no thunk name.
static void setKCFIThunkMetadata(Function *Thunk, unsigned Kind, uint64_t Type,
                                 unsigned Flags) {
  LLVMContext &Ctx = Thunk->getContext();
  auto *I32 = Type::getInt32Ty(Ctx);
  Metadata *Ops[] = {
      ConstantAsMetadata::get(ConstantInt::get(I32, Kind)),
      ConstantAsMetadata::get(ConstantInt::get(I32, uint32_t(Type))),
      ConstantAsMetadata::get(ConstantInt::get(I32, Flags))};
  Thunk->setMetadata("kcfi_thunk", MDNode::get(Ctx, Ops));
}

// Returns the declaration of the per-type thunk Prefix<type>, which the
// backend emits.
static Function *declareKCFIThunk(Module &M, StringRef Prefix, uint64_t Type) {
  std::string Name =
      (Prefix + utohexstr(Type, /*LowerCase=*/true, /*Width=*/8)).str();
  auto *Thunk = cast<Function>(
      M.getOrInsertFunction(Name, Type::getVoidTy(M.getContext())).getCallee());
  Thunk->setVisibility(GlobalValue::HiddenVisibility);
  Thunk->setDSOLocal(true);
  unsigned Flags = 0;
  if (Prefix == KCFILocalDispatchThunkPrefix ||
      Prefix == KCFILocalCheckThunkPrefix)
    Flags |= KCFIThunkLocal;
  if (Prefix == KCFIVfnCheckThunkPrefix)
    Flags |= KCFIThunkVfn;
  unsigned Kind = (Prefix == KCFIDispatchThunkPrefix ||
                   Prefix == KCFILocalDispatchThunkPrefix)
                      ? KCFIThunkDispatch
                      : KCFIThunkCheck;
  setKCFIThunkMetadata(Thunk, Kind, Type, Flags);
  return Thunk;
}

// Returns the declaration of the member thunk Prefix<type>_<tag>..., which
// checks that a target carries one of the membership tags Tags, and on a
// miss continues into the type's ordinary thunk. The backend reads the tags
// from the declaration.
static Function *declareKCFIMemberThunk(Module &M, StringRef Prefix,
                                        uint64_t Type, MDNode *Tags) {
  std::string Name =
      (Prefix + utohexstr(Type, /*LowerCase=*/true, /*Width=*/8)).str();
  for (const MDOperand &Tag : Tags->operands())
    Name += "_" + utohexstr(mdconst::extract<ConstantInt>(Tag)->getZExtValue(),
                            /*LowerCase=*/true, /*Width=*/8);
  auto *Thunk = cast<Function>(
      M.getOrInsertFunction(Name, Type::getVoidTy(M.getContext())).getCallee());
  Thunk->setVisibility(GlobalValue::HiddenVisibility);
  Thunk->setDSOLocal(true);
  Thunk->setMetadata("kcfi_member_tags", Tags);
  unsigned Flags = KCFIThunkMember;
  if (Prefix == KCFIMemberLocalDispatchThunkPrefix ||
      Prefix == KCFIMemberLocalCheckThunkPrefix)
    Flags |= KCFIThunkLocal;
  unsigned Kind = (Prefix == KCFIMemberDispatchThunkPrefix ||
                   Prefix == KCFIMemberLocalDispatchThunkPrefix)
                      ? KCFIThunkDispatch
                      : KCFIThunkCheck;
  setKCFIThunkMetadata(Thunk, Kind, Type, Flags);
  return Thunk;
}

// Returns the tags a test of membership tags lists, as a tuple: a tuple of one
// tag is a value as metadata in an argument.
static MDNode *getKCFIMemberTags(IntrinsicInst *Test) {
  Metadata *MD = cast<MetadataAsValue>(Test->getArgOperand(1))->getMetadata();
  if (auto *C = dyn_cast<ConstantAsMetadata>(MD))
    return MDNode::get(Test->getContext(), C);
  return cast<MDNode>(MD);
}

// A member thunk compares the target's KCFI type as well as its tags, so a
// call takes the tags of every test of its target that dominates it: a tag of
// another type's class matches no function of the call's type. A test checked
// this way is true; the check it asked for happens at the call, before
// control reaches the target.
Function *CFGuardImpl::getKCFIMemberThunk(Instruction *Call, Value *Target,
                                          uint64_t Type, bool Dispatch,
                                          bool Local) {
  SmallVector<Metadata *, 2> Tags;
  bool Tested = false;
  for (IntrinsicInst *Test : MemberTests) {
    if (Test->getArgOperand(0) != Target)
      continue;
    if (!DT)
      DT.emplace(*Call->getFunction());
    if (!DT->dominates(Test, Call))
      continue;
    Tested = true;
    FusedMemberTests.insert(Test);
    for (const MDOperand &Tag : getKCFIMemberTags(Test)->operands())
      if (!is_contained(Tags, Tag.get()))
        Tags.push_back(Tag.get());
  }
  if (!Tested)
    return nullptr;
  Module &M = *Call->getModule();
  // A miss continues into the type's ordinary thunk.
  StringRef Prefix;
  if (Dispatch) {
    declareKCFIThunk(
        M, Local ? KCFILocalDispatchThunkPrefix : KCFIDispatchThunkPrefix,
        Type);
    Prefix = Local ? KCFIMemberLocalDispatchThunkPrefix
                   : KCFIMemberDispatchThunkPrefix;
  } else {
    declareKCFIThunk(
        M, Local ? KCFILocalCheckThunkPrefix : KCFICheckThunkPrefix, Type);
    Prefix =
        Local ? KCFIMemberLocalCheckThunkPrefix : KCFIMemberCheckThunkPrefix;
  }
  return declareKCFIMemberThunk(M, Prefix, Type,
                                MDTuple::get(M.getContext(), Tags));
}

Function *CFGuardImpl::getKCFIThunk(CallBase &CB, StringRef Prefix) {
  if (!UseKCFIThunks)
    return nullptr;
  std::optional<OperandBundleUse> Bundle =
      CB.getOperandBundle(LLVMContext::OB_kcfi);
  if (!Bundle)
    return nullptr;
  auto *TypeId = cast<ConstantInt>(Bundle->Inputs[0]);
  // Every target of a call marked kcfi_local is in this image, so its thunk
  // may fail fast on a target outside it.
  bool Local = CB.getMetadata("kcfi_local");
  // A call whose target LTO tests by membership tags takes the member thunk.
  if (Function *Thunk =
          getKCFIMemberThunk(&CB, CB.getCalledOperand(), TypeId->getZExtValue(),
                             Prefix == KCFIDispatchThunkPrefix, Local))
    return Thunk;
  if (Local)
    Prefix = Prefix == KCFIDispatchThunkPrefix ? KCFILocalDispatchThunkPrefix
                                               : KCFILocalCheckThunkPrefix;
  return declareKCFIThunk(*CB.getModule(), Prefix, TypeId->getZExtValue());
}

void CFGuardImpl::insertKCFICheckThunk(IntrinsicInst *II) {
  // The type word at offset 4 is the ordinary type, and the one at offset 16
  // the second type that a function which can occupy a vtable slot carries.
  uint64_t Offset = cast<ConstantInt>(II->getArgOperand(2))->getZExtValue();
  uint64_t Type = cast<ConstantInt>(II->getArgOperand(1))->getZExtValue();
  Function *Thunk = nullptr;
  if (Offset == 4)
    Thunk = getKCFIMemberThunk(II, II->getArgOperand(0), Type,
                               /*Dispatch=*/false, /*Local=*/false);
  if (!Thunk)
    Thunk = declareKCFIThunk(
        *II->getModule(),
        Offset == 4 ? KCFICheckThunkPrefix : KCFIVfnCheckThunkPrefix, Type);

  IRBuilder<> B(II);
  SmallVector<llvm::OperandBundleDef, 1> Bundles;
  if (auto Bundle = II->getOperandBundle(LLVMContext::OB_funclet))
    Bundles.push_back(OperandBundleDef(*Bundle));
  CallInst *Check =
      B.CreateCall(GuardFnType, Thunk, {II->getArgOperand(0)}, Bundles);
  Check->setCallingConv(CallingConv::CFGuard_Check);
  II->eraseFromParent();
}

// Returns true if the dispatch mechanism can guard CB. On x86-64 it takes the
// target in RAX, so the call's convention must pass nothing else there: not
// AL, in which a variadic call under the System V convention passes the number
// of vector registers it uses, nor the sret pointer of a Swift call under that
// convention. It must also put the target in RAX, and treat R10 and R11 as
// clobbered, as the dispatch function uses them. Any other call takes a check.
static bool canUseDispatch(const CallBase &CB) {
  const Triple &TT = CB.getModule()->getTargetTriple();
  if (!TT.isX86_64())
    return true;

  // Whether the call uses the Win64 convention, as X86 lowering decides it.
  bool IsWin64;
  switch (CB.getCallingConv()) {
  case CallingConv::C:
  case CallingConv::Fast:
  case CallingConv::Tail:
  case CallingConv::Swift:
  case CallingConv::SwiftTail:
  case CallingConv::X86_FastCall:
  case CallingConv::X86_StdCall:
  case CallingConv::X86_ThisCall:
  case CallingConv::X86_VectorCall:
    // These follow the target's default, which is System V on NT-POSIX.
    IsWin64 = !TT.isWindowsNTPOSIXEnvironment();
    break;
  case CallingConv::Win64:
    IsWin64 = true;
    break;
  case CallingConv::X86_64_SysV:
    IsWin64 = false;
    break;
  default:
    return false;
  }
  if (IsWin64)
    return true;
  if (CB.getFunctionType()->isVarArg())
    return false;
  if (CB.getCallingConv() == CallingConv::Swift ||
      CB.getCallingConv() == CallingConv::SwiftTail)
    for (unsigned I = 0, E = CB.arg_size(); I != E; ++I)
      if (CB.paramHasAttr(I, Attribute::StructRet))
        return false;
  return true;
}

// Returns true if I may be a call. A callee may spill a value that is live
// across it to its own writable frame.
static bool mayCall(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  return isa<CallBase>(I) && !(II && II->isAssumeLikeIntrinsic());
}

// Returns the range to which the conditional branch that is the only way into
// BB constrains V. The compare must be in that branch's block with no call
// after it, so that BB uses the value it tested.
static ConstantRange getEdgeRange(const Value *V, const BasicBlock *BB) {
  using namespace PatternMatch;
  ConstantRange Full =
      ConstantRange::getFull(V->getType()->getScalarSizeInBits());
  const BasicBlock *Pred = BB->getSinglePredecessor();
  auto *BI = Pred ? dyn_cast<CondBrInst>(Pred->getTerminator()) : nullptr;
  const APInt *C;
  if (!BI || !match(BI->getCondition(), m_ICmp(m_Specific(V), m_APInt(C))))
    return Full;
  // A same-sign compare may be lowered with either signedness, so it proves
  // neither range.
  auto *Cmp = cast<ICmpInst>(BI->getCondition());
  if (Cmp->getParent() != Pred || Cmp->hasSameSign())
    return Full;
  for (const Instruction *I = Cmp->getNextNode(); I != BI; I = I->getNextNode())
    if (mayCall(*I))
      return Full;
  CmpInst::Predicate P = BI->getSuccessor(0) == BB ? Cmp->getPredicate()
                                                   : Cmp->getInversePredicate();
  return ConstantRange::makeExactICmpRegion(P, *C);
}

// Returns the range of the index V where BB uses it. Each step follows only
// the semantics of an operation, never a poison-generating flag, metadata, an
// attribute or an assumption: an index that breaks such a promise is exactly
// what an attacker supplies. computeConstantRange, computeKnownBits and
// LazyValueInfo are not used because each reads range attributes, or flags,
// !range and assumptions, whatever the query asks. Only values computed after
// LastCall, the last call before the use, count, since a callee may spill an
// older one.
static ConstantRange computeIndexRange(const Value *V, const BasicBlock *BB,
                                       const Instruction *LastCall,
                                       unsigned Depth = 0) {
  using namespace PatternMatch;
  if (auto *C = dyn_cast<ConstantInt>(V))
    return ConstantRange(C->getValue());
  unsigned BitWidth = V->getType()->getScalarSizeInBits();
  ConstantRange CR =
      LastCall ? ConstantRange::getFull(BitWidth) : getEdgeRange(V, BB);
  auto *I = dyn_cast<Instruction>(V);
  if (!I || I->getParent() != BB || (LastCall && I->comesBefore(LastCall)) ||
      Depth == MaxIndexRangeDepth)
    return CR;

  if (isa<ZExtInst, SExtInst, TruncInst>(I))
    return CR.intersectWith(
        computeIndexRange(I->getOperand(0), BB, LastCall, Depth + 1)
            .castOp(cast<CastInst>(I)->getOpcode(), BitWidth));

  const APInt *C;
  auto *BO = dyn_cast<BinaryOperator>(I);
  if (!BO || !match(BO->getOperand(1), m_APInt(C)))
    return CR;
  switch (BO->getOpcode()) {
  case Instruction::And:
    break;
  case Instruction::Shl:
  case Instruction::LShr:
    if (C->uge(BitWidth))
      return CR;
    break;
  case Instruction::URem:
    if (C->isZero())
      return CR;
    break;
  default:
    return CR;
  }
  return CR.intersectWith(
      computeIndexRange(BO->getOperand(0), BB, LastCall, Depth + 1)
          .binaryOp(BO->getOpcode(), ConstantRange(*C)));
}

// Returns the type a call's kcfi bundle checks, or null.
static const ConstantInt *getKCFIType(const CallBase &CB) {
  std::optional<OperandBundleUse> Bundle =
      CB.getOperandBundle(LLVMContext::OB_kcfi);
  return Bundle ? cast<ConstantInt>(Bundle->Inputs[0]) : nullptr;
}

// Returns true if every pointer-sized word of C is null or the address of a
// function this image defines or reaches through its own thunk. With a KCFI
// type, each function must carry it, so that leaving the call unchecked
// accepts no target the check would reject.
static bool holdsOnlyFunctions(const Constant *C, const ConstantInt *Type) {
  if (isa<ConstantPointerNull, ConstantAggregateZero>(C))
    return true;
  if (auto *F = dyn_cast<Function>(C)) {
    if (F->hasDLLImportStorageClass())
      return false;
    if (!Type)
      return true;
    const MDNode *MD = F->getMetadata(LLVMContext::MD_kcfi_type);
    return MD && mdconst::extract<ConstantInt>(MD->getOperand(0)) == Type;
  }
  return isa<ConstantArray, ConstantStruct>(C) &&
         all_of(C->operands(), [&](const Use &Op) {
           return holdsOnlyFunctions(cast<Constant>(Op), Type);
         });
}

// Returns true if CB's target is loaded from a constant table of functions at
// an index proven to select one of its words, so it is one of a fixed set of
// functions and needs no check. The table must be read-only once the image is
// loaded, and the proof must not rest on inbounds, which only makes an
// out-of-range address poison.
static bool hasProvenTarget(const CallBase &CB) {
  const BasicBlock *BB = CB.getParent();
  auto *Load = dyn_cast<LoadInst>(CB.getCalledOperand());
  auto *GEP =
      Load ? dyn_cast<GetElementPtrInst>(Load->getPointerOperand()) : nullptr;
  if (!GEP || Load->getParent() != BB || GEP->getParent() != BB)
    return false;

  const DataLayout &DL = CB.getDataLayout();
  unsigned IndexWidth = DL.getIndexTypeSizeInBits(GEP->getType());
  APInt Offset(IndexWidth, 0);
  auto *GV = dyn_cast<GlobalVariable>(
      GEP->getPointerOperand()->stripAndAccumulateConstantOffsets(
          DL, Offset, /*AllowNonInbounds=*/true));
  // A thread-local or explicitly placed constant may be writable.
  if (!GV || !GV->isConstant() || !GV->hasDefinitiveInitializer() ||
      GV->isDeclarationForLinker() || GV->isThreadLocal() || GV->hasSection() ||
      !holdsOnlyFunctions(GV->getInitializer(), getKCFIType(CB)))
    return false;

  SmallMapVector<Value *, APInt, 4> VariableOffsets;
  APInt ConstantOffset(IndexWidth, 0);
  if (!GEP->collectOffset(DL, IndexWidth, VariableOffsets, ConstantOffset) ||
      VariableOffsets.size() != 1)
    return false;
  Offset += ConstantOffset;
  const auto &[Index, Scale] = VariableOffsets.front();

  // Every offset must select a whole word inside the table.
  uint64_t WordSize = DL.getTypeStoreSize(Load->getType());
  uint64_t TableSize = DL.getTypeAllocSize(GV->getValueType());
  if (TableSize < WordSize || Scale.urem(WordSize) || Offset.urem(WordSize))
    return false;

  // The proof holds for values in registers. Requiring them to be computed
  // after the last call keeps them out of a callee's frame; the backend checks
  // that register allocation kept them out of this function's.
  const Instruction *LastCall = CB.getPrevNode();
  while (LastCall && !mayCall(*LastCall))
    LastCall = LastCall->getPrevNode();
  if (LastCall && GEP->comesBefore(LastCall))
    return false;

  ConstantRange Offsets = computeIndexRange(Index, BB, LastCall)
                              .sextOrTrunc(IndexWidth)
                              .multiply(ConstantRange(Scale))
                              .add(ConstantRange(Offset));
  return ConstantRange(APInt(IndexWidth, 0),
                       APInt(IndexWidth, TableSize - WordSize + 1))
      .contains(Offsets);
}

bool CFGuardImpl::runOnFunction(Function &F) {
  // Skip modules for which CFGuard checks have been disabled.
  bool CheckAll = CFGuardModuleFlag == ControlFlowGuardMode::Enabled;
  if (!CheckAll && !UseKCFIThunks && !UseKCFICheckThunks)
    return false;

  SmallVector<CallBase *, 8> IndirectCalls;
  // The loads of proven targets, and those of targets some other call of the
  // load does not prove.
  SmallPtrSet<LoadInst *, 4> ProvenLoads;
  SmallPtrSet<LoadInst *, 4> UnprovenLoads;
  SmallVector<IntrinsicInst *, 2> KCFIChecks;
  MemberTests.clear();
  FusedMemberTests.clear();
  DT.reset();

  // Iterate over the instructions to find all indirect call/invoke/callbr
  // instructions. Make a separate list of pointers to indirect
  // call/invoke/callbr instructions because the original instructions will be
  // deleted as the checks are added.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // Only a module with KCFI thunks has tests of membership tags, which
      // LowerTypeTests makes for such modules alone.
      if (auto *II = dyn_cast<IntrinsicInst>(&I); II && UseKCFICheckThunks) {
        if (II->getIntrinsicID() == Intrinsic::kcfi_member_test) {
          MemberTests.push_back(II);
          continue;
        }
        if (II->getIntrinsicID() == Intrinsic::kcfi_check &&
            isKCFICheckThunkOffset(
                cast<ConstantInt>(II->getArgOperand(2))->getZExtValue())) {
          KCFIChecks.push_back(II);
          continue;
        }
      }
      auto *CB = dyn_cast<CallBase>(&I);
      if (CB && CB->isIndirectCall() && !CB->hasFnAttr("guard_nocf") &&
          (CheckAll ||
           (UseKCFIThunks && CB->getOperandBundle(LLVMContext::OB_kcfi)))) {
        if (auto *Load = MarkProvenCalls
                             ? dyn_cast<LoadInst>(CB->getCalledOperand())
                             : nullptr) {
          if (hasProvenTarget(*CB)) {
            ProvenLoads.insert(Load);
            CFGuardProvenCounter++;
          } else {
            UnprovenLoads.insert(Load);
          }
        }
        IndirectCalls.push_back(CB);
        CFGuardCounter++;
      }
    }
  }

  // If no checks are needed, return early.
  if (IndirectCalls.empty() && KCFIChecks.empty() && MemberTests.empty())
    return false;

  for (IntrinsicInst *II : KCFIChecks)
    insertKCFICheckThunk(II);

  // A proven call keeps its check, which only the backend can drop, once it
  // knows that the target stayed in its register from the load to the call.
  // The mark is on the load, so each call of the loaded value must prove it.
  for (LoadInst *Load : ProvenLoads)
    if (!UnprovenLoads.contains(Load))
      Load->setMetadata("cfguard_proven", MDNode::get(F.getContext(), {}));

  // For each indirect call/invoke, add the appropriate dispatch or check.
  for (CallBase *CB : IndirectCalls) {
    if (GuardMechanism == Mechanism::Dispatch && canUseDispatch(*CB))
      insertCFGuardDispatch(CB);
    else
      insertCFGuardCheck(CB);
  }

  lowerKCFIMemberTests();
  return true;
}

void CFGuardImpl::lowerKCFIMemberTests() {
  for (IntrinsicInst *II : MemberTests) {
    // A test that guards no call through a member thunk checks its target in
    // place. Without a KCFI type to fall back on, a miss fails fast, and a
    // test of no tags is false.
    //
    // A test of a function the optimizer has made known, whose call became
    // direct and lost its KCFI check, is decided here, as LowerTypeTests
    // decides a test of a known function: a function defined in this module
    // passes if it carries one of the tags. One defined elsewhere, such as an
    // import, carries no tag, and a call through KCFI would have taken it at
    // KCFI's strength, so it passes; a mismatch of KCFI types was reported
    // when the call became direct.
    bool Result = true;
    if (!FusedMemberTests.contains(II)) {
      MDNode *Tags = getKCFIMemberTags(II);
      auto *F = dyn_cast<Function>(II->getArgOperand(0)->stripPointerCasts());
      if (F) {
        if (!F->isDeclarationForLinker()) {
          const MDNode *Tag = F->getMetadata("kcfi_member_tag");
          Result = Tag && is_contained(Tags->operands(), Tag->getOperand(0));
        }
      } else if (Tags->getNumOperands() == 0) {
        Result = false;
      } else {
        Function *Thunk = declareKCFIMemberThunk(
            *II->getModule(), KCFIMemberCheckThunkPrefix, /*Type=*/0, Tags);
        IRBuilder<> B(II);
        CallInst *Check =
            B.CreateCall(GuardFnType, Thunk, {II->getArgOperand(0)});
        Check->setCallingConv(CallingConv::CFGuard_Check);
      }
    }
    II->replaceAllUsesWith(ConstantInt::getBool(II->getContext(), Result));
    II->eraseFromParent();
  }
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
