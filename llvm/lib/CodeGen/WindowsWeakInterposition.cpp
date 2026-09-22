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
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/xxhash.h"

using namespace llvm;

#define DEBUG_TYPE "windows-weak-interposition"

// The linker bounds the records with __wkintp_start and __wkintp_end. The name
// fits in a PE section header without a string table.
static constexpr char RecordSection[] = ".wkintp";

namespace {
class WindowsWeakInterposition : public ModulePass {
public:
  static char ID;

  WindowsWeakInterposition() : ModulePass(ID) {}

  StringRef getPassName() const override {
    return "Windows weak definition interposition";
  }

  bool runOnModule(Module &M) override;
};
} // namespace

char WindowsWeakInterposition::ID = 0;

INITIALIZE_PASS(WindowsWeakInterposition, DEBUG_TYPE,
                "Windows weak definition interposition", false, false)

ModulePass *llvm::createWindowsWeakInterpositionPass() {
  return new WindowsWeakInterposition();
}

/// Create the record start-up reads for \p F: { i64 hash.low, i64 hash.high,
/// ptr target, ptr self }. The layout is fixed by the C runtime.
static GlobalVariable *createRecord(Function &F) {
  Module &M = *F.getParent();
  LLVMContext &Ctx = M.getContext();
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  XXH128_hash_t Hash = xxh3_128bits(arrayRefFromStringRef(F.getName()));
  Constant *Init = ConstantStruct::getAnon(
      {ConstantInt::get(Int64Ty, Hash.low64),
       ConstantInt::get(Int64Ty, Hash.high64),
       ConstantPointerNull::get(PtrTy), &F});

  // Constant, so the record is read-only in the image; start-up opens the page
  // for its one store. Private, because start-up finds it by walking the
  // section, and an external name would collide between objects that define
  // the same weak function.
  auto *Record = new GlobalVariable(M, Init->getType(), /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, Init,
                                    "__interpose." + F.getName());
  Record->setSection(RecordSection);
  Record->setAlignment(Align(8));
  return Record;
}

/// Prepend an entry to \p F that tail calls the target in \p Record when
/// start-up has filled it in.
static void addForwardingEntry(Function &F, GlobalVariable *Record) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock *Body = &F.getEntryBlock();
  BasicBlock *Entry = BasicBlock::Create(Ctx, "interpose.entry", &F, Body);
  BasicBlock *Forward = BasicBlock::Create(Ctx, "interpose.forward", &F, Body);

  IRBuilder<> Builder(Entry);
  // Volatile: start-up writes the slot, but the initializer says null.
  Value *Slot = Builder.CreateStructGEP(Record->getValueType(), Record, 2);
  LoadInst *Target = Builder.CreateAlignedLoad(
      Builder.getPtrTy(), Slot, Align(8), /*isVolatile=*/true);
  // Most images replace nothing, so keep the body on the fall-through path.
  Builder.CreateCondBr(Builder.CreateIsNull(Target), Body, Forward,
                       MDBuilder(Ctx).createLikelyBranchWeights());

  Builder.SetInsertPoint(Forward);
  SmallVector<Value *, 8> Args(make_pointer_range(F.args()));
  CallInst *Call = Builder.CreateCall(F.getFunctionType(), Target, Args);
  Call->setCallingConv(F.getCallingConv());
  Call->setAttributes(F.getAttributes());
  Call->setTailCall();
  if (F.getReturnType()->isVoidTy())
    Builder.CreateRetVoid();
  else
    Builder.CreateRet(Call);
}

bool WindowsWeakInterposition::runOnModule(Module &M) {
  bool Changed = false;
  for (Function &F : M) {
    // Forwarding passes the arguments on unchanged, which a variadic function
    // or one with in-memory argument blocks cannot do, and a naked function has
    // no entry to add to.
    if (F.isDeclaration() || !F.hasWeakAnyLinkage() || F.isVarArg() ||
        F.hasFnAttribute(Attribute::Naked) ||
        F.getAttributes().hasAttrSomewhere(Attribute::InAlloca) ||
        F.getAttributes().hasAttrSomewhere(Attribute::Preallocated))
      continue;
    addForwardingEntry(F, createRecord(F));
    Changed = true;
  }
  return Changed;
}
