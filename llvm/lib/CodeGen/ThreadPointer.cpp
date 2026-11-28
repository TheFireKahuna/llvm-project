//===- ThreadPointer.cpp - Pass and derive the thread pointer -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Under the owned thread-local model a thread's thread pointer names its
// block, which the runtime finds from the client id in the thread's TEB
// through its TID map. A function that carries "thread-pointer-argument" is
// passed it in a register; any other comes by its own, as its entry says:
//
//   derive (no mark)           inline, the cold arm __tp_slow(&map)
//   "thread-pointer-refusal"   inline at the head of the entry block, the cold
//                              arm __tp_try(&map), a negative answer returned
//   "thread-pointer-probe"     from __tp_probe(), which never adopts
//   "no-thread-pointer"        the runtime's no-thread header, __tp_none
//
// The derivation has no memory dependence for the life of a thread, so it is
// emitted once, at the nearest common dominator of its uses raised out of
// every loop, as the local-dynamic TLS clean-up places the module base:
//
//   tid  = TEB client id                gs:[0x48], [x18, #0x48]
//   map  = __imp___tp_map$val       acquire; zero until the fill runs
//   base = __imp___tp_base$val
//   e    = probe32 [map + tid]          zero on a page not committed
//   tp   = base + (((e & 63) + ((e & MASK & ~63) << 2)) << 10)
//                                       where e & 0x30, else the cold arm
//
// A funclet the unwinder calls is entered with no thread pointer of the
// function's, so its own uses take one from __tp_probe at its head; on
// NT-POSIX, whose unwind driver calls it with the thread pointer in its
// register, a funclet of a function passed one reads it there.
//
// A function holds a thread pointer where it reads one or passes it to calls
// that read it often enough (holdsThreadPointer); it passes it to each callee
// that may read it, and makes every other call at the callee's plain entry,
// which this pass gives each function passed one that code holding none may
// call. It hands each call that passes the thread pointer its
// "threadpointer" bundle, replaces llvm.thread.pointer, and lowers
// llvm.threadlocal.address: a local-exec variable at its offset from the
// thread pointer, a variable of the image at its offset from the image's
// base, which llvm.threadlocal.base forms once per function at the site the
// linker writes in the image's form, and any other through its record.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/ThreadPointer.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/StackProtector.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "thread-pointer"

// A block holds 64 slots, of which the entry's low six bits are the one.
static constexpr uint64_t SlotBits = 63;
static constexpr unsigned ChunkShift = TP_CHUNK_SHIFT - TP_BLOCK_SHIFT - 6;

// A word of the image's own that its fill writes from the runtime's export.
static GlobalVariable *getSealedValue(Module &M, const char *Name) {
  auto *GV = cast<GlobalVariable>(
      M.getOrInsertGlobal(Name, Type::getInt64Ty(M.getContext())));
  GV->setDSOLocal(true);
  return GV;
}

// One of the runtime's out-of-line answers, imported, which preserves every
// register but the answer and the target's call scratch.
static FunctionCallee getRuntime(Module &M, const char *Name, bool TakesMap) {
  PointerType *Ptr = PointerType::getUnqual(M.getContext());
  FunctionCallee Callee = M.getOrInsertFunction(
      Name,
      FunctionType::get(
          Ptr, TakesMap ? ArrayRef<Type *>(Ptr) : ArrayRef<Type *>(), false));
  auto *F = cast<Function>(Callee.getCallee());
  if (F->isDeclaration()) {
    F->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
    F->setCallingConv(
        M.getTargetTriple().isAArch64()
            ? CallingConv::AArch64_SME_ABI_Support_Routines_PreserveMost_From_X1
            : CallingConv::PreserveAll);
    F->addFnAttr(Attribute::NoUnwind);
    F->addFnAttr(Attribute::Cold);
  }
  return Callee;
}

static CallInst *callRuntime(IRBuilder<> &B, Module &M, const char *Name,
                             ArrayRef<Value *> Args = {},
                             ArrayRef<OperandBundleDef> Bundles = {}) {
  FunctionCallee Callee = getRuntime(M, Name, !Args.empty());
  CallInst *Call = B.CreateCall(Callee, Args, Bundles);
  Call->setCallingConv(cast<Function>(Callee.getCallee())->getCallingConv());
  return Call;
}

