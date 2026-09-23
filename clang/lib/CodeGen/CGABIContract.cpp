//===- CGABIContract.cpp - Physical ABI requirement emission --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CGABIContract.h"
#include "CGCXXABI.h"
#include "CodeGenModule.h"
#include "CodeGenTypes.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/ABIContract.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::CodeGen;
using llvm::abi::Kind;
using llvm::abi::Role;

namespace {
class ContractBuilder {
  CodeGenModule &CGM;
  ASTContext &Context;
  llvm::abi::Contract Contract;
  llvm::DenseMap<std::pair<QualType, unsigned>, uint32_t> Types;
  llvm::DenseMap<llvm::Type *, uint32_t> MachineTypes;
  llvm::DenseMap<GlobalDecl, uint32_t> Methods;

  uint32_t node(Kind K, llvm::ArrayRef<uint64_t> Properties = {}) {
    uint32_t ID = Contract.Nodes.size();
    llvm::abi::Node N;
    N.Category = K;
    N.Properties.append(Properties.begin(), Properties.end());
    Contract.Nodes.push_back(std::move(N));
    return ID;
  }

  void edge(uint32_t From, Role Relation, uint32_t To,
            llvm::ArrayRef<uint64_t> Properties = {}) {
    llvm::abi::Edge E{Relation, To, {}};
    E.Properties.append(Properties.begin(), Properties.end());
    Contract.Nodes[From].Edges.push_back(std::move(E));
  }

  std::string identity(QualType T) {
    std::string Name;
    llvm::raw_string_ostream OS(Name);
    CGM.getCXXABI().getMangleContext().mangleCXXRTTIName(T, OS);
    return Name;
  }

  // Machine kinds are schema values, not LLVM TypeID values. Struct element
  // offsets come from DataLayout, including packed and target ABI coercions.
  uint32_t machineType(llvm::Type *T) {
    auto [It, Inserted] = MachineTypes.try_emplace(T, Contract.Nodes.size());
    if (!Inserted)
      return It->second;
    uint32_t ID = node(Kind::MachineType, {0, 0, 0, 0, 0});
    uint64_t K = 0, Count = 0, Flags = 0;
    if (T->isVoidTy())
      K = 0;
    else if (T->isIntegerTy()) {
      K = 1;
      Count = T->getIntegerBitWidth();
    } else if (T->isPointerTy()) {
      K = 2;
      Flags = T->getPointerAddressSpace();
    } else if (T->isHalfTy())
      K = 3;
    else if (T->isBFloatTy())
      K = 4;
    else if (T->isFloatTy())
      K = 5;
    else if (T->isDoubleTy())
      K = 6;
    else if (T->isX86_FP80Ty())
      K = 7;
    else if (T->isFP128Ty())
      K = 8;
    else if (T->isPPC_FP128Ty())
      K = 9;
    else if (auto *A = llvm::dyn_cast<llvm::ArrayType>(T)) {
      K = 10;
      Count = A->getNumElements();
      edge(ID, Role::Element, machineType(A->getElementType()));
    } else if (auto *V = llvm::dyn_cast<llvm::VectorType>(T)) {
      K = 11;
      Count = V->getElementCount().getKnownMinValue();
      Flags = V->getElementCount().isScalable();
      edge(ID, Role::Element, machineType(V->getElementType()));
    } else if (auto *S = llvm::dyn_cast<llvm::StructType>(T)) {
      K = 12;
      Count = S->getNumElements();
      Flags = S->isPacked();
      const llvm::StructLayout *Layout = CGM.getDataLayout().getStructLayout(S);
      for (unsigned I = 0; I != Count; ++I)
        edge(ID, Role::Field, machineType(S->getElementType(I)),
             {I, Layout->getElementOffsetInBits(I), UINT64_MAX, 0});
    } else
      llvm::reportFatalUsageError(
          "unsupported machine type in COFF ABI contract");
    uint64_t Size = 0, Align = 0;
    if (T->isSized()) {
      Size = CGM.getDataLayout().getTypeSizeInBits(T).getKnownMinValue();
      Align = CGM.getDataLayout().getABITypeAlign(T).value() * 8;
    }
    Contract.Nodes[ID].Properties = {K, Size, Align, Count, Flags};
    return ID;
  }

