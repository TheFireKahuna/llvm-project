//===-- ModuleUtils.cpp - Functions to manipulate Modules -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This family of functions perform manipulations on Modules.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace llvm;

#define DEBUG_TYPE "moduleutils"

static void appendToGlobalArray(StringRef ArrayName, Module &M, Function *F,
                                int Priority, Constant *Data) {
  IRBuilder<> IRB(M);

  // Get the current set of static global constructors and add the new ctor
  // to the list.
  SmallVector<Constant *, 16> CurrentCtors;
  StructType *EltTy;
  if (GlobalVariable *GVCtor = M.getNamedGlobal(ArrayName)) {
    EltTy = cast<StructType>(GVCtor->getValueType()->getArrayElementType());
    if (Constant *Init = GVCtor->getInitializer()) {
      unsigned n = Init->getNumOperands();
      CurrentCtors.reserve(n + 1);
      for (unsigned i = 0; i != n; ++i)
        CurrentCtors.push_back(cast<Constant>(Init->getOperand(i)));
    }
    GVCtor->eraseFromParent();
  } else {
    EltTy = StructType::get(
        IRB.getInt32Ty(),
        PointerType::get(M.getContext(), F->getAddressSpace()), IRB.getPtrTy());
  }

  // Build a 3 field global_ctor entry.  We don't take a comdat key.
  Constant *CSVals[3];
  CSVals[0] = IRB.getInt32(Priority);
  CSVals[1] = F;
  CSVals[2] = Data ? ConstantExpr::getPointerCast(Data, IRB.getPtrTy())
                   : Constant::getNullValue(IRB.getPtrTy());
  Constant *RuntimeCtorInit =
      ConstantStruct::get(EltTy, ArrayRef(CSVals, EltTy->getNumElements()));

  CurrentCtors.push_back(RuntimeCtorInit);

  // Create a new initializer.
  ArrayType *AT = ArrayType::get(EltTy, CurrentCtors.size());
  Constant *NewInit = ConstantArray::get(AT, CurrentCtors);

  // Create the new global variable and replace all uses of
  // the old global variable with the new one.
  (void)new GlobalVariable(M, NewInit->getType(), false,
                           GlobalValue::AppendingLinkage, NewInit, ArrayName);
}

void llvm::appendToGlobalCtors(Module &M, Function *F, int Priority, Constant *Data) {
  appendToGlobalArray("llvm.global_ctors", M, F, Priority, Data);
}

void llvm::appendToGlobalDtors(Module &M, Function *F, int Priority, Constant *Data) {
  appendToGlobalArray("llvm.global_dtors", M, F, Priority, Data);
}

static void transformGlobalArray(StringRef ArrayName, Module &M,
                                 const GlobalCtorTransformFn &Fn) {
  GlobalVariable *GVCtor = M.getNamedGlobal(ArrayName);
  if (!GVCtor)
    return;

  IRBuilder<> IRB(M);
  SmallVector<Constant *, 16> CurrentCtors;
  bool Changed = false;
  StructType *EltTy =
      cast<StructType>(GVCtor->getValueType()->getArrayElementType());
  if (Constant *Init = GVCtor->getInitializer()) {
    CurrentCtors.reserve(Init->getNumOperands());
    for (Value *OP : Init->operands()) {
      Constant *C = cast<Constant>(OP);
      Constant *NewC = Fn(C);
      Changed |= (!NewC || NewC != C);
      if (NewC)
        CurrentCtors.push_back(NewC);
    }
  }
  if (!Changed)
    return;

  GVCtor->eraseFromParent();

  // Create a new initializer.
  ArrayType *AT = ArrayType::get(EltTy, CurrentCtors.size());
  Constant *NewInit = ConstantArray::get(AT, CurrentCtors);

  // Create the new global variable and replace all uses of
  // the old global variable with the new one.
  (void)new GlobalVariable(M, NewInit->getType(), false,
                           GlobalValue::AppendingLinkage, NewInit, ArrayName);
}

void llvm::transformGlobalCtors(Module &M, const GlobalCtorTransformFn &Fn) {
  transformGlobalArray("llvm.global_ctors", M, Fn);
}

void llvm::transformGlobalDtors(Module &M, const GlobalCtorTransformFn &Fn) {
  transformGlobalArray("llvm.global_dtors", M, Fn);
}

static void collectUsedGlobals(GlobalVariable *GV,
                               SmallSetVector<Constant *, 16> &Init) {
  if (!GV || !GV->hasInitializer())
    return;

  auto *CA = cast<ConstantArray>(GV->getInitializer());
  for (Use &Op : CA->operands())
    Init.insert(cast<Constant>(Op));
}