// Where the code emitted before At says it is: At's location, or line 0 of
// the function, since a call of a function the module defines needs one.
static DebugLoc getLoc(Function &F, Instruction *At) {
  if (DebugLoc DL = At->getDebugLoc())
    return DL;
  if (DISubprogram *SP = F.getSubprogram())
    return DILocation::get(F.getContext(), 0, 0, SP);
  return DebugLoc();
}

// Emits the derivation before At and returns the thread pointer, a phi at the
// head of the block that takes over from At. Where the function Reports, the
// cold arm's refusal is the function's result.
static Value *derive(Function &F, Instruction *At, bool Reports) {
  Module &M = *F.getParent();
  LLVMContext &C = F.getContext();
  Type *I32 = Type::getInt32Ty(C), *I64 = Type::getInt64Ty(C);
  GlobalVariable *MapWord = getSealedValue(M, COFF::TP::MapImport);
  GlobalVariable *BaseWord = getSealedValue(M, COFF::TP::BaseImport);
  BasicBlock *Home = At->getParent();
  MDNode *Unlikely = MDBuilder(C).createUnlikelyBranchWeights();
  IRBuilder<> B(At);
  B.SetCurrentDebugLocation(getLoc(F, At));
  // The cold arm, ending in a branch to Rest: a negative answer, which no
  // block's address is, is the function's result.
  auto Cold = [&](BasicBlock *Rest) -> Value * {
    Value *Tp =
        callRuntime(B, M, Reports ? COFF::TP::Try : COFF::TP::Slow, {MapWord});
    if (!Reports)
      return Rest ? B.CreateBr(Rest), Tp : Tp;
    BasicBlock *Refused = BasicBlock::Create(C, "tp.refused", &F, Rest);
    Value *Int = B.CreatePtrToInt(Tp, I64);
    B.CreateCondBr(B.CreateICmpSLT(Int, B.getInt64(0)), Refused, Rest,
                   Unlikely);
    B.SetInsertPoint(Refused);
    B.CreateRet(B.CreateSExtOrTrunc(Int, F.getReturnType()));
    return Tp;
  };

  // A probe's landing needs a landing-pad personality; under a funclet one
  // the cold arm answers every case.
  if (F.hasPersonalityFn() &&
      isFuncletEHPersonality(classifyEHPersonality(F.getPersonalityFn()))) {
    BasicBlock *Rest = Reports ? Home->splitBasicBlock(At, "tp") : nullptr;
    if (Rest)
      Home->getTerminator()->eraseFromParent(), B.SetInsertPoint(Home);
    return Cold(Rest);
  }

  Value *Teb = M.getTargetTriple().isAArch64()
                   ? B.CreateIntToPtr(
                         B.CreateIntrinsic(
                             Intrinsic::read_register, I64,
                             MetadataAsValue::get(
                                 C, MDNode::get(C, MDString::get(C, "x18")))),
                         B.getPtrTy())
                   : ConstantPointerNull::get(B.getPtrTy(256));
  Value *Tid =
      B.CreateLoad(I32, B.CreateConstGEP1_64(B.getInt8Ty(), Teb, TP_CLIENT_ID));
  LoadInst *Map = B.CreateAlignedLoad(I64, MapWord, Align(8));
  Map->setAtomic(AtomicOrdering::Acquire);
  LoadInst *Base = B.CreateAlignedLoad(I64, BaseWord, Align(8));
  Base->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(C, {}));

  // The map's word, then the entry by a fault-probing load and its slot test,
  // each to the cold arm.
  BasicBlock *Probe = Home->splitBasicBlock(At, "tp.probe");
  BasicBlock *Join = Probe->splitBasicBlock(Probe->begin(), "tp");
  BasicBlock *Fast = BasicBlock::Create(C, "tp.fast", &F, Join);
  BasicBlock *ColdBB = BasicBlock::Create(C, "tp.cold", &F, Join);
  Home->getTerminator()->eraseFromParent();
  Probe->getTerminator()->eraseFromParent();
  B.SetInsertPoint(Home);
  B.CreateCondBr(B.CreateICmpEQ(Map, B.getInt64(0)), ColdBB, Probe, Unlikely);
  B.SetInsertPoint(Probe);
  LoadInst *E = B.CreateAlignedLoad(
      I32,
      B.CreateIntToPtr(B.CreateAdd(Map, B.CreateZExt(Tid, I64)),
                       B.getPtrTy(COFF::TP::FaultProbeAddrSpace)),
      Align(4), /*isVolatile=*/true);
  B.CreateCondBr(B.CreateICmpEQ(B.CreateAnd(E, TP_SLOT_TEST), B.getInt32(0)),
                 ColdBB, Fast, Unlikely);

  B.SetInsertPoint(Fast);
  Value *Entry = B.CreateZExt(E, I64);
  Value *Blocks = B.CreateAdd(
      B.CreateAnd(Entry, SlotBits),
      B.CreateShl(B.CreateAnd(Entry, TP_MASK & ~SlotBits), ChunkShift));
  Value *TpFast = B.CreateIntToPtr(
      B.CreateAdd(Base, B.CreateShl(Blocks, TP_BLOCK_SHIFT)), B.getPtrTy());
  B.CreateBr(Join);
  B.SetInsertPoint(ColdBB);
  Value *TpCold = Cold(Join);
  B.SetInsertPoint(&*Join->getFirstInsertionPt());
  PHINode *Tp = B.CreatePHI(B.getPtrTy(), 2, "tp");
  Tp->addIncoming(TpFast, Fast);
  Tp->addIncoming(TpCold, ColdBB);

  // The probe's landing is a landing pad of the function's exception table,
  // so the function has a personality. On Windows Itanium it is the runtime's,
  // which only lets a search pass; on NT-POSIX the module's, or the C one.
  if (!F.hasPersonalityFn()) {
    bool Itanium = M.getTargetTriple().isWindowsItaniumEnvironment();
    auto Own = Itanium ? M.end() : find_if(M, [](Function &G) {
      return G.hasPersonalityFn() &&
             !isFuncletEHPersonality(
                 classifyEHPersonality(G.getPersonalityFn()));
    });
    F.setPersonalityFn(
        Own != M.end() ? Own->getPersonalityFn()
                       : cast<Constant>(M.getOrInsertFunction(
                                             Itanium ? COFF::TP::Personality
                                                     : "__gcc_personality_seh0",
                                             FunctionType::get(I32, true))
                                            .getCallee()));
  }
  return Tp;
}

