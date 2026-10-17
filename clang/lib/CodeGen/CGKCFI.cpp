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
    : CGM(CGM), HasFacts(CGM.getTriple().isOSBinFormatCOFF()),
      HasVTableSlotTypes(CGM.getTarget().getCXXABI().isItaniumFamily()) {}

/// Whether T names a record that has no name, or one declared in a function,
/// whose mangling two translation units need not agree on.
static bool hasKCFIUnstableRecord(QualType T) {
  T = T.getCanonicalType();
  if (T->isPointerType() || T->isReferenceType())
    return hasKCFIUnstableRecord(T->getPointeeType());
  if (const auto *MPT = T->getAs<MemberPointerType>())
    return hasKCFIUnstableRecord(MPT->getPointeeType());
  if (const auto *AT = T->getAsArrayTypeUnsafe())
    return hasKCFIUnstableRecord(AT->getElementType());
  if (const auto *FT = T->getAs<FunctionType>()) {
    if (hasKCFIUnstableRecord(FT->getReturnType()))
      return true;
    if (const auto *FPT = dyn_cast<FunctionProtoType>(FT))
      return llvm::any_of(FPT->param_types(), hasKCFIUnstableRecord);
    return false;
  }
  if (const auto *TD = T->getAsTagDecl())
    return (!TD->getIdentifier() && !TD->getTypedefNameForAnonDecl()) ||
           TD->getDeclContext()->isFunctionOrMethod();
  return false;
}