static void appendToUsedList(Module &M, StringRef Name, ArrayRef<GlobalValue *> Values) {
  GlobalVariable *GV = M.getGlobalVariable(Name);

  SmallSetVector<Constant *, 16> Init;
  collectUsedGlobals(GV, Init);
  if (GV)
    GV->eraseFromParent();

  Type *ArrayEltTy = llvm::PointerType::getUnqual(M.getContext());
  for (auto *V : Values)
    Init.insert(ConstantExpr::getPointerBitCastOrAddrSpaceCast(V, ArrayEltTy));

  if (Init.empty())
    return;

  ArrayType *ATy = ArrayType::get(ArrayEltTy, Init.size());
  GV = new llvm::GlobalVariable(M, ATy, false, GlobalValue::AppendingLinkage,
                                ConstantArray::get(ATy, Init.getArrayRef()),
                                Name);
  GV->setSection("llvm.metadata");
}

void llvm::appendToUsed(Module &M, ArrayRef<GlobalValue *> Values) {
  appendToUsedList(M, "llvm.used", Values);
}

void llvm::appendToCompilerUsed(Module &M, ArrayRef<GlobalValue *> Values) {
  appendToUsedList(M, "llvm.compiler.used", Values);
}

static void removeFromUsedList(Module &M, StringRef Name,
                               function_ref<bool(Constant *)> ShouldRemove) {
  GlobalVariable *GV = M.getNamedGlobal(Name);
  if (!GV)
    return;

  SmallSetVector<Constant *, 16> Init;
  collectUsedGlobals(GV, Init);

  Type *ArrayEltTy = cast<ArrayType>(GV->getValueType())->getElementType();

  SmallVector<Constant *, 16> NewInit;
  for (Constant *MaybeRemoved : Init) {
    if (!ShouldRemove(MaybeRemoved->stripPointerCasts()))
      NewInit.push_back(MaybeRemoved);
  }

  if (!NewInit.empty()) {
    ArrayType *ATy = ArrayType::get(ArrayEltTy, NewInit.size());
    GlobalVariable *NewGV =
        new GlobalVariable(M, ATy, false, GlobalValue::AppendingLinkage,
                           ConstantArray::get(ATy, NewInit), "", GV,
                           GV->getThreadLocalMode(), GV->getAddressSpace());
    NewGV->setSection(GV->getSection());
    NewGV->takeName(GV);
  }

  GV->eraseFromParent();
}

void llvm::removeFromUsedLists(Module &M,
                               function_ref<bool(Constant *)> ShouldRemove) {
  removeFromUsedList(M, "llvm.used", ShouldRemove);
  removeFromUsedList(M, "llvm.compiler.used", ShouldRemove);
}

void llvm::setKCFIType(Module &M, Function &F, StringRef MangledType) {
  if (!M.getModuleFlag("kcfi"))
    return;
  // Matches CodeGenModule::CreateKCFITypeId in Clang.
  LLVMContext &Ctx = M.getContext();
  MDBuilder MDB(Ctx);
  std::string Type = MangledType.str();
  if (M.getModuleFlag("cfi-normalize-integers"))
    Type += ".normalized";
  if (M.getModuleFlag("cfi-generalize-pointers"))
    Type += ".generalized";

  // Determine which hash algorithm to use
  auto *MD = dyn_cast_or_null<MDString>(M.getModuleFlag("kcfi-hash"));
  KCFIHashAlgorithm Algorithm =
      parseKCFIHashAlgorithm(MD ? MD->getString() : "");

  uint32_t TypeId = getKCFITypeID(Type, Algorithm);
  if (M.getModuleFlag("function-type-prefix"))
    TypeId = getTypePrefixTypeId(TypeId);
  F.setMetadata(LLVMContext::MD_kcfi_type,
                MDNode::get(Ctx, MDB.createConstant(ConstantInt::get(
                                     Type::getInt32Ty(Ctx), TypeId))));
  // If the module was compiled with -fpatchable-function-entry, ensure
  // we use the same patchable-function-prefix.
  if (auto *MD = mdconst::extract_or_null<ConstantInt>(
          M.getModuleFlag("kcfi-offset"))) {
    if (unsigned Offset = MD->getZExtValue())
      F.addFnAttr("patchable-function-prefix", std::to_string(Offset));
  }
}

FunctionCallee llvm::declareSanitizerInitFunction(Module &M, StringRef InitName,
                                                  ArrayRef<Type *> InitArgTypes,
                                                  bool Weak) {
  assert(!InitName.empty() && "Expected init function name");
  auto *VoidTy = Type::getVoidTy(M.getContext());
  auto *FnTy = FunctionType::get(VoidTy, InitArgTypes, false);
  auto FnCallee = M.getOrInsertFunction(InitName, FnTy);
  auto *Fn = cast<Function>(FnCallee.getCallee());
  if (Weak && Fn->isDeclaration())
    Fn->setLinkage(Function::ExternalWeakLinkage);
  return FnCallee;
}