// The address of a variable another image may define, through the record
// whose import pointer is that of <name>$tls: the defining image's place word
// {place, form, key} and the variable's offset. The base is the thread
// pointer plus the place, or for a wide image the wide word there decoded
// with the key the importer's own import of __tp_key holds; the form
// selects. Every word read is fixed for the life of the thread.
static Value *getRecordAddress(IRBuilder<> &B, Module &M, const GlobalValue *GV,
                               unsigned Word, Value *Tp) {
  LLVMContext &C = M.getContext();
  Type *I32 = B.getInt32Ty(), *I64 = B.getInt64Ty();
  PointerType *Ptr = B.getPtrTy();
  auto *RecordTy = StructType::get(Ptr, I32, I32);
  auto *Record = cast<GlobalVariable>(
      M.getOrInsertGlobal((GV->getName() + "$tls").str(), RecordTy));
  Record->setConstant(true);
  Record->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
  auto *KeyGV = cast<GlobalVariable>(M.getOrInsertGlobal(COFF::TP::Key, I64));
  KeyGV->setConstant(true);
  KeyGV->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
  MDNode *Invariant = MDNode::get(C, {});
  auto Load = [&](Type *Ty, Value *Addr, uint64_t At) {
    LoadInst *L = B.CreateAlignedLoad(
        Ty, B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), Addr, At),
        Align(Ty == I32 ? 4 : 8));
    L->setMetadata(LLVMContext::MD_invariant_load, Invariant);
    return L;
  };
  Value *PlaceWord = Load(Ptr, Record, TP_RECORD_PLACE_AT);
  Value *Direct =
      B.CreateGEP(B.getInt8Ty(), Tp, Load(I64, PlaceWord, TP_PLACE_AT));
  Value *WideWord = Load(I64, Direct, 0);
  Value *Wide =
      B.CreateIntToPtr(B.CreateXor(B.CreateIntrinsic(Intrinsic::fshr, I64,
                                                     {WideWord, WideWord,
                                                      B.getInt64(TP_ROTATE)}),
                                   Load(I64, KeyGV, 0)),
                       Ptr);
  Value *Base = B.CreateSelect(B.CreateICmpNE(Load(I64, PlaceWord, TP_FORM_AT),
                                              B.getInt64(TP_FORM_DIRECT)),
                               Wide, Direct);
  return B.CreateGEP(B.getInt8Ty(), Base,
                     B.CreateZExt(Load(I32, Record, Word), I64));
}

