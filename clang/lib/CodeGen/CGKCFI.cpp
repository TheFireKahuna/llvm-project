//===--- CGKCFI.cpp - KCFI types and link facts ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The KCFI types of calls, vtable slots and destructors when every function
// carries a type prefix, and the link facts that record which KCFI types a
// module opens to functions without a prefix of ours.
//
//===----------------------------------------------------------------------===//

#include "CGKCFI.h"
#include "CGCXXABI.h"
#include "CodeGenModule.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/Basic/Builtins.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/xxhash.h"
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace clang;
using namespace CodeGen;

using KCFITypeId = CodeGenKCFI::KCFITypeId;

CodeGenKCFI::CodeGenKCFI(CodeGenModule &CGM)
    : CGM(CGM),
      HasVTableSlotTypes(CGM.getTarget().getCXXABI().isItaniumFamily()) {}

KCFITypeId CodeGenKCFI::createTypeIds(QualType FnType, StringRef Salt) {
  KCFITypeId TypeId(CGM.CreateKCFITypeId(FnType, Salt), 0);
  // Only link facts read the precise identifier.
  return TypeId;
}

static std::string kcfiVfnSalt(QualType FnType) {
  std::string Salt = "__vfn";
  if (const auto *FP = FnType->getAs<FunctionProtoType>())
    if (const auto &Info = FP->getExtraAttributeInfo())
      Salt = (Info.CFISalt + "." + Salt).str();
  return Salt;
}

KCFITypeId CodeGenKCFI::createVfnTypeIds(QualType FnType) {
  KCFITypeId &TypeId =
      TypeIds[{FnType.getCanonicalType().getAsOpaquePtr(), VfnKind}];
  if (!TypeId.first) {
    TypeId = createTypeIds(FnType, kcfiVfnSalt(FnType));
    // A check of the second type reads, in a function without one on x86, the
    // padding before its marker.
    uint32_t Value = TypeId.first->getZExtValue();
    if (CGM.getTriple().isX86() && llvm::isX86TypePrefixPadding(Value))
      TypeId.first = llvm::ConstantInt::get(CGM.Int32Ty, Value + 1);
  }
  return TypeId;
}

void CodeGenKCFI::setVfnType(llvm::Function *F, QualType FnType) {
  F->setMetadata("kcfi_vfn_type",
                 llvm::MDNode::get(CGM.getLLVMContext(),
                                   llvm::ConstantAsMetadata::get(
                                       createVfnTypeIds(FnType).first)));
}

static StringRef kcfiCallSalt(QualType FnType) {
  if (const auto *FP = FnType->getAs<FunctionProtoType>())
    if (const auto &Info = FP->getExtraAttributeInfo())
      return Info.CFISalt;
  return StringRef();
}

KCFITypeId CodeGenKCFI::createCallTypeIds(QualType FnType) {
  KCFITypeId &TypeId =
      TypeIds[{FnType.getCanonicalType().getAsOpaquePtr(), CallKind}];
  if (!TypeId.first)
    TypeId = createTypeIds(FnType, kcfiCallSalt(FnType));
  return TypeId;
}

/// The salt of a destructor's KCFI type, further salted by DeletingClass for
/// a deleting destructor.
static std::string kcfiDestructorSalt(CodeGenModule &CGM,
                                      const CXXRecordDecl *DeletingClass) {
  std::string Salt = "__cxa_dtor";
  if (DeletingClass) {
    llvm::raw_string_ostream Out(Salt);
    Out << '.';
    CGM.getCXXABI().getMangleContext().mangleCanonicalTypeName(
        CGM.getContext().getCanonicalTagType(DeletingClass), Out);
  }
  return Salt;
}

/// The function type the runtime calls a destructor through.
static QualType kcfiDestructorType(ASTContext &Ctx) {
  return Ctx.getFunctionType(Ctx.VoidTy, {Ctx.VoidPtrTy},
                             FunctionProtoType::ExtProtoInfo());
}

KCFITypeId
CodeGenKCFI::createDestructorTypeIds(const CXXRecordDecl *DeletingClass) {
  KCFITypeId &TypeId = TypeIds[{DeletingClass, DestructorKind}];
  if (!TypeId.first)
    TypeId = createTypeIds(kcfiDestructorType(CGM.getContext()),
                           kcfiDestructorSalt(CGM, DeletingClass));
  return TypeId;
}

static std::string kcfiSlotSalt(CodeGenModule &CGM, const CXXMethodDecl *MD) {
  std::string Salt;
  if (const auto *FP = MD->getType()->getAs<FunctionProtoType>())
    if (const auto &Info = FP->getExtraAttributeInfo())
      Salt = (Info.CFISalt + ".").str();
  llvm::raw_string_ostream Out(Salt);
  CGM.getCXXABI().getMangleContext().mangleCanonicalTypeName(
      CGM.getContext().getCanonicalTagType(MD->getParent()), Out);
  return Salt;
}

KCFITypeId CodeGenKCFI::createVTableSlotTypeIds(GlobalDecl Slot) {
  if (const auto *DD = dyn_cast<CXXDestructorDecl>(Slot.getDecl()))
    return createDestructorTypeIds(
        Slot.getDtorType() == Dtor_Deleting ? DD->getParent() : nullptr);

  const auto *MD = cast<CXXMethodDecl>(Slot.getDecl()->getCanonicalDecl());
  KCFITypeId &TypeId = TypeIds[{MD, SlotKind}];
  if (!TypeId.first)
    TypeId = createTypeIds(MD->getType(), kcfiSlotSalt(CGM, MD));
  return TypeId;
}

bool CodeGenKCFI::setVTableSlotType(GlobalDecl GD, llvm::Function *F) {
  // A virtual member function carries the type of the vtable slot it
  // occupies in the vtables of its class, and a destructor, which the runtime
  // calls, the type the runtime calls it through.
  const auto *MD = dyn_cast<CXXMethodDecl>(GD.getDecl());
  if (!MD || !HasVTableSlotTypes ||
      !(MD->isVirtual() || isa<CXXDestructorDecl>(MD)))
    return false;
  llvm::ConstantInt *TypeId;
  if (isa<CXXDestructorDecl>(MD) && GD.getDtorType() != Dtor_Deleting)
    TypeId = createDestructorTypeIds().first;
  else
    TypeId = createVTableSlotTypeIds(
                 CGM.getItaniumVTableContext().findOriginalMethod(
                     GD.getCanonicalDecl()))
                 .first;
  F->setMetadata(llvm::LLVMContext::MD_kcfi_type,
                 llvm::MDNode::get(CGM.getLLVMContext(),
                                   llvm::ConstantAsMetadata::get(TypeId)));
  if (!isa<CXXDestructorDecl>(MD))
    setVfnType(F, MD->getType());
  return true;
}