  uint32_t argument(const ABIArgInfo &A) {
    uint64_t K, Offset = 0, Align = 0, AS = 0, Flags = 0, Index = 0;
    switch (A.getKind()) {
    case ABIArgInfo::Direct:
      K = 0;
      break;
    case ABIArgInfo::Extend:
      K = 1;
      break;
    case ABIArgInfo::Indirect:
      K = 2;
      break;
    case ABIArgInfo::IndirectAliased:
      K = 3;
      break;
    case ABIArgInfo::Ignore:
      K = 4;
      break;
    case ABIArgInfo::Expand:
      K = 5;
      break;
    case ABIArgInfo::CoerceAndExpand:
      K = 6;
      break;
    case ABIArgInfo::TargetSpecific:
      K = 7;
      break;
    case ABIArgInfo::InAlloca:
      K = 8;
      break;
    }
    if (A.isDirect() || A.isExtend() || A.isTargetSpecific()) {
      Offset = A.getDirectOffset();
      Align = A.getDirectAlign();
      Flags |= uint64_t(A.getInReg());
    }
    if (A.isDirect())
      Flags |= uint64_t(A.getCanBeFlattened()) << 1;
    if (A.isExtend())
      Flags |= uint64_t(A.isSignExt()) << 2 | uint64_t(A.isZeroExt()) << 3;
    if (A.isIndirect() || A.isIndirectAliased()) {
      Align = A.getIndirectAlign().getQuantity();
      AS = A.getIndirectAddrSpace();
      Flags |= uint64_t(A.getIndirectRealign()) << 4;
    }
    if (A.isIndirect())
      Flags |= uint64_t(A.getInReg()) | uint64_t(A.getIndirectByVal()) << 5 |
               uint64_t(A.isSRetAfterThis()) << 6;
    if (A.isInAlloca()) {
      Index = A.getInAllocaFieldIndex();
      Flags |= uint64_t(A.getInAllocaIndirect()) << 7 |
               uint64_t(A.getInAllocaSRet()) << 8;
    }
    uint32_t ID = node(Kind::Argument, {K, Offset, Align, AS, Flags, Index,
                                        uint64_t(A.getPaddingInReg()), 0, 0});
    if (A.canHaveCoerceToType())
      if (llvm::Type *T = A.getCoerceToType())
        edge(ID, Role::Coercion, machineType(T));
    if (llvm::Type *T = A.getPaddingType())
      edge(ID, Role::Padding, machineType(T));
    if (A.isCoerceAndExpand())
      edge(ID, Role::Expansion,
           machineType(A.getUnpaddedCoerceAndExpandType()));
    return ID;
  }