static bool runImpl(Function &F, DominatorTree &DT, const TargetMachine &TM) {
  // The uses of the thread pointer the function holds, if any: what reads it
  // and the calls that pass it. Every other call of a function passed one is
  // made at its plain entry, but a plain entry's call of its own function.
  SmallVector<CallBase *, 8> Uses;
  bool Holds = holdsThreadPointer(F), Changed = false;
  for (Instruction &I : instructions(F)) {
    auto *CB = dyn_cast<CallBase>(&I);
    if (!CB || CB->getOperandBundle(LLVMContext::OB_threadpointer))
      continue;
    if (CB->getIntrinsicID() == Intrinsic::thread_pointer ||
        CB->getIntrinsicID() == Intrinsic::threadlocal_address ||
        (Holds && passesThreadPointer(*CB))) {
      Uses.push_back(CB);
      continue;
    }
    auto *Callee = dyn_cast<GlobalValue>(CB->getCalledOperand());
    const auto *Body = dyn_cast_or_null<Function>(
        Callee ? Callee->getAliaseeObject() : nullptr);
    // A definition of the module's own that holds none is its plain entry,
    // whose callee the call keeps.
    if (Body && Body->hasFnAttribute("thread-pointer-argument") &&
        !F.hasFnAttribute("thread-pointer-plain-entry") &&
        !(Body->hasFnAttribute("thread-pointer-unused") &&
          !Body->isDeclaration() && Body->isDefinitionExact())) {
      CB->setCalledOperand(getThreadPointerPlainEntry(*Callee));
      Changed = true;
    }
  }
  if (Uses.empty())
    return Changed;

  Module &M = *F.getParent();
  LLVMContext &C = F.getContext();
  PointerType *Ptr = PointerType::getUnqual(C);
  BasicBlock &EntryBB = F.getEntryBlock();

  // A use in a funclet the unwinder calls, under the pad that begins it.
  DenseMap<BasicBlock *, ColorVector> Colors;
  if (F.hasPersonalityFn() &&
      isScopedEHPersonality(classifyEHPersonality(F.getPersonalityFn())))
    Colors = colorEHFunclets(F);
  auto FuncletOf = [&](BasicBlock *BB) -> BasicBlock * {
    auto It = Colors.find(BB);
    return It == Colors.end() || It->second.front() == &EntryBB
               ? nullptr
               : It->second.front();
  };

  // The body's home: the nearest common dominator of its uses, raised out of
  // every loop and landing pad.
  BasicBlock *Home = nullptr;
  for (CallBase *Use : Uses)
    if (!FuncletOf(Use->getParent()))
      Home = Home ? DT.findNearestCommonDominator(Home, Use->getParent())
                  : Use->getParent();
  bool Reports = F.hasFnAttribute("thread-pointer-refusal");
  Instruction *At = nullptr;
  if (Home) {
    LoopInfo LI(DT);
    while (Home != &EntryBB && (LI.getLoopFor(Home) || Home->isEHPad())) {
      if (Loop *L = LI.getLoopFor(Home))
        Home = L->getOutermostLoop()->getHeader();
      Home = DT.getNode(Home)->getIDom()->getBlock();
    }
    At = Home->getTerminator();
    for (CallBase *Use : Uses)
      if (Use->getParent() == Home && Use->comesBefore(At))
        At = Use;
    // A refusal is returned before anything of the body has run; the frame's
    // escape stays in the entry block with its allocas.
    if (Home == &EntryBB || Reports) {
      At = &*EntryBB.getFirstNonPHIOrAlloca();
      for (Instruction &I : make_range(At->getIterator(), EntryBB.end()))
        if (auto *II = dyn_cast<IntrinsicInst>(&I);
            II && II->getIntrinsicID() == Intrinsic::localescape)
          At = II->getNextNode();
    }
  }

  // The thread pointer of the body, and of each funclet.
  Value *Tp = nullptr;
  if (F.hasFnAttribute("no-thread-pointer")) {
    auto *None = cast<GlobalVariable>(
        M.getOrInsertGlobal(COFF::TP::None, Type::getInt8Ty(C)));
    if (None->isDeclaration())
      None->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
    Tp = None;
  } else if (!Home) {
  } else if (F.hasFnAttribute("thread-pointer-argument")) {
    Tp = CallInst::Create(
        Intrinsic::getOrInsertDeclaration(&M, Intrinsic::thread_pointer, Ptr),
        "tp", EntryBB.getFirstNonPHIOrAlloca());
  } else if (F.hasFnAttribute("thread-pointer-probe")) {
    IRBuilder<> B(At);
    B.SetCurrentDebugLocation(getLoc(F, At));
    Tp = callRuntime(B, M, COFF::TP::Probe);
  } else {
    Tp = derive(F, At, Reports);
  }
  MapVector<BasicBlock *, std::pair<Value *, Value *>> Funclets;
  auto GetTp = [&](CallBase *Use) -> std::pair<Value *, Value *> & {
    BasicBlock *Pad = isa_and_nonnull<GlobalVariable>(Tp)
                          ? nullptr
                          : FuncletOf(Use->getParent());
    auto [It, New] = Funclets.try_emplace(Pad, Tp, nullptr);
    if (New && Pad) {
      IRBuilder<> B(&*Pad->getFirstInsertionPt());
      B.SetCurrentDebugLocation(getLoc(F, Use));
      OperandBundleDef Funclet("funclet", &*Pad->getFirstNonPHIIt());
      It->second.first =
          F.hasFnAttribute("thread-pointer-argument") &&
                  TM.getTargetTriple().isWindowsNTPOSIXEnvironment()
              ? B.CreateCall(Intrinsic::getOrInsertDeclaration(
                                 &M, Intrinsic::thread_pointer, Ptr),
                             {}, Funclet, "tp")
              : callRuntime(B, M, COFF::TP::Probe, {}, Funclet);
    }
    return It->second;
  };

  // Each use takes its region's thread pointer: a read of it, a call that
  // passes it in its bundle, or a variable's address, a local-exec one's at
  // its offset from the thread pointer, one of the image at its offset from
  // the image's base, formed once where the thread pointer is, and any other
  // through its record. At follows a use it was.
  for (CallBase *Use : Uses) {
    auto &[UseTp, Base] = GetTp(Use);
    Intrinsic::ID ID = Use->getIntrinsicID();
    Instruction *Next = Use->getNextNode();
    Value *New = UseTp;
    IRBuilder<> B(Use);
    auto *GV =
        ID == Intrinsic::threadlocal_address
            ? cast<GlobalVariable>(Use->getArgOperand(0)->stripPointerCasts())
            : nullptr;
    auto [RecordOf, Word] = GV ? TargetMachine::getThreadLocalRecord(GV)
                               : std::pair<const GlobalValue *, unsigned>();
    bool LocalExec =
        GV && GV->getThreadLocalMode() == GlobalValue::LocalExecTLSModel;
    if (ID == Intrinsic::thread_pointer && UseTp == Use)
      continue;
    if (!GV && ID != Intrinsic::thread_pointer) {
      auto *Passing = CallBase::addOperandBundle(
          Use, LLVMContext::OB_threadpointer,
          OperandBundleDef("threadpointer", UseTp), Use->getIterator());
      Passing->copyMetadata(*Use);
      Passing->takeName(Use);
      New = Next = Passing;
    } else if (GV && !LocalExec &&
               (!TM.shouldAssumeDSOLocal(RecordOf) ||
                (RecordOf != GV && GV->isDeclaration()))) {
      New = getRecordAddress(B, M, RecordOf, Word, UseTp);
    } else if (GV) {
      auto *TpI = dyn_cast<Instruction>(UseTp);
      if (!LocalExec && !Base)
        Base = IRBuilder<>(!TpI || isa<IntrinsicInst>(TpI) ? At
                           : isa<PHINode>(TpI)
                               ? &*TpI->getParent()->getFirstInsertionPt()
                               : TpI->getNextNode())
                   .CreateIntrinsic(Intrinsic::threadlocal_base, {}, {UseTp});
      New = B.CreateGEP(B.getInt8Ty(), LocalExec ? UseTp : Base,
                        B.CreateIntrinsic(Intrinsic::threadlocal_offset,
                                          GV->getType(), {GV}));
    }
    if (At == Use)
      At = Next;
    Use->replaceAllUsesWith(New);
    Use->eraseFromParent();
  }
  return true;
}