Function *llvm::createSanitizerCtor(Module &M, StringRef CtorName) {
  Function *Ctor = Function::createWithDefaultAttr(
      FunctionType::get(Type::getVoidTy(M.getContext()), false),
      GlobalValue::InternalLinkage, M.getDataLayout().getProgramAddressSpace(),
      CtorName, &M);
  Ctor->addFnAttr(Attribute::NoUnwind);
  setKCFIType(M, *Ctor, "_ZTSFvvE"); // void (*)(void)
  BasicBlock *CtorBB = BasicBlock::Create(M.getContext(), "", Ctor);
  ReturnInst::Create(M.getContext(), CtorBB);
  // Ensure Ctor cannot be discarded, even if in a comdat.
  appendToUsed(M, {Ctor});
  return Ctor;
}

std::pair<Function *, FunctionCallee> llvm::createSanitizerCtorAndInitFunctions(
    Module &M, StringRef CtorName, StringRef InitName,
    ArrayRef<Type *> InitArgTypes, ArrayRef<Value *> InitArgs,
    StringRef VersionCheckName, bool Weak) {
  assert(!InitName.empty() && "Expected init function name");
  assert(InitArgs.size() == InitArgTypes.size() &&
         "Sanitizer's init function expects different number of arguments");
  FunctionCallee InitFunction =
      declareSanitizerInitFunction(M, InitName, InitArgTypes, Weak);
  Function *Ctor = createSanitizerCtor(M, CtorName);
  IRBuilder<> IRB(M);

  BasicBlock *RetBB = &Ctor->getEntryBlock();
  if (Weak) {
    RetBB->setName("ret");
    auto *EntryBB = BasicBlock::Create(M.getContext(), "entry", Ctor, RetBB);
    auto *CallInitBB =
        BasicBlock::Create(M.getContext(), "callfunc", Ctor, RetBB);
    auto *InitFn = cast<Function>(InitFunction.getCallee());
    auto *InitFnPtr =
        PointerType::get(M.getContext(), InitFn->getAddressSpace());
    IRB.SetInsertPoint(EntryBB);
    Value *InitNotNull =
        IRB.CreateICmpNE(InitFn, ConstantPointerNull::get(InitFnPtr));
    IRB.CreateCondBr(InitNotNull, CallInitBB, RetBB);
    IRB.SetInsertPoint(CallInitBB);
  } else {
    IRB.SetInsertPoint(RetBB->getTerminator());
  }

  IRB.CreateCall(InitFunction, InitArgs);
  if (!VersionCheckName.empty()) {
    FunctionCallee VersionCheckFunction = M.getOrInsertFunction(
        VersionCheckName, FunctionType::get(IRB.getVoidTy(), {}, false),
        AttributeList());
    IRB.CreateCall(VersionCheckFunction, {});
  }

  if (Weak)
    IRB.CreateBr(RetBB);

  return std::make_pair(Ctor, InitFunction);
}

std::pair<Function *, FunctionCallee>
llvm::getOrCreateSanitizerCtorAndInitFunctions(
    Module &M, StringRef CtorName, StringRef InitName,
    ArrayRef<Type *> InitArgTypes, ArrayRef<Value *> InitArgs,
    function_ref<void(Function *, FunctionCallee)> FunctionsCreatedCallback,
    StringRef VersionCheckName, bool Weak) {
  assert(!CtorName.empty() && "Expected ctor function name");

  if (Function *Ctor = M.getFunction(CtorName))
    // FIXME: Sink this logic into the module, similar to the handling of
    // globals. This will make moving to a concurrent model much easier.
    if (Ctor->arg_empty() ||
        Ctor->getReturnType() == Type::getVoidTy(M.getContext()))
      return {Ctor,
              declareSanitizerInitFunction(M, InitName, InitArgTypes, Weak)};

  Function *Ctor;
  FunctionCallee InitFunction;
  std::tie(Ctor, InitFunction) = llvm::createSanitizerCtorAndInitFunctions(
      M, CtorName, InitName, InitArgTypes, InitArgs, VersionCheckName, Weak);
  FunctionsCreatedCallback(Ctor, InitFunction);
  return std::make_pair(Ctor, InitFunction);
}

void llvm::filterDeadComdatFunctions(
    SmallVectorImpl<Function *> &DeadComdatFunctions) {
  SmallPtrSet<Function *, 32> MaybeDeadFunctions;
  SmallPtrSet<Comdat *, 32> MaybeDeadComdats;
  for (Function *F : DeadComdatFunctions) {
    MaybeDeadFunctions.insert(F);
    if (Comdat *C = F->getComdat())
      MaybeDeadComdats.insert(C);
  }

  // Find comdats for which all users are dead now.
  SmallPtrSet<Comdat *, 32> DeadComdats;
  for (Comdat *C : MaybeDeadComdats) {
    auto IsUserDead = [&](GlobalObject *GO) {
      auto *F = dyn_cast<Function>(GO);
      return F && MaybeDeadFunctions.contains(F);
    };
    if (all_of(C->getUsers(), IsUserDead))
      DeadComdats.insert(C);
  }

  // Only keep functions which have no comdat or a dead comdat.
  erase_if(DeadComdatFunctions, [&](Function *F) {
    Comdat *C = F->getComdat();
    return C && !DeadComdats.contains(C);
  });
}

