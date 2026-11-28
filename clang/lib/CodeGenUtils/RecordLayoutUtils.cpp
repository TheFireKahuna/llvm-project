//===--- RecordLayoutUtils.cpp - Shared record layout queries -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CodeGenUtils/RecordLayoutUtils.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/TargetInfo.h"

namespace clang::CodeGenUtils {

bool hasOwnStorage(const ASTContext &Ctx, const CXXRecordDecl *Decl,
                   const CXXRecordDecl *Query) {
  const ASTRecordLayout &DeclLayout = Ctx.getASTRecordLayout(Decl);
  if (DeclLayout.isPrimaryBaseVirtual() && DeclLayout.getPrimaryBase() == Query)
    return false;
  for (const auto &Base : Decl->bases())
    if (!hasOwnStorage(Ctx, Base.getType()->getAsCXXRecordDecl(), Query))
      return false;
  return true;
}

bool isDiscreteBitFieldABI(const ASTContext &Ctx, const RecordDecl *RD) {
  return Ctx.getTargetInfo().getCXXABI().isMicrosoft() || RD->isMsStruct(Ctx);
}

bool isEmptyFieldForLayout(const ASTContext &Ctx, const FieldDecl *FD) {
  if (FD->isZeroLengthBitField())
    return true;

  if (FD->isUnnamedBitField())
    return false;

  return isEmptyRecordForLayout(Ctx, FD->getType());
}

bool isEmptyRecordForLayout(const ASTContext &Ctx, QualType T) {
  const auto *RD = T->getAsRecordDecl();
  if (!RD)
    return false;

  // If this is a C++ record, check the bases first.
  if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
    if (CXXRD->isDynamicClass())
      return false;

    for (const auto &I : CXXRD->bases())
      if (!isEmptyRecordForLayout(Ctx, I.getType()))
        return false;
  }

  for (const auto *I : RD->fields())
    if (!isEmptyFieldForLayout(Ctx, I))
      return false;

  return true;
}

bool isOverlappingVBaseABI(const ASTContext &Ctx) {
  return !Ctx.getTargetInfo().getCXXABI().isMicrosoft();
}

void LayoutWalker::walkType(QualType T, uint64_t Offset) {
  T = Ctx.getCanonicalType(T).getUnqualifiedType();
  if (const auto *AT = T->getAs<AtomicType>();
      AT && Ctx.getTypeSize(AT->getValueType()) == Ctx.getTypeSize(T))
    return walkType(AT->getValueType(), Offset);
  if (const auto *AT = Ctx.getAsConstantArrayType(T))
    return array(Offset, T, AT->getZExtSize(), AT->getElementType());
  if (const auto *CT = T->getAs<ComplexType>())
    return array(Offset, T, 2, CT->getElementType());
  if (const auto *VT = T->getAs<VectorType>())
    return array(Offset, T, VT->getNumElements(), VT->getElementType());
  if (const RecordDecl *RD = T->getAsRecordDecl())
    return record(Offset, RD);

  ScalarKind Kind = ScalarKind::Opaque;
  if (T->isBitIntType())
    Kind = ScalarKind::BitInt;
  else if (T->isIntegerType() || T->isEnumeralType())
    Kind = ScalarKind::Integer;
  else if (T->isRealFloatingType())
    Kind = ScalarKind::Float;
  else if (T->isAnyPointerType() || T->isReferenceType() ||
           T->isBlockPointerType() || T->isNullPtrType())
    Kind = ScalarKind::Pointer;
  else if (T->isMemberFunctionPointerType())
    Kind = ScalarKind::MemberFunctionPointer;
  else if (T->isMemberDataPointerType())
    Kind = ScalarKind::MemberDataPointer;
  scalar(Offset, Kind, T);
}

void LayoutWalker::walkRecord(const RecordDecl *RD, uint64_t Offset,
                              bool IsCompleteObject) {
  const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
  if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD)) {
    if (Layout.hasOwnVFPtr())
      scalar(Offset, ScalarKind::VTablePointer, Ctx.VoidPtrTy);
    for (const CXXBaseSpecifier &B : CRD->bases())
      if (!B.isVirtual()) {
        const CXXRecordDecl *Base = B.getType()->getAsCXXRecordDecl();
        base(Offset + Ctx.toBits(Layout.getBaseClassOffset(Base)), Base,
             /*IsVirtual=*/false);
      }
    if (IsCompleteObject)
      for (const CXXBaseSpecifier &B : CRD->vbases()) {
        const CXXRecordDecl *Base = B.getType()->getAsCXXRecordDecl();
        base(Offset + Ctx.toBits(Layout.getVBaseClassOffset(Base)), Base,
             /*IsVirtual=*/true);
      }
  }
  for (const FieldDecl *FD : RD->fields()) {
    uint64_t FieldOffset = Offset + Layout.getFieldOffset(FD->getFieldIndex());
    if (!FD->isBitField())
      walkType(FD->getType(), FieldOffset);
    else if (FD->getBitWidthValue() != 0)
      bitField(FieldOffset, FD);
  }
}

} // namespace clang::CodeGenUtils