KCFITypeId CodeGenKCFI::createTypeIds(QualType FnType, StringRef Salt) {
  KCFITypeId TypeId(CGM.CreateKCFITypeId(FnType, Salt), 0);
  // Only link facts read the precise identifier.
  if (!HasFacts)
    return TypeId;
  // Without pointer generalisation the two identifiers are the same, and a
  // record that the translation units may mangle differently keeps the check
  // identifier, so that two objects agree on the precise type.
  if (!CGM.getCodeGenOpts().SanitizeCfiICallGeneralizePointers ||
      hasKCFIUnstableRecord(FnType))
    TypeId.second = TypeId.first->getZExtValue();
  else
    TypeId.second =
        CGM.CreateKCFITypeId(FnType, Salt, /*GeneralizePointers=*/false)
            ->getZExtValue();
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

void CodeGenKCFI::addConversionType(QualType From, QualType To, bool LValue,
                                    const Expr *Operand) {
  if (!HasFacts)
    return;
  // A function pointer that an object of another type is reinterpreted as,
  // or that is reinterpreted as an object of another type, can be read or
  // written untyped, as *(void **)&fp = dlsym(...) or memcpy(&fp, ...) do;
  // so can one that a pointer of another type reaches through more levels.
  if (LValue) {
    if (From->isFunctionPointerType())
      addConversionType(To, From);
    if (To->isFunctionPointerType())
      addConversionType(From, To);
    else if (!From->isFunctionPointerType() && From->isPointerType() &&
             To->isPointerType())
      addConversionType(From->getPointeeType(), To->getPointeeType(),
                        /*LValue=*/true);
    return;
  }
  if (!To->isFunctionPointerType()) {
    if (From->isPointerType() && To->isPointerType()) {
      if (Operand)
        addBoundaryRecord(To->getPointeeType(), Operand);
      addConversionType(From->getPointeeType(), To->getPointeeType(),
                        /*LValue=*/true);
    }
    return;
  }
  QualType FnType = To->getPointeeType();
  if (From->isFunctionPointerType())
    From = From->getPointeeType();
  if (From->isFunctionType() && CGM.getContext().hasSameType(From, FnType))
    return;
  KCFITypeId TypeId = createCallTypeIds(FnType);
  if (From->isFunctionType() && createCallTypeIds(From).first == TypeId.first)
    return;
  // Anything else, an integer, an object pointer or a pointer to a function
  // of another type, may hold the address of a function without a prefix of
  // ours, such as one that GetProcAddress returned.
  DynamicTypes.insert(TypeId);
}

void CodeGenKCFI::addUnionReadType(const Expr *LValue) {
  QualType T = LValue->getType();
  if (!HasFacts || !T->isFunctionPointerType())
    return;
  // A function pointer read from a union may have been written as a member
  // of another type, also when it is held in a record or an array that is
  // the union's member.
  for (const Expr *E = LValue->IgnoreParens();;) {
    if (const auto *Subscript = dyn_cast<ArraySubscriptExpr>(E)) {
      const auto *Decay =
          dyn_cast<ImplicitCastExpr>(Subscript->getBase()->IgnoreParens());
      if (!Decay || Decay->getCastKind() != CK_ArrayToPointerDecay)
        return;
      E = Decay->getSubExpr()->IgnoreParens();
      continue;
    }
    const auto *ME = dyn_cast<MemberExpr>(E);
    const auto *Field = ME ? dyn_cast<FieldDecl>(ME->getMemberDecl()) : nullptr;
    if (!Field)
      return;
    const RecordDecl *RD = Field->getParent();
    if (RD->isUnion())
      for (const FieldDecl *Other : RD->fields())
        if (!CGM.getContext().hasSameType(Other->getType(), Field->getType())) {
          DynamicTypes.insert(createCallTypeIds(T->getPointeeType()));
          return;
        }
    // A pointer leads out of the object.
    if (ME->isArrow())
      return;
    E = ME->getBase()->IgnoreParens();
  }
}

/// Whether code in another image or foreign code says that it may create
/// objects of RD: RD has a uuid, which COM objects implement, is dllimport or
/// dllexport, or has an explicit default visibility that the visibility
/// mapping exports. An implicit visibility says nothing, since every class
/// without an attribute has it.
static bool isKCFIForeignClass(const CodeGenModule &CGM,
                               const CXXRecordDecl *RD) {
  return RD->hasAttr<UuidAttr>() || RD->hasAttr<DLLImportAttr>() ||
         RD->hasAttr<DLLExportAttr>() ||
         CGM.isMappedImportVisibility(RD->getLinkageAndVisibility());
}

bool CodeGenKCFI::isVTableOpen(const CXXRecordDecl *RD) {
  const CXXRecordDecl *Def = RD->getDefinition();
  if (!Def || isKCFIForeignClass(CGM, Def))
    return true;
  return !Def->forallBases([this](const CXXRecordDecl *Base) {
    return !isKCFIForeignClass(CGM, Base);
  });
}

/// Collect into TypeIds the KCFI types of the function pointers that a value
/// of type T holds or reaches through pointers and the fields and bases of
/// records, and of the vtable slots of the polymorphic classes it reaches.
/// With Records, the walk stops at the records it reaches and lists them
/// there instead.
static void collectKCFIReachableTypes(
    CodeGenModule &CGM, QualType T,
    llvm::SmallPtrSetImpl<const RecordDecl *> &Visited,
    llvm::SetVector<KCFITypeId> &TypeIds,
    llvm::SetVector<const RecordDecl *> *Records = nullptr) {
  T = CGM.getContext().getBaseElementType(T.getCanonicalType());
  if (T->isPointerType() || T->isReferenceType()) {
    QualType Pointee = T->getPointeeType();
    if (Pointee->isFunctionType())
      TypeIds.insert(CGM.getKCFI()->createCallTypeIds(Pointee));
    else
      collectKCFIReachableTypes(CGM, Pointee, Visited, TypeIds, Records);
    return;
  }

  const RecordDecl *RD = T->getAsRecordDecl();
  if (RD)
    RD = RD->getDefinition();
  if (RD && Records) {
    Records->insert(RD);
    return;
  }
  if (!RD || !Visited.insert(RD).second)
    return;
  for (const FieldDecl *Field : RD->fields())
    collectKCFIReachableTypes(CGM, Field->getType(), Visited, TypeIds);

  const auto *CXXRD = dyn_cast<CXXRecordDecl>(RD);
  if (!CXXRD)
    return;
  for (const CXXBaseSpecifier &Base : CXXRD->bases())
    collectKCFIReachableTypes(CGM, Base.getType(), Visited, TypeIds);

  // An object of a polymorphic class that another image created may reach
  // that image's functions through any of its vtable slots, when the class
  // says that another image may create its objects, as a virtual call
  // requires.
  if (!CXXRD->isDynamicClass() || !CGM.getKCFI()->hasVTableSlotTypes() ||
      !CGM.getKCFI()->isVTableOpen(CXXRD))
    return;
  ItaniumVTableContext &VTContext = CGM.getItaniumVTableContext();
  for (const CXXMethodDecl *MD : CXXRD->methods()) {
    if (!MD->isVirtual())
      continue;
    MD = MD->getCanonicalDecl();
    if (const auto *DD = dyn_cast<CXXDestructorDecl>(MD)) {
      TypeIds.insert(CGM.getKCFI()->createDestructorTypeIds());
      TypeIds.insert(CGM.getKCFI()->createVTableSlotTypeIds(
          VTContext.findOriginalMethod(GlobalDecl(DD, Dtor_Deleting))));
    } else {
      TypeIds.insert(CGM.getKCFI()->createVTableSlotTypeIds(
          VTContext.findOriginalMethod(GlobalDecl(MD))));
    }
  }
}

static void collectKCFIParamTypes(CodeGenModule &CGM, QualType FnType,
                                  llvm::SetVector<KCFITypeId> &TypeIds,
                                  llvm::SetVector<const RecordDecl *> *Records,
                                  bool Callbacks);

/// Collect into TypeIds the KCFI types of the function pointers that an
/// object of type T holds itself: T, or the fields and bases of a record and
/// of the records it holds by value. Pointers to objects are not followed.
/// With Records, the walk stops at the records it reaches and lists them
/// there instead. With Callbacks, each function pointer that the object holds
/// gives instead the types that its parameters receive, as
/// collectKCFIParamTypes collects them, and Records lists only the records
/// that those parameters reach.
static void
collectKCFIHeldTypes(CodeGenModule &CGM, QualType T,
                     llvm::SmallPtrSetImpl<const RecordDecl *> &Visited,
                     llvm::SetVector<KCFITypeId> &TypeIds,
                     llvm::SetVector<const RecordDecl *> *Records = nullptr,
                     bool Callbacks = false) {
  T = CGM.getContext().getBaseElementType(T.getCanonicalType());
  if (T->isPointerType() || T->isReferenceType()) {
    QualType Pointee = T->getPointeeType();
    if (!Pointee->isFunctionType())
      return;
    if (Callbacks)
      collectKCFIParamTypes(CGM, Pointee, TypeIds, Records,
                            /*Callbacks=*/false);
    else
      TypeIds.insert(CGM.getKCFI()->createCallTypeIds(Pointee));
    return;
  }

  const RecordDecl *RD = T->getAsRecordDecl();
  if (RD)
    RD = RD->getDefinition();
  if (RD && Records && !Callbacks) {
    Records->insert(RD);
    return;
  }
  if (!RD || !Visited.insert(RD).second)
    return;
  for (const FieldDecl *Field : RD->fields())
    collectKCFIHeldTypes(CGM, Field->getType(), Visited, TypeIds, Records,
                         Callbacks);
  if (const auto *CXXRD = dyn_cast<CXXRecordDecl>(RD))
    for (const CXXBaseSpecifier &Base : CXXRD->bases())
      collectKCFIHeldTypes(CGM, Base.getType(), Visited, TypeIds, Records,
                           Callbacks);
}

/// Collect into TypeIds the KCFI types of the function pointers that code
/// hands over in a value of type T: a function pointer, or one held in the
/// object that T is or points to. Function pointers that the receiver reaches
/// further, through pointers in that object, come from wherever the code that
/// hands it over found them. With Callbacks, as collectKCFIHeldTypes.
static void
collectKCFIHandedTypes(CodeGenModule &CGM, QualType T,
                       llvm::SmallPtrSetImpl<const RecordDecl *> &Visited,
                       llvm::SetVector<KCFITypeId> &TypeIds,
                       llvm::SetVector<const RecordDecl *> *Records = nullptr,
                       bool Callbacks = false) {
  T = T.getCanonicalType();
  if ((T->isPointerType() || T->isReferenceType()) &&
      !T->getPointeeType()->isFunctionType())
    T = T->getPointeeType();
  collectKCFIHeldTypes(CGM, T, Visited, TypeIds, Records, Callbacks);
}

/// Collect into TypeIds the KCFI types of the function pointers that a call
/// to a function of type FnType hands it in its parameters, as
/// collectKCFIHandedTypes walks them. With Callbacks, those that the callee,
/// which may be foreign code, may in turn hand to the functions that the call
/// hands it: code that is handed a function of ours may call it with
/// pointers to its own functions.
static void collectKCFIParamTypes(CodeGenModule &CGM, QualType FnType,
                                  llvm::SetVector<KCFITypeId> &TypeIds,
                                  llvm::SetVector<const RecordDecl *> *Records,
                                  bool Callbacks) {
  const auto *FPT = FnType->getAs<FunctionProtoType>();
  if (!FPT)
    return;
  llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
  for (QualType T : FPT->param_types())
    collectKCFIHandedTypes(CGM, T, Visited, TypeIds, Records, Callbacks);
}

/// The declaration whose value E directly is, when E is a call to a function
/// that it names or a load of a variable: a value that may come from another
/// image or from foreign code.
static const NamedDecl *getKCFIBoundaryDecl(const Expr *E) {
  E = E->IgnoreParens();
  if (const auto *Call = dyn_cast<CallExpr>(E)) {
    // Memory that an allocation function returns holds no function pointer
    // yet. The replaceable global allocation functions and the C library's
    // allocation functions carry an implicit alloc_size.
    const FunctionDecl *FD = Call->getDirectCallee();
    if (FD && (FD->hasAttr<RestrictAttr>() || FD->hasAttr<AllocSizeAttr>()))
      return nullptr;
    return FD;
  }
  if (const auto *Cast = dyn_cast<ImplicitCastExpr>(E))
    if (Cast->getCastKind() == CK_LValueToRValue)
      if (const auto *Ref =
              dyn_cast<DeclRefExpr>(Cast->getSubExpr()->IgnoreParens()))
        return dyn_cast<VarDecl>(Ref->getDecl());
  return nullptr;
}

/// Collect into TypeIds the KCFI types of the function pointers that a call
/// to a function of type FnType hands back to its caller, through its return
/// type and through the objects that its pointer parameters to non-const
/// types point to, following pointers and the fields and bases of records.
/// With Handed, only the function pointers that the returned value holds and
/// those that the callee stores where an out-parameter, a pointer to a
/// non-const pointer, points count, as collectKCFIHandedTypes walks them,
/// and with Callbacks, the types that those function pointers' parameters
/// receive instead. With Records, the walk stops at the records it reaches and
/// lists them there instead.
static void
collectKCFIResultTypes(CodeGenModule &CGM, QualType FnType, bool Handed,
                       llvm::SmallPtrSetImpl<const RecordDecl *> &Visited,
                       llvm::SetVector<KCFITypeId> &TypeIds,
                       llvm::SetVector<const RecordDecl *> *Records = nullptr,
                       bool Callbacks = false) {
  const auto *FT = FnType->castAs<FunctionType>();
  if (Handed)
    collectKCFIHandedTypes(CGM, FT->getReturnType(), Visited, TypeIds, Records,
                           Callbacks);
  else
    collectKCFIReachableTypes(CGM, FT->getReturnType(), Visited, TypeIds,
                              Records);
  const auto *FPT = dyn_cast<FunctionProtoType>(FT);
  if (!FPT)
    return;
  // A function pointer passed by value flows to the callee, but the callee
  // can store one into an object that a pointer to a non-const type points
  // to.
  for (QualType T : FPT->param_types()) {
    T = T.getCanonicalType();
    if (!(T->isPointerType() || T->isReferenceType()) ||
        T->getPointeeType().isConstQualified())
      continue;
    if (Handed) {
      if (T->getPointeeType()->isPointerType())
        collectKCFIHandedTypes(CGM, T->getPointeeType(), Visited, TypeIds,
                               Records, Callbacks);
    } else
      collectKCFIReachableTypes(CGM, T->getPointeeType(), Visited, TypeIds,
                                Records);
  }
}

/// E without the conversions between pointer types around it.
static const Expr *ignoreKCFIPointerConversions(const Expr *E) {
  E = E->IgnoreParens();
  while (const auto *Cast = dyn_cast<CastExpr>(E)) {
    if (Cast->getCastKind() != CK_BitCast && Cast->getCastKind() != CK_NoOp)
      break;
    E = Cast->getSubExpr()->IgnoreParens();
  }
  return E;
}

/// Whether only this translation unit stores into VD: a local variable, a
/// parameter or a variable with internal linkage. Its values are those that
/// its initializer and its assignments store.
static bool isKCFILocalVariable(const VarDecl *VD) {
  return VD->isLocalVarDeclOrParm() || !VD->isExternallyVisible();
}

void CodeGenKCFI::addBoundaryRecord(QualType Pointee, const Expr *Operand) {
  if (!Pointee->isRecordType())
    return;
  const NamedDecl *D = getKCFIBoundaryDecl(Operand);
  const RecordDecl *RD = Pointee->getAsRecordDecl()->getDefinition();
  if (!D || !RD)
    return;
  D = cast<NamedDecl>(D->getCanonicalDecl());
  if (const auto *VD = dyn_cast<VarDecl>(D); VD && isKCFILocalVariable(VD))
    LocalBoundaryRecords[VD].insert(RD);
  else
    BoundaryRecords[D].insert(RD);
}

void CodeGenKCFI::addAssignment(const Expr *LHS, const Expr *RHS) {
  if (!HasFacts || !LHS->getType()->isPointerType())
    return;
  const auto *Ref = dyn_cast<DeclRefExpr>(LHS->IgnoreParens());
  const auto *VD = Ref ? dyn_cast<VarDecl>(Ref->getDecl()) : nullptr;
  if (!VD || !isKCFILocalVariable(VD))
    return;
  if (const NamedDecl *D =
          getKCFIBoundaryDecl(ignoreKCFIPointerConversions(RHS)))
    LocalSources[VD->getCanonicalDecl()].insert(
        cast<NamedDecl>(D->getCanonicalDecl()));
}

void CodeGenKCFI::resolveLocalBoundaryRecords() {
  for (const auto &[Local, Records] : LocalBoundaryRecords) {
    SmallVector<const VarDecl *, 4> Worklist = {Local};
    llvm::SmallPtrSet<const VarDecl *, 4> Visited = {Local};
    while (!Worklist.empty()) {
      const VarDecl *VD = Worklist.pop_back_val();
      llvm::SetVector<const NamedDecl *> Sources;
      // A parameter's initializer is its default argument, which callers
      // that pass the argument do not store.
      if (const Expr *Init = VD->getAnyInitializer();
          Init && !isa<ParmVarDecl>(VD))
        if (const NamedDecl *D =
                getKCFIBoundaryDecl(ignoreKCFIPointerConversions(Init)))
          Sources.insert(cast<NamedDecl>(D->getCanonicalDecl()));
      auto Assigned = LocalSources.find(VD);
      if (Assigned != LocalSources.end())
        Sources.insert_range(Assigned->second);
      for (const NamedDecl *D : Sources) {
        const auto *Source = dyn_cast<VarDecl>(D);
        if (!Source || !isKCFILocalVariable(Source))
          BoundaryRecords[D].insert_range(Records);
        else if (Visited.insert(Source).second)
          Worklist.push_back(Source);
      }
    }
  }
}

void CodeGenKCFI::collectInflowTypes(
    const FunctionDecl *FD, bool Params, llvm::SetVector<KCFITypeId> &TypeIds,
    llvm::SetVector<const RecordDecl *> *Records,
    llvm::SetVector<const RecordDecl *> *Held) {
  llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
  if (Params) {
    for (const ParmVarDecl *Param : FD->parameters())
      collectKCFIHandedTypes(CGM, Param->getType(), Visited, TypeIds, Records);
    auto VAArgs = VAArgTypes.find(FD->getCanonicalDecl());
    if (VAArgs != VAArgTypes.end())
      for (const Type *T : VAArgs->second)
        collectKCFIHandedTypes(CGM, QualType(T, 0), Visited, TypeIds, Records);
    // The caller may call the functions of ours that FD hands back with
    // pointers to its own functions.
    llvm::SmallPtrSet<const RecordDecl *, 16> ResultVisited;
    collectKCFIResultTypes(CGM, FD->getType(), /*Handed=*/true, ResultVisited,
                           TypeIds, Records,
                           /*Callbacks=*/true);
    return;
  }

  collectKCFIResultTypes(CGM, FD->getType(), /*Handed=*/false, Visited, TypeIds,
                         Records);
  // So may FD call the functions of ours that a call hands it, in its
  // parameters or in the objects that the call passes untyped.
  collectKCFIParamTypes(CGM, FD->getType(), TypeIds, Held,
                        /*Callbacks=*/true);
  auto Untyped = UntypedArgTypes.find(FD->getCanonicalDecl());
  if (Untyped == UntypedArgTypes.end())
    return;
  llvm::SmallPtrSet<const RecordDecl *, 16> UntypedVisited;
  for (const Type *T : Untyped->second) {
    if (T->isFunctionType()) {
      collectKCFIParamTypes(CGM, QualType(T, 0), TypeIds, Held,
                            /*Callbacks=*/false);
      continue;
    }
    if (T->isFunctionPointerType())
      TypeIds.insert(createCallTypeIds(T->getPointeeType()));
    collectKCFIHeldTypes(CGM, QualType(T, 0), UntypedVisited, TypeIds, Held,
                         /*Callbacks=*/true);
  }
}

void CodeGenKCFI::addVAArgType(QualType T, const Decl *D) {
  if (!HasFacts)
    return;
  // A variadic function's callers hand it function pointers in its variadic
  // arguments as they do in its parameters, but a va_list that a function
  // was handed may come from any caller of whichever function started it.
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  if (FD && FD->isVariadic()) {
    VAArgTypes[FD->getCanonicalDecl()].insert(
        T.getCanonicalType().getTypePtr());
    return;
  }
  llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
  collectKCFIHandedTypes(CGM, T, Visited, DynamicTypes);
}

void CodeGenKCFI::addCalledType(QualType FnType) {
  if (HasFacts)
    CalledTypes.insert(FnType.getCanonicalType().getTypePtr());
}

void CodeGenKCFI::addCallArguments(
    const FunctionDecl *FD,
    llvm::iterator_range<CallExpr::const_arg_iterator> Args,
    unsigned NumParams) {
  if (!HasFacts)
    return;
  // Without builtins, a C library function still does what its name says.
  unsigned BuiltinID = FD->getBuiltinID();
  if (!BuiltinID)
    BuiltinID = FD->getMemoryFunctionKind();
  switch (BuiltinID) {
  case 0:
    break;
  case Builtin::BImemcpy:
  case Builtin::BI__builtin_memcpy:
  case Builtin::BI__builtin_memcpy_inline:
  case Builtin::BI__builtin___memcpy_chk:
  case Builtin::BImempcpy:
  case Builtin::BI__builtin_mempcpy:
  case Builtin::BImemmove:
  case Builtin::BI__builtin_memmove:
  case Builtin::BI__builtin___memmove_chk: {
    // A copy reinterprets its source as an object of its destination's type,
    // as a conversion of a pointer to it does.
    if (llvm::size(Args) < 2)
      return;
    const Expr *Dest = ignoreKCFIPointerConversions(*Args.begin());
    if (Dest->getType()->isPointerType())
      addBoundaryRecord(Dest->getType()->getPointeeType(),
                        ignoreKCFIPointerConversions(*++Args.begin()));
    return;
  }
  default:
    // None of the C library's functions stores a function pointer of its own
    // into memory that its caller hands it.
    return;
  }
  // A callee can store its own function pointers into an object that a
  // pointer in a variadic argument or an untyped pointer points to. Since
  // nothing says that the callee knows the object's type, as a typed pointer
  // parameter does, only the function pointers that the object holds count,
  // as for a record that the callee's result is converted to a pointer to.
  for (auto [I, Arg] : llvm::enumerate(Args)) {
    QualType ParamType = Arg->getType();
    if (I < NumParams && (!ParamType->isPointerType() ||
                          ParamType->getPointeeType().isConstQualified() ||
                          !(ParamType->getPointeeType()->isVoidType() ||
                            ParamType->getPointeeType()->isCharType())))
      continue;
    QualType T = ignoreKCFIPointerConversions(Arg)->getType();
    if (!T->isPointerType())
      continue;
    QualType Pointee = T->getPointeeType();
    // The callee may call a function pointer passed so as one passed in a
    // parameter of its type.
    if (Pointee->isFunctionType()) {
      UntypedArgTypes[FD->getCanonicalDecl()].insert(
          Pointee.getCanonicalType().getTypePtr());
      continue;
    }
    if (Pointee.isConstQualified())
      continue;
    QualType Object =
        CGM.getContext().getBaseElementType(Pointee).getCanonicalType();
    const RecordDecl *RD = Object->getAsRecordDecl();
    if (RD && (RD = RD->getDefinition()))
      BoundaryRecords[FD->getCanonicalDecl()].insert(RD);
    else if (!Object->isFunctionPointerType())
      continue;
    UntypedArgTypes[FD->getCanonicalDecl()].insert(Object.getTypePtr());
  }
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

namespace {
/// The link facts of one declaration: the KCFI types it reaches directly,
/// and the records through which it reaches others, those it reaches
/// through pointers from them and those they hold.
struct KCFILinkFacts {
  llvm::SetVector<KCFITypeId> TypeIds;
  llvm::SetVector<const RecordDecl *> Reached;
  llvm::SetVector<const RecordDecl *> Held;
};

/// A function type in a link fact: its check identifier, which the fact's
/// symbol carries as its value, and its precise identifier, which the symbol's
/// name carries so that the linker can propagate by it.
using KCFIFactType = std::pair<uint32_t, uint32_t>;

/// The key, in 16 hex digits, that names a precise type in link facts: its
/// precise identifier. A fact's symbol carries the precise key in its name and
/// the check identifier, which fits a COFF absolute, as its value, so that two
/// precise types that share a check identifier never name the same symbol.
static std::string kcfiPreciseName(uint32_t Precise) {
  return llvm::utohexstr(Precise, /*LowerCase=*/true, /*Width=*/16);
}

/// Emits link facts as weak constants, as the __kcfi_typeid_ constants are,
/// with every number in the name, so that the symbols of two objects never
/// disagree. A type fact <Prefix><precise>_<Name> has its check identifier,
/// which fits a COFF absolute symbol, as its value, and names the precise
/// identifier so that two types that share a check identifier each give their
/// own fact. The types of a record are a node, emitted once per object as
/// __kcfi_node_<node>_<precise> for each of its types, and referred to by each
/// fact <Prefix>n<node>_<Name>. A node is named by a hash of its types' check
/// identifiers, so equal nodes of two objects are the same symbols; a node of
/// one type is emitted as a type fact instead.
class KCFILinkFactEmitter {
  CodeGenModule &CGM;
  llvm::DenseMap<std::pair<const RecordDecl *, unsigned>,
                 std::pair<uint64_t, SmallVector<KCFIFactType, 4>>>
      Nodes;
  llvm::DenseSet<uint64_t> Emitted;

  void emit(StringRef Symbol, uint64_t Value) {
    CGM.getModule().appendModuleInlineAsm((".weak " + Twine(Symbol) +
                                           "\n.set " + Symbol + ", " +
                                           Twine(Value) + "\n")
                                              .str());
  }

  const std::pair<uint64_t, SmallVector<KCFIFactType, 4>> &
  getNode(const RecordDecl *RD, bool Reached) {
    auto [It, Inserted] = Nodes.try_emplace({RD, Reached});
    if (!Inserted)
      return It->second;
    llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
    llvm::SetVector<KCFITypeId> TypeIds;
    QualType T = CGM.getContext().getCanonicalTagType(RD);
    if (Reached)
      collectKCFIReachableTypes(CGM, T, Visited, TypeIds);
    else
      collectKCFIHeldTypes(CGM, T, Visited, TypeIds);
    SmallVector<KCFIFactType, 4> &Types = It->second.second;
    for (const auto &[TypeId, Precise] : TypeIds)
      Types.push_back({(uint32_t)TypeId->getZExtValue(), Precise});
    llvm::sort(Types);
    SmallVector<uint8_t, 16> Bytes;
    for (auto [Type, Precise] : Types)
      for (unsigned I = 0; I != 4; ++I)
        Bytes.push_back(Type >> (8 * I));
    It->second.first = llvm::xxh3_64bits(Bytes);
    return It->second;
  }

  llvm::DenseSet<uint64_t> Precise;

public:
  explicit KCFILinkFactEmitter(CodeGenModule &CGM) : CGM(CGM) {}

  // Publishes that this object opens the precise type P, whose check
  // identifier is H, dynamically, so the linker seeds its propagation with it.
  void emitPrecise(uint64_t P, uint32_t H) {
    if (Precise.insert(P).second)
      emit("__kcfi_popen_" + llvm::utohexstr(P, /*LowerCase=*/true,
                                             /*Width=*/16),
           H);
  }

  void emitFacts(StringRef Prefix, StringRef Name, const KCFILinkFacts &Facts) {
    if (!CodeGenModule::allowKCFIIdentifier(Name))
      return;
    // A type fact is keyed by its precise identifier, so two precise types that
    // share a check identifier each give their own fact and the linker opens
    // the right one; the value carries the check identifier.
    llvm::MapVector<uint64_t, uint32_t> Types;
    for (const auto &[TypeId, Precise] : Facts.TypeIds)
      Types.insert({Precise, uint32_t(TypeId->getZExtValue())});
    llvm::SetVector<const std::pair<uint64_t, SmallVector<KCFIFactType, 4>> *>
        FactNodes;
    for (bool Reached : {true, false})
      for (const RecordDecl *RD : Reached ? Facts.Reached : Facts.Held) {
        const auto &Node = getNode(RD, Reached);
        if (Node.second.size() == 1) {
          auto [Check, Precise] = Node.second.front();
          Types.insert({Precise, Check});
        } else if (!Node.second.empty()) {
          FactNodes.insert(&Node);
        }
      }

    for (auto [Key, Check] : Types)
      emit((Twine(Prefix) +
            llvm::utohexstr(Key, /*LowerCase=*/true, /*Width=*/16) + "_" + Name)
               .str(),
           Check);
    for (const auto *Node : FactNodes) {
      std::string Id =
          llvm::utohexstr(Node->first, /*LowerCase=*/true, /*Width=*/16);
      emit((Twine(Prefix) + "n" + Id + "_" + Name).str(), 0);
      if (!Emitted.insert(Node->first).second)
        continue;
      for (auto [Type, Precise] : Node->second)
        emit("__kcfi_node_" + Id + "_" + kcfiPreciseName(Precise), Type);
    }
  }
};
} // namespace

/// Whether GV, the global of declaration D, is a known import: dllimport, or
/// marked with an explicit default visibility that the visibility mapping
/// imports. A declaration that only -fno-plt imports may be in this image.
static bool isKCFIKnownImport(const CodeGenModule &CGM,
                              const llvm::GlobalValue &GV, const NamedDecl *D) {
  return GV.hasDLLImportStorageClass() &&
         (D->hasAttr<DLLImportAttr>() ||
          CGM.isMappedImportVisibility(D->getLinkageAndVisibility()));
}

void CodeGenKCFI::emitFacts() {
  llvm::Module &M = CGM.getModule();
  KCFILinkFactEmitter LinkFacts(CGM);
  resolveLocalBoundaryRecords();
  for (llvm::Function &F : M.functions()) {
    GlobalDecl GD;
    if (F.hasLocalLinkage() || !CGM.lookupRepresentativeDecl(F.getName(), GD))
      continue;
    const auto *FD = dyn_cast<FunctionDecl>(GD.getDecl());
    if (!FD)
      continue;
    FD = FD->getMostRecentDecl();

    KCFILinkFacts Facts;
    if (F.isDeclarationForLinker()) {
      if (F.use_empty())
        continue;
      // A known import is in another image, which also defines the functions
      // whose pointers a call to it hands back, also in the records that its
      // result is converted to a pointer to.
      auto Boundary = BoundaryRecords.find(FD->getCanonicalDecl());
      if (isKCFIKnownImport(CGM, F, FD)) {
        F.setMetadata("kcfi_import",
                      llvm::MDNode::get(CGM.getLLVMContext(), {}));
        collectInflowTypes(FD, /*Params=*/false, DynamicTypes);
        if (Boundary != BoundaryRecords.end())
          for (const RecordDecl *RD : Boundary->second) {
            llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
            collectKCFIHeldTypes(CGM, CGM.getContext().getCanonicalTagType(RD),
                                 Visited, DynamicTypes);
          }
      } else if (FD->isExternC()) {
        // A C function may be foreign code that the linker brings into the
        // image, which then opens these types.
        collectInflowTypes(FD, /*Params=*/false, Facts.TypeIds, &Facts.Reached,
                           &Facts.Held);
        if (Boundary != BoundaryRecords.end())
          Facts.Held.insert_range(Boundary->second);
        LinkFacts.emitFacts("__kcfi_inflow_", F.getName(), Facts);
      }
    } else if (F.hasDLLExportStorageClass()) {
      // Another image may call an exported function with pointers to its own
      // functions.
      collectInflowTypes(FD, /*Params=*/true, DynamicTypes);
    } else if (FD->isExternC()) {
      // Foreign code that the linker brings into the image may call a C
      // function with pointers to its own functions, and the linker then
      // opens these types.
      collectInflowTypes(FD, /*Params=*/true, Facts.TypeIds, &Facts.Held);
      LinkFacts.emitFacts("__kcfi_param_", F.getName(), Facts);
    }
  }

  // Variables hold function pointers that code in another image or foreign
  // code reads or writes in the same ways.
  for (llvm::GlobalVariable &GV : M.globals()) {
    GlobalDecl GD;
    if (GV.hasLocalLinkage() || !CGM.lookupRepresentativeDecl(GV.getName(), GD))
      continue;
    const auto *VD = dyn_cast<VarDecl>(GD.getDecl());
    if (!VD)
      continue;
    VD = VD->getMostRecentDecl();

    llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
    KCFILinkFacts Facts;
    if (GV.isDeclarationForLinker()) {
      if (GV.use_empty())
        continue;
      if (isKCFIKnownImport(CGM, GV, VD)) {
        // Imported data holds the other image's function pointers, also in
        // the records that a value loaded from it is converted to a pointer
        // to.
        collectKCFIReachableTypes(CGM, VD->getType(), Visited, DynamicTypes);
        auto Boundary = BoundaryRecords.find(VD->getCanonicalDecl());
        if (Boundary != BoundaryRecords.end())
          for (const RecordDecl *RD : Boundary->second)
            collectKCFIHeldTypes(CGM, CGM.getContext().getCanonicalTagType(RD),
                                 Visited, DynamicTypes);
      } else if (VD->isExternC()) {
        // A C variable may be defined in foreign code, or be imported data.
        collectKCFIReachableTypes(CGM, VD->getType(), Visited, Facts.TypeIds,
                                  &Facts.Reached);
        auto Boundary = BoundaryRecords.find(VD->getCanonicalDecl());
        if (Boundary != BoundaryRecords.end())
          Facts.Held.insert_range(Boundary->second);
        LinkFacts.emitFacts("__kcfi_inflow_", GV.getName(), Facts);
      }
    } else if (GV.hasDLLExportStorageClass()) {
      // Another image may store pointers to its own functions into an
      // exported variable.
      collectKCFIHandedTypes(CGM, VD->getType(), Visited, DynamicTypes);
    } else if (VD->isExternC()) {
      // So may foreign code into a C variable.
      collectKCFIHandedTypes(CGM, VD->getType(), Visited, Facts.TypeIds,
                             &Facts.Held);
      LinkFacts.emitFacts("__kcfi_param_", GV.getName(), Facts);
    }
  }

  // A call through a pointer that may reach a function without a prefix of
  // ours hands back the function pointers held in what it returns and in what
  // it stores through its out-parameters, and those may be called in turn.
  // The callee may also call the functions of ours that the call hands it
  // with pointers to its own functions.
  // A cast between two of our own function pointer types also opens a type,
  // so the callee is not assumed to write into every object that a pointer
  // parameter reaches, as a known import, which is foreign code, is.
  SmallVector<std::pair<KCFITypeId, QualType>> Called;
  for (const Type *T : CalledTypes)
    Called.emplace_back(createCallTypeIds(QualType(T, 0)), QualType(T, 0));
  // A call through a vtable or a member function pointer checks a type of its
  // own, which the class it is made through opened.
  for (auto [TypeId, T] : DynamicCalls)
    Called.emplace_back(TypeId, QualType(T, 0));
  // A call propagates only when the call's own precise type is open: two types
  // that share a check identifier, such as a dlsym cast and an unrelated call,
  // open independently.
  llvm::DenseSet<uint32_t> OpenedPrecise;
  unsigned Scanned = 0;
  auto refreshOpened = [&] {
    for (; Scanned < DynamicTypes.size(); ++Scanned)
      OpenedPrecise.insert(DynamicTypes[Scanned].second);
  };
  auto isOpened = [&](KCFITypeId TypeId) {
    return OpenedPrecise.contains(TypeId.second);
  };
  refreshOpened();
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[TypeId, FnType] : Called) {
      if (FnType.isNull() || !isOpened(TypeId))
        continue;
      llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
      collectKCFIResultTypes(CGM, FnType, /*Handed=*/true, Visited,
                             DynamicTypes);
      collectKCFIParamTypes(CGM, FnType, DynamicTypes, nullptr,
                            /*Callbacks=*/true);
      FnType = QualType();
      Changed = true;
    }
    refreshOpened();
  }
  // The linker opens the types of a call through a pointer of a type that
  // this object does not open, when it opens that type. The facts are keyed by
  // the called type's precise key, which the linker propagates by.
  llvm::MapVector<uint64_t, KCFILinkFacts> CalledFacts;
  for (auto &[TypeId, FnType] : Called) {
    if (FnType.isNull())
      continue;
    llvm::SmallPtrSet<const RecordDecl *, 16> Visited;
    KCFILinkFacts &Facts = CalledFacts[TypeId.second];
    collectKCFIResultTypes(CGM, FnType, /*Handed=*/true, Visited, Facts.TypeIds,
                           &Facts.Held);
    collectKCFIParamTypes(CGM, FnType, Facts.TypeIds, &Facts.Held,
                          /*Callbacks=*/true);
  }
  for (auto &[Key, Facts] : CalledFacts)
    LinkFacts.emitFacts(
        "__kcfi_tinflow_",
        llvm::utohexstr(Key, /*LowerCase=*/true, /*Width=*/16), Facts);

  if (DynamicTypes.empty())
    return;
  // The precise types this object opens dynamically, so that the linker can
  // follow a call through one of them into the types it hands back.
  for (auto TypeId : DynamicTypes)
    LinkFacts.emitPrecise(TypeId.second,
                          (uint32_t)TypeId.first->getZExtValue());
  // The routines are keyed by the check identifier alone, so each is listed
  // once however many precise types open it.
  llvm::NamedMDNode *Dynamic = M.getOrInsertNamedMetadata("kcfi.dynamic");
  llvm::SmallPtrSet<llvm::ConstantInt *, 32> Listed;
  for (auto [TypeId, Precise] : DynamicTypes)
    if (Listed.insert(TypeId).second)
      Dynamic->addOperand(llvm::MDNode::get(
          CGM.getLLVMContext(), llvm::ConstantAsMetadata::get(TypeId)));
}