  void function(uint32_t ID, const CGFunctionInfo &FI) {
    uint64_t Flags = uint64_t(FI.isChainCall()) |
                     uint64_t(FI.isDelegateCall()) << 1 |
                     uint64_t(FI.isCmseNSCall()) << 2 |
                     uint64_t(FI.isReturnsRetained()) << 3 |
                     uint64_t(FI.isNoCallerSavedRegs()) << 4;
    Contract.Nodes[ID].Category = Kind::Function;
    Contract.Nodes[ID].Properties = {
        FI.getEffectiveCallingConvention(),
        FI.getNumRequiredArgs(),
        uint64_t(FI.isVariadic()),
        uint64_t(FI.isInstanceMethod()),
        FI.getHasRegParm() ? FI.getRegParm() + 1U : 0U,
        Flags,
        FI.usesInAlloca() ? uint64_t(FI.getArgStructAlignment().getQuantity())
                          : 0,
        FI.getMaxVectorWidth()};
    edge(ID, Role::Result, type(FI.getReturnType()));
    edge(ID, Role::ResultABI, argument(FI.getReturnInfo()));
    unsigned I = 0;
    for (const auto &A : FI.arguments()) {
      auto E = FI.getExtParameterInfo(I);
      uint64_t ABI;
      switch (E.getABI()) {
      case ParameterABI::Ordinary:
        ABI = 0;
        break;
      case ParameterABI::SwiftIndirectResult:
        ABI = 1;
        break;
      case ParameterABI::SwiftErrorResult:
        ABI = 2;
        break;
      case ParameterABI::SwiftContext:
        ABI = 3;
        break;
      case ParameterABI::SwiftAsyncContext:
        ABI = 4;
        break;
      case ParameterABI::HLSLOut:
        ABI = 5;
        break;
      case ParameterABI::HLSLInOut:
        ABI = 6;
        break;
      }
      edge(ID, Role::Parameter, type(A.type), {I});
      edge(ID, Role::ParameterABI, argument(A.info),
           {I, ABI,
            uint64_t(E.isConsumed()) | uint64_t(E.hasPassObjectSize()) << 1});
      ++I;
    }
    if (FI.usesInAlloca())
      edge(ID, Role::ArgumentFrame, machineType(FI.getArgStruct()));
  }

  uint32_t method(GlobalDecl GD) {
    GD = GD.getCanonicalDecl();
    auto [It, Inserted] = Methods.try_emplace(GD, Contract.Nodes.size());
    if (!Inserted)
      return It->second;
    uint32_t ID = node(Kind::Function);
    Contract.Nodes[ID].Identity = CGM.getMangledName(GD).str();
    function(ID, CGM.getTypes().arrangeGlobalDeclaration(GD));
    return ID;
  }

  void vtable(uint32_t ID, const CXXRecordDecl *RD) {
    const VTableLayout &Layout =
        CGM.getItaniumVTableContext().getVTableLayout(RD);
    uint32_t Table =
        node(Kind::VTable, {uint64_t(CGM.getLangOpts().RelativeCXXABIVTables)});
    edge(ID, Role::VTable, Table);
    unsigned I = 0;
    for (const VTableComponent &C : Layout.vtable_components()) {
      uint64_t K;
      int64_t Offset = 0;
      uint32_t Target = ID;
      switch (C.getKind()) {
      case VTableComponent::CK_VCallOffset:
        K = 0;
        Offset = C.getVCallOffset().getQuantity();
        break;
      case VTableComponent::CK_VBaseOffset:
        K = 1;
        Offset = C.getVBaseOffset().getQuantity();
        break;
      case VTableComponent::CK_OffsetToTop:
        K = 2;
        Offset = C.getOffsetToTop().getQuantity();
        break;
      case VTableComponent::CK_RTTI:
        K = 3;
        break;
      case VTableComponent::CK_FunctionPointer:
        K = 4;
        Target = method(C.getGlobalDecl(false));
        break;
      case VTableComponent::CK_CompleteDtorPointer:
        K = 5;
        Target = method(C.getGlobalDecl(false));
        break;
      case VTableComponent::CK_DeletingDtorPointer:
        K = 6;
        Target = method(C.getGlobalDecl(false));
        break;
      case VTableComponent::CK_UnusedFunctionPointer:
        K = 7;
        Target = method(GlobalDecl(C.getUnusedFunctionDecl()));
        break;
      }
      edge(Table, Role::VTableEntry, Target, {I++, K, uint64_t(Offset)});
    }
    // Base order is defined by the language layout, unlike DenseMap iteration.
    // Visit the complete object and all base subobjects in layout order below.
    for (unsigned V = 0; V != Layout.getNumVTables(); ++V)
      edge(Table, Role::AddressPoint, ID,
           {Layout.getVTableOffset(V), V, Layout.getAddressPointIndices()[V]});
    for (const auto &[Slot, T] : Layout.vtable_thunks())
      edge(Table, Role::Thunk, ID,
           {Slot, uint64_t(T.This.NonVirtual),
            uint64_t(T.This.Virtual.Itanium.VCallOffsetOffset),
            uint64_t(T.Return.NonVirtual),
            uint64_t(T.Return.Virtual.Itanium.VBaseOffsetOffset)});
  }