std::string llvm::getUniqueModuleId(Module *M) {
  MD5 Md5;

  auto *UniqueSourceFileIdentifier = dyn_cast_or_null<MDNode>(
      M->getModuleFlag("Unique Source File Identifier"));
  if (UniqueSourceFileIdentifier) {
    Md5.update(
        cast<MDString>(UniqueSourceFileIdentifier->getOperand(0))->getString());
  } else {
    bool ExportsSymbols = false;
    for (auto &GV : M->global_values()) {
      if (GV.isDeclaration() || GV.getName().starts_with("llvm.") ||
          !GV.hasExternalLinkage() || GV.hasComdat())
        continue;
      ExportsSymbols = true;
      Md5.update(GV.getName());
      Md5.update(ArrayRef<uint8_t>{0});
    }

    if (!ExportsSymbols)
      return "";
  }

  MD5::MD5Result R;
  Md5.final(R);

  SmallString<32> Str;
  MD5::stringifyResult(R, Str);
  return ("." + Str).str();
}

GlobalVariable *llvm::embedBufferInModule(Module &M, MemoryBufferRef Buf,
                                          StringRef SectionName,
                                          Align Alignment,
                                          bool SectionExclude) {
  // Embed the memory buffer into the module.
  Constant *ModuleConstant = ConstantDataArray::get(
      M.getContext(), ArrayRef(Buf.getBufferStart(), Buf.getBufferSize()));
  GlobalVariable *GV = new GlobalVariable(
      M, ModuleConstant->getType(), true, GlobalValue::PrivateLinkage,
      ModuleConstant, "llvm.embedded.object");
  GV->setSection(SectionName);
  GV->setAlignment(Alignment);

  LLVMContext &Ctx = M.getContext();
  NamedMDNode *MD = M.getOrInsertNamedMetadata("llvm.embedded.objects");
  Metadata *MDVals[] = {ConstantAsMetadata::get(GV),
                        MDString::get(Ctx, SectionName)};
  MD->addOperand(llvm::MDNode::get(Ctx, MDVals));

  if (SectionExclude)
    GV->setMetadata(LLVMContext::MD_exclude, llvm::MDNode::get(Ctx, {}));
  else
    GV->setMetadata(LLVMContext::MD_metadata_section_kind,
                    llvm::MDNode::get(Ctx, {}));

  appendToCompilerUsed(M, GV);
  return GV;
}

bool llvm::lowerGlobalIFuncUsersAsGlobalCtor(
    Module &M, ArrayRef<GlobalIFunc *> FilteredIFuncsToLower) {
  SmallVector<GlobalIFunc *, 32> AllIFuncs;
  ArrayRef<GlobalIFunc *> IFuncsToLower = FilteredIFuncsToLower;
  if (FilteredIFuncsToLower.empty()) { // Default to lowering all ifuncs
    for (GlobalIFunc &GI : M.ifuncs())
      AllIFuncs.push_back(&GI);
    IFuncsToLower = AllIFuncs;
  }

  bool UnhandledUsers = false;
  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();

  PointerType *TableEntryTy =
      PointerType::get(Ctx, DL.getProgramAddressSpace());

  ArrayType *FuncPtrTableTy =
      ArrayType::get(TableEntryTy, IFuncsToLower.size());

  Align PtrAlign = DL.getABITypeAlign(TableEntryTy);

  // Create a global table of function pointers we'll initialize in a global
  // constructor.
  auto *FuncPtrTable = new GlobalVariable(
      M, FuncPtrTableTy, false, GlobalValue::InternalLinkage,
      PoisonValue::get(FuncPtrTableTy), "", nullptr,
      GlobalVariable::NotThreadLocal, DL.getDefaultGlobalsAddressSpace());
  FuncPtrTable->setAlignment(PtrAlign);

  // Create a function to initialize the function pointer table.
  Function *NewCtor = Function::Create(
      FunctionType::get(Type::getVoidTy(Ctx), false), Function::InternalLinkage,
      DL.getProgramAddressSpace(), "", &M);

  BasicBlock *BB = BasicBlock::Create(Ctx, "", NewCtor);
  IRBuilder<> InitBuilder(BB);

  size_t TableIndex = 0;
  for (GlobalIFunc *GI : IFuncsToLower) {
    Function *ResolvedFunction = GI->getResolverFunction();

    // We don't know what to pass to a resolver function taking arguments
    //
    // FIXME: Is this even valid? clang and gcc don't complain but this
    // probably should be invalid IR. We could just pass through undef.
    if (!std::empty(ResolvedFunction->getFunctionType()->params())) {
      LLVM_DEBUG(dbgs() << "Not lowering ifunc resolver function "
                        << ResolvedFunction->getName() << " with parameters\n");
      UnhandledUsers = true;
      continue;
    }

    // Initialize the function pointer table.
    CallInst *ResolvedFunc = InitBuilder.CreateCall(ResolvedFunction);
    Value *Casted = InitBuilder.CreatePointerCast(ResolvedFunc, TableEntryTy);
    Constant *GEP = cast<Constant>(InitBuilder.CreateConstInBoundsGEP2_32(
        FuncPtrTableTy, FuncPtrTable, 0, TableIndex++));
    InitBuilder.CreateAlignedStore(Casted, GEP, PtrAlign);

    // Update all users to load a pointer from the global table.
    for (User *User : make_early_inc_range(GI->users())) {
      Instruction *UserInst = dyn_cast<Instruction>(User);
      if (!UserInst) {
        // TODO: Should handle constantexpr casts in user instructions. Probably
        // can't do much about constant initializers.
        UnhandledUsers = true;
        continue;
      }

      IRBuilder<> UseBuilder(UserInst);
      LoadInst *ResolvedTarget =
          UseBuilder.CreateAlignedLoad(TableEntryTy, GEP, PtrAlign);
      Value *ResolvedCast =
          UseBuilder.CreatePointerCast(ResolvedTarget, GI->getType());
      UserInst->replaceUsesOfWith(GI, ResolvedCast);
    }

    // If we handled all users, erase the ifunc.
    if (GI->use_empty())
      GI->eraseFromParent();
  }

  InitBuilder.CreateRetVoid();

  PointerType *ConstantDataTy = PointerType::get(Ctx, 0);

  // TODO: Is this the right priority? Probably should be before any other
  // constructors?
  const int Priority = 10;
  appendToGlobalCtors(M, NewCtor, Priority,
                      ConstantPointerNull::get(ConstantDataTy));
  return UnhandledUsers;
}