// A function that reads its thread pointer holds it, whatever was inferred
// before its body took the read; code passing none may call each function
// passed one at its plain entry, which is the function's address. Code
// reaches every thread-local variable through llvm.threadlocal.address,
// which IR that names one directly gets here.
static bool prepareModule(Module &M) {
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.isThreadLocal())
      continue;
    convertUsersOfConstantsToInstructions(&GV);
    for (Use &U : make_early_inc_range(GV.uses())) {
      auto *I = dyn_cast<Instruction>(U.getUser());
      auto *II = dyn_cast_or_null<IntrinsicInst>(I);
      if (!I ||
          (II && (II->getIntrinsicID() == Intrinsic::threadlocal_address ||
                  II->getIntrinsicID() == Intrinsic::threadlocal_offset)))
        continue;
      auto *Phi = dyn_cast<PHINode>(I);
      IRBuilder<> B(Phi ? Phi->getIncomingBlock(U)->getTerminator() : I);
      U.set(B.CreateThreadLocalAddress(&GV));
    }
  }
  for (Function &F : M)
    if (F.hasFnAttribute("thread-pointer-unused") &&
        (F.hasFnAttribute(Attribute::SafeStack) ||
         any_of(instructions(F), needsThreadPointer)))
      F.removeFnAttr("thread-pointer-unused");
  createThreadPointerPlainEntries(M);
  takeThreadPointerPlainEntries(M);
  return true;
}