  void record(uint32_t ID, const RecordDecl *RD) {
    RD = RD->getDefinition();
    assert(RD && "complete ABI record has no definition");
    const ASTRecordLayout &L = Context.getASTRecordLayout(RD);
    auto *CXX = dyn_cast<CXXRecordDecl>(RD);
    uint64_t Flags = uint64_t(RD->isUnion());
    if (CXX)
      Flags |= uint64_t(CXX->isDynamicClass()) << 1 |
               uint64_t(CXX->canPassInRegisters()) << 2 |
               uint64_t(CXX->isParamDestroyedInCallee()) << 3;
    Contract.Nodes[ID].Category = Kind::Record;
    Contract.Nodes[ID].Properties = {
        uint64_t(L.getSize().getQuantity()),
        uint64_t(L.getAlignment().getQuantity()),
        uint64_t(L.getDataSize().getQuantity()),
        uint64_t(CXX ? L.getNonVirtualSize().getQuantity()
                     : L.getSize().getQuantity()),
        uint64_t(CXX ? L.getNonVirtualAlignment().getQuantity()
                     : L.getAlignment().getQuantity()),
        Flags};
    unsigned I = 0;
    for (const FieldDecl *F : RD->fields()) {
      uint64_t Width = F->isBitField() ? F->getBitWidthValue() : UINT64_MAX;
      edge(ID, Role::Field, type(F->getType()),
           {I, L.getFieldOffset(I), Width,
            uint64_t(F->hasAttr<NoUniqueAddressAttr>())});
      ++I;
    }
    if (!CXX)
      return;
    for (const CXXBaseSpecifier &B : CXX->bases()) {
      auto *Base = B.getType()->getAsCXXRecordDecl();
      CharUnits Offset = B.isVirtual() ? L.getVBaseClassOffset(Base)
                                       : L.getBaseClassOffset(Base);
      // Access is an RTTI traversal property; encode its meaning, not AS_*.
      uint64_t Access = B.getAccessSpecifier() == AS_public      ? 0
                        : B.getAccessSpecifier() == AS_protected ? 1
                                                                 : 2;
      edge(ID, Role::Base, type(B.getType()),
           {uint64_t(Offset.getQuantity()), uint64_t(B.isVirtual()), Access});
    }
    if (const CXXRecordDecl *Base = L.getPrimaryBase())
      edge(ID, Role::PrimaryBase, type(Context.getCanonicalTagType(Base)),
           {uint64_t(L.isPrimaryBaseVirtual())});
    for (const CXXBaseSpecifier &B : CXX->vbases())
      edge(ID, Role::VirtualBase, type(B.getType()),
           {uint64_t(L.getVBaseClassOffset(B.getType()->getAsCXXRecordDecl())
                         .getQuantity())});
    if (CXX->isDynamicClass())
      vtable(ID, CXX);
  }

public:
  explicit ContractBuilder(CodeGenModule &CGM)
      : CGM(CGM), Context(CGM.getContext()) {}