// Follows the use-def chain of \p V backwards until it finds a Function,
// in which case it collects in \p Versions. Return true on successful
// use-def chain traversal, false otherwise.
static bool
collectVersions(Value *V, SmallVectorImpl<Function *> &Versions,
                function_ref<TargetTransformInfo &(Function &)> GetTTI) {
  if (auto *F = dyn_cast<Function>(V)) {
    if (!GetTTI(*F).isMultiversionedFunction(*F))
      return false;
    Versions.push_back(F);
  } else if (auto *Sel = dyn_cast<SelectInst>(V)) {
    if (!collectVersions(Sel->getTrueValue(), Versions, GetTTI))
      return false;
    if (!collectVersions(Sel->getFalseValue(), Versions, GetTTI))
      return false;
  } else if (auto *Phi = dyn_cast<PHINode>(V)) {
    for (unsigned I = 0, E = Phi->getNumIncomingValues(); I != E; ++I)
      if (!collectVersions(Phi->getIncomingValue(I), Versions, GetTTI))
        return false;
  } else {
    // Unknown instruction type. Bail.
    return false;
  }
  return true;
}

bool llvm::collectFunctionVersions(
    Function &Resolver, SmallVectorImpl<Function *> &Versions,
    function_ref<TargetTransformInfo &(Function &)> GetTTI) {
  for (BasicBlock &BB : Resolver)
    if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator()))
      if (!collectVersions(Ret->getReturnValue(), Versions, GetTTI))
        return false;
  return true;
}

void llvm::resolveThreadLocalGuards(Module &M) {
  // What a thread-local variable's record names: its guard, or for a guard's
  // declaration the variable.
  auto Named = [](const GlobalVariable *GV) {
    const MDNode *Record = GV->getMetadata("tls_record");
    return mdconst::dyn_extract_or_null<GlobalVariable>(
        Record ? Record->getOperand(0).get() : nullptr);
  };
  for (GlobalVariable &GV : make_early_inc_range(M.globals()))
    if (GlobalVariable *Var = GV.isDeclaration() ? Named(&GV) : nullptr;
        Var && !Var->isDeclarationForLinker())
      if (GlobalVariable *Guard = Named(Var)) {
        GV.replaceAllUsesWith(Guard);
        GV.eraseFromParent();
      }
}

