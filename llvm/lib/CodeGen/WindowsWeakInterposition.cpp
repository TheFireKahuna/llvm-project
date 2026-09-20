//===- WindowsWeakInterposition.cpp - Program-wide weak definitions -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A weak function definition means that it is the implementation's fallback
// and that a strong definition elsewhere in the program supersedes it. On ELF
// that supersession reaches across shared objects; on PE it stops at the image
// boundary, so a program replacing operator new cannot reach the copy the C++
// runtime bound, and blocks allocated on one side and released on the other
// meet two allocators.
//
// This pass gives a weak definition an entry that forwards to the program's
// definition when there is one. Call sites are left alone, so nothing pays for
// the reachability: the cost is one predicted branch at the entry of a kind of
// function that is rare by construction. The pointer it reads is null in the
// image and start-up fills it, so an image whose program replaced nothing runs
// exactly the code it would have run without this pass.
//
// Each such function gets a record in a section of its own, holding a hash of
// the symbol name, the pointer, and the function's own address. Start-up walks
// the section and matches each record against the names the program exports,
// which is where a replacement makes itself reachable, as it does in the
// dynamic symbol table on ELF. The hash is what the match is on, so no names
// are stored.
//
// Only weak-any definitions take part. Inline functions and template
// instantiations are linkonce, which is a deduplication rule rather than a
// statement that the program may supersede them, and covering them would put
// an indirection in front of every inline call.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/xxhash.h"

using namespace llvm;

#define DEBUG_TYPE "windows-weak-interposition"

namespace {

// The linker bounds the run with __wkintp_start and __wkintp_end, which is how
// start-up finds it without a symbol per record. Eight characters, which is
// what a PE section header holds without a string table.
constexpr char RecordSection[] = ".wkintp";

// What __builtin_expect gives a branch nothing else knows the shape of.
constexpr uint32_t LikelyWeight = 2000;
constexpr uint32_t UnlikelyWeight = 1;

class WindowsWeakInterposition : public ModulePass {
public:
  static char ID;

  WindowsWeakInterposition() : ModulePass(ID) {}

  StringRef getPassName() const override {
    return "Windows weak definition interposition";
  }

  bool runOnModule(Module &M) override;

private:
  // The record a linker and the C runtime read. The layout is fixed.
  StructType *getRecordType(Module &M);
  // Returns the record for F, creating it, or null when F cannot forward.
  GlobalVariable *createRecord(Module &M, Function &F);
  void addForwardingEntry(Function &F, GlobalVariable *Record);

  StructType *RecordTy = nullptr;
};

} // namespace

char WindowsWeakInterposition::ID = 0;

INITIALIZE_PASS(WindowsWeakInterposition, DEBUG_TYPE,
                "Windows weak definition interposition", false, false)

ModulePass *llvm::createWindowsWeakInterpositionPass() {
  return new WindowsWeakInterposition();
}

StructType *WindowsWeakInterposition::getRecordType(Module &M) {
  if (!RecordTy) {
    Type *Int64 = Type::getInt64Ty(M.getContext());
    PointerType *Ptr = PointerType::getUnqual(M.getContext());
    RecordTy = StructType::create({Int64, Int64, Ptr, Ptr},
                                  "windows.interposition.record");
  }
  return RecordTy;
}

GlobalVariable *WindowsWeakInterposition::createRecord(Module &M, Function &F) {
  // The forwarding entry passes the arguments on unchanged, which a variadic
  // function cannot do, and a naked function has no entry to add one to.
  if (F.isVarArg() || F.hasFnAttribute(Attribute::Naked))
    return nullptr;

  StructType *Ty = getRecordType(M);
  XXH128_hash_t Hash = xxh3_128bits(arrayRefFromStringRef(F.getName()));
  Type *Int64 = Type::getInt64Ty(M.getContext());
  Constant *Init = ConstantStruct::get(
      Ty, {ConstantInt::get(Int64, Hash.low64),
           ConstantInt::get(Int64, Hash.high64),
           ConstantPointerNull::get(PointerType::getUnqual(M.getContext())),
           &F});

  // Constant so that the record lands in read-only memory: start-up opens the
  // page for the one store and closes it again, which is the standing an
  // import address has. Every read of the pointer is volatile, so none of them
  // is folded into the null the initializer holds.
  //
  // Private, because the forwarding entry is the only thing that names it and
  // start-up reaches it by walking the section. An external symbol here would
  // collide when two objects define the same weak function, and would also be
  // picked as the name that makes their weak defaults unique.
  auto *Record = new GlobalVariable(M, Ty, /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, Init,
                                    "__interpose." + F.getName());
  Record->setSection(RecordSection);
  Record->setAlignment(Align(8));
  return Record;
}

void WindowsWeakInterposition::addForwardingEntry(Function &F,
                                                  GlobalVariable *Record) {
  LLVMContext &Context = F.getContext();
  BasicBlock &Original = F.getEntryBlock();
  BasicBlock *Entry = BasicBlock::Create(Context, "interpose.entry", &F,
                                         &Original);
  BasicBlock *Forward = BasicBlock::Create(Context, "interpose.forward", &F,
                                           &Original);

  PointerType *Ptr = PointerType::getUnqual(Context);
  IRBuilder<> Builder(Entry);
  // Volatile: start-up writes this from outside anything the optimizer can
  // see, and the initializer says null.
  Value *Slot = Builder.CreateStructGEP(Record->getValueType(), Record, 2);
  LoadInst *Target = Builder.CreateLoad(Ptr, Slot, /*isVolatile=*/true);
  Target->setAlignment(Align(8));
  // An image whose program replaced nothing runs this on every call, so the
  // body is the edge that falls through and the forward is laid out away from
  // it. Nothing here can know the answer, so it has to be said.
  BranchInst *Branch =
      Builder.CreateCondBr(Builder.CreateIsNull(Target), &Original, Forward);
  Branch->setMetadata(LLVMContext::MD_prof,
                      MDBuilder(Context).createBranchWeights(LikelyWeight,
                                                             UnlikelyWeight));

  Builder.SetInsertPoint(Forward);
  SmallVector<Value *, 8> Args;
  for (Argument &Arg : F.args())
    Args.push_back(&Arg);
  CallInst *Call = Builder.CreateCall(F.getFunctionType(), Target, Args);
  Call->setCallingConv(F.getCallingConv());
  Call->setAttributes(F.getAttributes());
  // A tail call keeps a body that needed no frame from growing one: the
  // arguments and the signature are the ones this function was entered with.
  Call->setTailCallKind(CallInst::TCK_Tail);
  if (F.getReturnType()->isVoidTy())
    Builder.CreateRetVoid();
  else
    Builder.CreateRet(Call);
}

bool WindowsWeakInterposition::runOnModule(Module &M) {
  bool Changed = false;
  for (Function &F : M) {
    if (F.isDeclaration() || !F.hasWeakAnyLinkage())
      continue;
    if (GlobalVariable *Record = createRecord(M, F)) {
      addForwardingEntry(F, Record);
      Changed = true;
    }
  }
  return Changed;
}