  uint32_t type(QualType T, bool Complete = true) {
    T = Context.getCanonicalType(T);
    if (T->isIncompleteType() || T->isVoidType())
      Complete = false;
    auto [It, Inserted] =
        Types.try_emplace(std::make_pair(T, Complete), Contract.Nodes.size());
    if (!Inserted)
      return It->second;
    uint32_t ID = node(Kind::Identity);
    Contract.Nodes[ID].Identity = identity(T);
    Contract.Nodes[ID].Qualifiers = T.getCVRQualifiers();
    if (!Complete)
      return ID;
    if (const auto *R = T->getAs<RecordType>()) {
      record(ID, R->getDecl());
      return ID;
    }
    if (const auto *F = T->getAs<FunctionProtoType>()) {
      function(ID, CGM.getTypes().arrangeFreeFunctionType(
                       Context.getCanonicalType(QualType(F, 0))
                           .castAs<FunctionProtoType>()));
      return ID;
    }
    if (const auto *F = T->getAs<FunctionNoProtoType>()) {
      function(ID, CGM.getTypes().arrangeFreeFunctionType(
                       Context.getCanonicalType(QualType(F, 0))
                           .castAs<FunctionNoProtoType>()));
      return ID;
    }
    TypeInfo Info = Context.getTypeInfo(T);
    Kind K = Kind::Scalar;
    llvm::SmallVector<uint64_t, 6> Properties{Info.Width, Info.Align};
    if (const auto *P = T->getAs<PointerType>()) {
      K = Kind::Pointer;
      Properties.push_back(
          Context.getTargetAddressSpace(P->getPointeeType().getAddressSpace()));
      // Pointing at a class does not depend on that class's physical layout.
      // Function pointers do depend on the lowering of calls through them.
      edge(ID, Role::Pointee,
           type(P->getPointeeType(), P->getPointeeType()->isFunctionType()));
    } else if (const auto *R = T->getAs<ReferenceType>()) {
      K = Kind::Reference;
      Properties.push_back(T->isRValueReferenceType());
      edge(ID, Role::Pointee, type(R->getPointeeType(), false));
    } else if (const auto *P = T->getAs<MemberPointerType>()) {
      K = Kind::MemberPointer;
      Properties.push_back(P->isMemberFunctionPointer());
      edge(ID, Role::Class,
           type(Context.getCanonicalTagType(P->getMostRecentCXXRecordDecl()),
                false));
      edge(ID, Role::Pointee,
           type(P->getPointeeType(), P->isMemberFunctionPointer()));
    } else if (const auto *A = Context.getAsConstantArrayType(T)) {
      K = Kind::Array;
      Properties.push_back(A->getSize().getZExtValue());
      edge(ID, Role::Element, type(A->getElementType()));
    } else if (const auto *V = T->getAs<VectorType>()) {
      K = Kind::Vector;
      Properties.append({V->getNumElements(), uint64_t(T->isExtVectorType())});
      edge(ID, Role::Element, type(V->getElementType()));
    } else if (const auto *E = T->getAs<EnumType>()) {
      K = Kind::Enumeration;
      edge(ID, Role::Underlying, type(E->getDecl()->getIntegerType()));
    } else if (const auto *C = T->getAs<ComplexType>()) {
      K = Kind::Complex;
      edge(ID, Role::Element, type(C->getElementType()));
    } else if (const auto *A = T->getAs<AtomicType>()) {
      K = Kind::Atomic;
      edge(ID, Role::Element, type(A->getValueType()));
    }
    Contract.Nodes[ID].Category = K;
    Contract.Nodes[ID].Properties = std::move(Properties);
    return ID;
  }

  std::string build(QualType T, bool IsName) {
    if (IsName) {
      std::string Name = identity(T);
      uint32_t ID = node(
          Kind::Name, {Name.size() - 4 + 1, uint64_t(Context.getCharWidth())});
      Contract.Nodes[ID].Identity = std::move(Name);
    } else
      type(T);
    std::string Bytes = Contract.encode();
    if (Bytes.empty())
      llvm::reportFatalUsageError("non-canonical physical ABI graph");
    return Bytes;
  }
};
} // namespace

void clang::CodeGen::setTypeABIContract(CodeGenModule &CGM,
                                        llvm::GlobalObject &Object, QualType T,
                                        bool IsName) {
  std::string Bytes = ContractBuilder(CGM).build(T, IsName);
  Object.setMetadata(
      "coff.abi",
      llvm::MDNode::get(CGM.getLLVMContext(),
                        llvm::MDString::get(CGM.getLLVMContext(), Bytes)));
}