bool llvm::collectFunctionOverride(
    const GlobalIFunc &GI,
    function_ref<TargetTransformInfo &(Function &)> GetTTI,
    function_ref<StringRef(const Function &, SmallVectorImpl<uint32_t> &)>
        GetCapabilities,
    FunctionOverride &FO, raw_ostream *Why) {
  auto Fail = [&](const Twine &Reason) {
    if (Why)
      *Why << Reason;
    return false;
  };
  auto *Resolver = const_cast<Function *>(GI.getResolverFunction());
  SmallVector<Function *> Versions;
  if (GI.isInterposable())
    return Fail("it may be replaced at link time");
  if (!Resolver || Resolver->isDeclaration() || Resolver->isInterposable())
    return Fail("its resolver is not defined in this module for good");
  if (!collectFunctionVersions(*Resolver, Versions, GetTTI))
    return Fail("its resolver returns other than versions of a "
                "multiversioned function");
  SmallVector<std::pair<APInt, Function *>> Ranked;
  for (Function *V : Versions)
    Ranked.emplace_back(GetTTI(*V).getPriorityMask(*V), V);
  sort(Ranked, [](const auto &L, const auto &R) {
    return L.first != R.first ? L.first.ugt(R.first)
                              : L.second->getName() < R.second->getName();
  });
  Ranked.erase(unique(Ranked), Ranked.end());

  // The resolver is read as a chain: the first version in priority order
  // whose features the processor has, ending at the one that needs none.
  FO = FunctionOverride();
  for (size_t I = 0, E = Ranked.size(); I != E; ++I) {
    Function *V = Ranked[I].second;
    if (FO.Default)
      return Fail("version '" + V->getName() + "' ranks below '" +
                  FO.Default->getName() + "', which needs no feature");
    if (I && Ranked[I - 1].first == Ranked[I].first)
      return Fail("versions '" + Ranked[I - 1].second->getName() + "' and '" +
                  V->getName() + "' have one priority");
    SmallVector<uint32_t, 2> Capabilities;
    if (StringRef Feature = GetCapabilities(*V, Capabilities); !Feature.empty())
      return Fail("version '" + V->getName() + "' needs '" + Feature +
                  "', which Windows selects no body by");
    if (Capabilities.empty())
      FO.Default = V;
    else
      FO.Candidates.emplace_back(V, std::move(Capabilities));
  }
  return FO.Default || Fail("no version runs on every processor");
}

bool llvm::callsThreadPointerReader(const CallBase &CB) {
  const Function *Callee = CB.getCalledFunction();
  if (!Callee || !Callee->hasFnAttribute("thread-pointer-argument") ||
      Callee->hasFnAttribute("thread-pointer-unused"))
    return false;
  return !Callee->isDeclaration() ||
         (Callee->getFnAttribute("thread-pointer-argument")
                  .getValueAsString() == "read" &&
          !CB.getMemoryEffects()
               .getWithoutLoc(IRMemLocation::ArgMem)
               .getWithoutLoc(IRMemLocation::InaccessibleMem)
               .doesNotAccessMemory());
}

/// The function whose marks describe what \p CB calls by name: the callee, or
/// an ifunc's resolver. A function's address is its plain entry, so a call
/// through a pointer has none.
static const Function *getCalledByName(const CallBase &CB) {
  const Value *Callee = CB.getCalledOperand()->stripPointerCastsAndAliases();
  if (const auto *GI = dyn_cast<GlobalIFunc>(Callee))
    return GI->getResolverFunction();
  return dyn_cast<Function>(Callee);
}

bool llvm::passesThreadPointer(const CallBase &CB) {
  if (CB.isInlineAsm() || CB.getOperandBundle(LLVMContext::OB_threadpointer) ||
      (CB.isMustTailCall() &&
       !CB.getFunction()->hasFnAttribute("thread-pointer-argument")))
    return false;
  const Function *Callee = getCalledByName(CB);
  if (!Callee)
    return CB.isIndirectCall() && CB.getOperandBundle(LLVMContext::OB_kcfi) &&
           CB.getModule()->getControlFlowGuardMode() !=
               ControlFlowGuardMode::Enabled;
  return !Callee->isIntrinsic() &&
         Callee->hasFnAttribute("thread-pointer-argument") &&
         !Callee->hasFnAttribute("thread-pointer-unused") &&
         !(Callee->isDeclaration() &&
           CB.getMemoryEffects()
               .getWithoutLoc(IRMemLocation::ArgMem)
               .getWithoutLoc(IRMemLocation::InaccessibleMem)
               .doesNotAccessMemory());
}

bool llvm::needsThreadPointer(const Instruction &I) {
  // A variadic ifunc has no plain entry.
  const auto *CB = dyn_cast<CallBase>(&I);
  return CB &&
         (CB->getIntrinsicID() == Intrinsic::thread_pointer ||
          CB->getIntrinsicID() == Intrinsic::threadlocal_address ||
          (isa<GlobalIFunc>(
               CB->getCalledOperand()->stripPointerCastsAndAliases()) &&
           CB->getFunctionType()->isVarArg() && passesThreadPointer(*CB)));
}

bool llvm::holdsThreadPointer(const Function &F) {
  if (F.hasFnAttribute("thread-pointer-argument"))
    return !F.hasFnAttribute("thread-pointer-unused");
  if (F.hasFnAttribute("thread-pointer-probe") ||
      F.hasFnAttribute("thread-pointer-refusal") ||
      F.hasFnAttribute("no-thread-pointer"))
    return true;
  return any_of(instructions(F), [](const Instruction &I) {
    const auto *CB = dyn_cast<CallBase>(&I);
    return needsThreadPointer(I) || (CB && callsThreadPointerReader(*CB));
  });
}