PreservedAnalyses ThreadPointerPass::run(Module &M,
                                         ModuleAnalysisManager &MAM) {
  prepareModule(M);
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (Function &F : M)
    if (!F.isDeclaration())
      runImpl(F, FAM.getResult<DominatorTreeAnalysis>(F), *TM);
  return PreservedAnalyses::none();
}

namespace {
class ThreadPointerLegacy : public FunctionPass {
public:
  static char ID;
  ThreadPointerLegacy() : FunctionPass(ID) {}
  bool doInitialization(Module &M) override { return prepareModule(M); }
  bool runOnFunction(Function &F) override {
    return runImpl(F, getAnalysis<DominatorTreeWrapperPass>().getDomTree(),
                   getAnalysis<TargetPassConfig>().getTM<TargetMachine>());
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<TargetPassConfig>();
    // Instruction selection reads the protector's layout, which this pass,
    // adding no stack object, leaves as it was.
    AU.addPreserved<StackProtector>();
  }
};
} // namespace

char ThreadPointerLegacy::ID = 0;
INITIALIZE_PASS_BEGIN(ThreadPointerLegacy, DEBUG_TYPE,
                      "Pass and derive the thread pointer", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(ThreadPointerLegacy, DEBUG_TYPE,
                    "Pass and derive the thread pointer", false, false)

FunctionPass *llvm::createThreadPointerPass() {
  return new ThreadPointerLegacy();
}