void llvm::takeThreadPointerPlainEntries(Module &M) {
  // A resolver returns the version its ifunc hands its thread pointer, and
  // what the compiler or the linker keeps names the function itself, as does
  // a filter's recovery of the frame of the body it belongs to. A filter
  // that holds none is its own plain entry, which a catchpad names as the
  // function it is.
  SmallPtrSet<const Function *, 4> Resolvers;
  for (GlobalIFunc &GI : M.ifuncs())
    Resolvers.insert(GI.getResolverFunction());
  auto Keeps = [&](const Use &U) {
    const User *Owner = U.getUser();
    if (const auto *CB = dyn_cast<CallBase>(Owner);
        CB && (CB->isCallee(&U) ||
               CB->getIntrinsicID() == Intrinsic::localrecover ||
               CB->getIntrinsicID() == Intrinsic::eh_recoverfp))
      return true;
    if (isa<CatchPadInst>(Owner))
      return cast<Function>(U.get())->hasFnAttribute("thread-pointer-unused");
    if (const auto *I = dyn_cast<Instruction>(Owner))
      return Resolvers.contains(I->getFunction());
    return isa<GlobalAlias, GlobalIFunc>(Owner) ||
           (isa<ConstantArray>(Owner) &&
            any_of(Owner->users(), [](const User *Array) {
              const auto *GV = dyn_cast<GlobalVariable>(Array);
              return GV && (GV->getName() == "llvm.used" ||
                            GV->getName() == "llvm.compiler.used");
            }));
  };
  for (Function &F : make_early_inc_range(M))
    if (F.hasFnAttribute("thread-pointer-argument") &&
        !F.getName().starts_with("\01") &&
        any_of(F.uses(), [&](const Use &U) { return !Keeps(U); }))
      F.replaceUsesWithIf(getThreadPointerPlainEntry(F),
                          [&](Use &U) { return !Keeps(U); });
}

GlobalValue *llvm::getThreadPointerPlainEntry(GlobalValue &GV) {
  Module &M = *GV.getParent();
  std::string Name = ("\01" + GV.getName()).str();
  if (GlobalValue *Plain = M.getNamedValue(Name))
    return Plain;
  // A weak reference's plain entry is weak too.
  auto &F = cast<Function>(GV);
  Function *Plain = Function::Create(
      F.getFunctionType(),
      F.hasExternalWeakLinkage() ? GlobalValue::ExternalWeakLinkage
                                 : GlobalValue::ExternalLinkage,
      F.getAddressSpace(), Name, &M);
  Plain->copyAttributesFrom(&F);
  Plain->removeFnAttr("thread-pointer-argument");
  Plain->removeFnAttr("thread-pointer-unused");
  return Plain;
}

void llvm::createThreadPointerPlainEntries(Module &M) {
  // Every function passed its thread pointer that code passing none may call
  // has one: any but a local one that only callers holding it call.
  SmallVector<std::pair<Function *, GlobalValue *>, 16> Bodies;
  for (Function &F : M)
    if (!F.isDeclaration() && !F.hasAvailableExternallyLinkage() &&
        F.hasFnAttribute("thread-pointer-argument") &&
        !F.getName().starts_with("\01") &&
        !M.getNamedValue(("\01" + F.getName()).str()) &&
        (!F.hasLocalLinkage() || any_of(F.uses(), [](const Use &U) {
          const auto *CB = dyn_cast<CallBase>(U.getUser());
          return !CB || !CB->isCallee(&U) ||
                 !holdsThreadPointer(*CB->getFunction()) ||
                 !passesThreadPointer(*CB);
        })))
      Bodies.push_back({&F, &F});
  // A weak external passed its thread pointer defines both entries, each
  // falling back to its alternate's entry of the same kind.
  DenseMap<GlobalValue *, GlobalValue *> Plain;
  for (Function &F : make_early_inc_range(M))
    if (Attribute Alt = F.getFnAttribute("weak-alternate");
        Alt.isValid() && F.hasFnAttribute("thread-pointer-argument")) {
      Plain[&F] = getThreadPointerPlainEntry(F);
      F.addFnAttr("weak-alternate", (Alt.getValueAsString() + "$tp").str());
    }
  // A resolver's marks are its ifunc's, which its plain entry calls.
  for (GlobalIFunc &GI : M.ifuncs())
    for (auto &[F, Callee] : Bodies)
      if (F == GI.getResolverFunction())
        Callee = &GI;
  for (auto [F, Callee] : Bodies) {
    // No call forwards a variadic ifunc's arguments.
    if (Callee != F && cast<FunctionType>(Callee->getValueType())->isVarArg())
      continue;
    bool Refusal =
        F->getFnAttribute("thread-pointer-plain-entry").getValueAsString() ==
        "refusal";
    F->removeFnAttr("thread-pointer-plain-entry");
    // A function that holds no thread pointer runs the same whoever calls it,
    // so its plain entry is it: one body, one address. An ifunc passes its
    // thread pointer to the version it chooses.
    GlobalValue *G;
    if (Callee == F && F->hasFnAttribute("thread-pointer-unused") && !Refusal) {
      G = GlobalAlias::create(Callee->getValueType(), Callee->getAddressSpace(),
                              Callee->getLinkage(), "", Callee, &M);
      G->setVisibility(Callee->getVisibility());
      G->setDLLStorageClass(Callee->getDLLStorageClass());
      G->setUnnamedAddr(Callee->getUnnamedAddr());
      G->setDSOLocal(Callee->isDSOLocal());
    } else if (F->isVarArg() &&
               (any_of(*F,
                       [&](BasicBlock &BB) {
                         BlockAddress *BA = BlockAddress::lookup(&BB);
                         return BA && any_of(BA->users(), [&](User *U) {
                                  auto *I = dyn_cast<Instruction>(U);
                                  return !I || I->getFunction() != F;
                                });
                       }) ||
                any_of(instructions(*F), [](const Instruction &I) {
                  const auto *II = dyn_cast<IntrinsicInst>(&I);
                  return II && II->getIntrinsicID() == Intrinsic::localescape;
                }))) {
      // A block address held outside a variadic function names a block of
      // one body, as a filter's recovery names the frame of one, so such a
      // function keeps the one body, passed none.
      F->removeFnAttr("thread-pointer-argument");
      F->removeFnAttr("pin-thread-pointer");
      continue;
    } else {
      Function *Body;
      if (F->isVarArg()) {
        ValueToValueMapTy VMap;
        Body = CloneFunction(F, VMap);
      } else {
        auto *FTy = cast<FunctionType>(Callee->getValueType());
        Body = Function::Create(FTy, Callee->getLinkage(), F->getAddressSpace(),
                                "", &M);
        Body->copyAttributesFrom(F);
        // It unwinds through nothing of its own, so its derivation's probe
        // takes a landing pad whatever the function's personality.
        Body->setPersonalityFn(nullptr);
        Body->setVisibility(Callee->getVisibility());
        Body->setDLLStorageClass(Callee->getDLLStorageClass());
        Body->setDSOLocal(Callee->isDSOLocal());
        SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
        cast<GlobalObject>(Callee)->getAllMetadata(MDs);
        for (auto &[Kind, MD] : MDs)
          if (Kind != LLVMContext::MD_dbg)
            Body->addMetadata(Kind, *MD);
        IRBuilder<> B(BasicBlock::Create(M.getContext(), "", Body));
        CallInst *Call = B.CreateCall(
            FTy, Callee,
            SmallVector<Value *, 8>(make_pointer_range(Body->args())));
        Call->setCallingConv(F->getCallingConv());
        Call->setTailCall();
        if (Body->getReturnType()->isVoidTy())
          B.CreateRetVoid();
        else
          B.CreateRet(Call);
      }
      // An ifunc's resolver leaves no section on COFF, so an inline ifunc's
      // plain entry is a COMDAT of its own.
      Body->setComdat(
          Callee == F ? F->getComdat()
          : Body->isWeakForLinker()
              ? M.getOrInsertComdat(("\01" + Callee->getName()).str())
              : nullptr);
      Body->removeFnAttr("thread-pointer-argument");
      Body->removeFnAttr("pin-thread-pointer");
      Body->removeFnAttr("thread-pointer-unused");
      Body->addFnAttr("thread-pointer-plain-entry");
      if (Refusal)
        Body->addFnAttr("thread-pointer-refusal");
      G = Body;
    }
    G->setName("\01" + Callee->getName());
    Plain[Callee] = G;
  }
  // An alias of such a function, or of such an alias, names both of its
  // entries.
  auto PlainOf = [&](auto &Self, GlobalValue *GV) -> GlobalValue * {
    auto *A = dyn_cast_or_null<GlobalAlias>(GV);
    if (!A || A->getName().starts_with("\01") || Plain.contains(A))
      return Plain.lookup(GV);
    GlobalValue *G = Self(
        Self, dyn_cast<GlobalValue>(A->getAliasee()->stripPointerCasts()));
    if (G) {
      auto *PA =
          GlobalAlias::create(A->getValueType(), A->getAddressSpace(),
                              A->getLinkage(), "\01" + A->getName(), G, &M);
      PA->copyAttributesFrom(A);
      G = PA;
    }
    return Plain[A] = G;
  };
  for (GlobalAlias &A : make_early_inc_range(M.aliases()))
    PlainOf(PlainOf, &A);
  // What is kept for a reference the compiler cannot see is kept with its
  // plain entry, which is what such a reference names.
  for (bool CompilerUsed : {false, true}) {
    SmallVector<GlobalValue *, 8> Used, New;
    collectUsedGlobalVariables(M, Used, CompilerUsed);
    for (GlobalValue *GV : Used)
      if (GlobalValue *G = Plain.lookup(GV))
        New.push_back(G);
    CompilerUsed ? appendToCompilerUsed(M, New) : appendToUsed(M, New);
  }
}
