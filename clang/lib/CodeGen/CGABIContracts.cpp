//===--- CGABIContracts.cpp - ABI contracts between images ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A contract is the layout of a type that code in one image depends on and
// another image's code was compiled with: a class's size, alignment, bases,
// fields and vtable, or the type of a datum that is not a class. Its digest
// covers its canonical form, so two images that agree on a digest agree on
// the layout. A module provides the contracts of the types its exported
// definitions are about, and requires those of the types its references to
// other images' symbols use the layout of; the linker checks the two against
// each other.
//
//===----------------------------------------------------------------------===//

#include "CGABIContracts.h"
#include "CGCXXABI.h"
#include "CodeGenModule.h"
#include "CodeGenTypes.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace CodeGen;

// The version of the canonical form of a contract, which every digest covers:
// a change to the form, or to what it records, takes a new version.
static constexpr unsigned ContractSchemaVersion = 1;

// The version of the rule that places vtables and of the representation of
// type_info objects, which every digest also covers, so that an image built
// under another rule fails to load against one whose types it requires.
static constexpr unsigned LayoutVersion = 1;

CodeGenABIContracts::CodeGenABIContracts(CodeGenModule &CGM)
    : CGM(CGM), Context(CGM.getContext()),
      Mangler(CGM.getCXXABI().getMangleContext()) {}

void CodeGenABIContracts::addVTableImport(llvm::GlobalValue *GV,
                                          const CXXRecordDecl *RD) {
  VTableImports.push_back({GV, RD});
}

void CodeGenABIContracts::addTypeInfo(llvm::GlobalValue *GV,
                                      const CXXRecordDecl *RD) {
  TypeInfos.push_back({GV, RD});
}

// Whether the contract of RD, a definition, is checked between images: that
// of a C++ class with a name by which other images know it, outside an
// extern "C" context, or under -fabi-witness-c-types that of a C struct at file
// scope. A class marked no_abi_witness is exempt.
bool CodeGenABIContracts::isContracted(const RecordDecl *RD) const {
  if (RD->isInvalidDecl() || RD->hasAttr<NoABIWitnessAttr>() ||
      (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl()))
    return false;
  if (Context.getLangOpts().CPlusPlus &&
      !RD->getDeclContext()->isExternCContext())
    return RD->isExternallyVisible();
  return CGM.getCodeGenOpts().ABIWitnessCTypes &&
         RD->getDeclContext()->getRedeclContext()->isFileContext();
}

bool CodeGenABIContracts::isLayoutUsed(const RecordDecl *RD) const {
  return UsedLayouts.contains(RD);
}

// The definition of the record that T names once pointers, references, arrays
// and qualifiers are stripped, if any.
static const RecordDecl *getStrippedRecord(QualType T) {
  while (true) {
    T = T.getCanonicalType();
    if (T->isPointerType() || T->isReferenceType() || T->isBlockPointerType())
      T = T->getPointeeType();
    else if (const ArrayType *AT = T->getAsArrayTypeUnsafe())
      T = AT->getElementType();
    else
      break;
  }
  const RecordDecl *RD = T->getAsRecordDecl();
  return RD ? RD->getDefinition() : nullptr;
}

// Adds the records that the contracts of D are about: the class D is a member
// of, and the records that its signature, or the type of the datum, names.
void CodeGenABIContracts::addRelatedRecords(const NamedDecl *D,
                                            RecordSet &Records) const {
  auto Add = [&](QualType T) {
    if (const RecordDecl *RD = getStrippedRecord(T))
      Records.insert(RD);
  };
  if (const auto *MD = dyn_cast<CXXMethodDecl>(D))
    if (const RecordDecl *RD = MD->getParent()->getDefinition())
      Records.insert(RD);
  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    Add(FD->getReturnType());
    for (const ParmVarDecl *P : FD->parameters())
      Add(P->getType());
  } else if (const auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->isStaticDataMember())
      if (const RecordDecl *RD =
              cast<RecordDecl>(VD->getDeclContext())->getDefinition())
        Records.insert(RD);
    Add(VD->getType());
  }
}

// The records that a pointer or reference in the layout of RD points to: in
// its own fields, and in those of its bases and of the records it holds by
// value.
const CodeGenABIContracts::RecordSet &
CodeGenABIContracts::getPointees(const RecordDecl *RD) {
  auto [It, Inserted] = RecordPointees.try_emplace(RD);
  if (!Inserted)
    return It->second;
  RecordSet Pointees;
  llvm::SmallPtrSet<const RecordDecl *, 8> Seen;
  SmallVector<const RecordDecl *, 8> Worklist = {RD};
  while (!Worklist.empty()) {
    const RecordDecl *R = Worklist.pop_back_val();
    if (!Seen.insert(R).second)
      continue;
    if (const auto *CRD = dyn_cast<CXXRecordDecl>(R))
      for (const CXXBaseSpecifier &B : CRD->bases())
        if (const RecordDecl *Base = B.getType()->getAsRecordDecl())
          if (const RecordDecl *Def = Base->getDefinition())
            Worklist.push_back(Def);
    for (const FieldDecl *FD : R->fields()) {
      QualType T = Context.getBaseElementType(FD->getType());
      if (T->isPointerType() || T->isReferenceType()) {
        QualType Pointee = Context.getBaseElementType(T->getPointeeType());
        if (const RecordDecl *P = Pointee->getAsRecordDecl())
          if (const RecordDecl *Def = P->getDefinition())
            Pointees.insert(Def);
      } else if (const RecordDecl *Member = T->getAsRecordDecl()) {
        if (const RecordDecl *Def = Member->getDefinition())
          Worklist.push_back(Def);
      }
    }
  }
  // The walk adds no entry to the map, so It is still valid.
  return It->second = std::move(Pointees);
}

// Adds the records of Related whose contracts are checked, only those whose
// layout the module's code uses if UsedOnly, together with the records that a
// pointer or reference in their layouts points to, one level deep, whose
// layout the module's code uses: an image vouches for such a record only if
// its own code used the layout, and requires it only if its code does.
void CodeGenABIContracts::addCoveredRecords(const RecordSet &Related,
                                            bool UsedOnly, RecordSet &Covered) {
  RecordSet Direct;
  for (const RecordDecl *RD : Related)
    if (isContracted(RD) && (!UsedOnly || isLayoutUsed(RD)))
      Direct.insert(RD);
  for (const RecordDecl *RD : Direct) {
    Covered.insert(RD);
    for (const RecordDecl *P : getPointees(RD))
      if (isContracted(P) && isLayoutUsed(P))
        Covered.insert(P);
  }
}

// Whether the type of the datum VD, which is not a record, has a contract:
// that of C++ data, or under -fabi-witness-c-types that of extern "C" data,
// when the type is complete.
bool CodeGenABIContracts::hasDataContract(const VarDecl *VD) const {
  QualType T = VD->getType();
  if (T->isRecordType() || T->isIncompleteType())
    return false;
  return !VD->isExternC() || CGM.getCodeGenOpts().ABIWitnessCTypes;
}

// The identity of a record's contract: its Itanium mangling, as the name of
// its type_info object spells it, which C structs take as C++ classes do.
std::string CodeGenABIContracts::getIdentity(const RecordDecl *RD) const {
  std::string Name;
  llvm::raw_string_ostream OS(Name);
  Mangler.mangleCXXRTTIName(Context.getCanonicalTagType(RD), OS);
  // The mangled name of the name object starts with _ZTS, after which it
  // spells the type.
  return Name.substr(4);
}

CodeGenABIContracts::Digest
CodeGenABIContracts::finishDigest(StringRef Identity,
                                  StringRef Contract) const {
  std::string Input;
  llvm::raw_string_ostream OS(Input);
  encodeULEB128(ContractSchemaVersion, OS);
  encodeULEB128(LayoutVersion, OS);
  encodeULEB128(Identity.size(), OS);
  OS << Identity << Contract;
  std::array<uint8_t, 32> Hash =
      llvm::SHA256::hash(llvm::arrayRefFromStringRef(Input));
  Digest D;
  std::copy_n(Hash.begin(), D.size(), D.begin());
  return D;
}

CodeGenABIContracts::Digest
CodeGenABIContracts::getRecordDigest(const RecordDecl *RD) {
  auto It = RecordDigests.find(RD);
  if (It != RecordDigests.end())
    return It->second;
  std::string Contract;
  llvm::raw_string_ostream OS(Contract);
  encodeRecord(RD, OS);
  Digest D = finishDigest(getIdentity(RD), Contract);
  RecordDigests[RD] = D;
  return D;
}

CodeGenABIContracts::Digest
CodeGenABIContracts::getDataDigest(const VarDecl *VD, StringRef Identity) {
  std::string Contract;
  llvm::raw_string_ostream OS(Contract);
  encodeType(VD->getType(), OS);
  return finishDigest(Identity, Contract);
}

// The format of a floating-point type, which the contract records rather
// than the type's spelling.
static uint8_t getFloatFormat(const llvm::fltSemantics &S) {
  if (&S == &llvm::APFloat::IEEEhalf())
    return 1;
  if (&S == &llvm::APFloat::BFloat())
    return 2;
  if (&S == &llvm::APFloat::IEEEsingle())
    return 3;
  if (&S == &llvm::APFloat::IEEEdouble())
    return 4;
  if (&S == &llvm::APFloat::x87DoubleExtended())
    return 5;
  if (&S == &llvm::APFloat::IEEEquad())
    return 6;
  if (&S == &llvm::APFloat::PPCDoubleDouble())
    return 7;
  return 0;
}

// Encodes the layout of T as a field or a datum has it. A scalar is its kind,
// size and alignment in bits; a pointer or reference is only that, never its
// pointee; a record held by value is its digest, or its own layout if it has
// no name, or its size and alignment alone if no_abi_witness exempts it.
void CodeGenABIContracts::encodeType(QualType T, llvm::raw_ostream &OS) {
  T = Context.getCanonicalType(T).getUnqualifiedType();
  auto EncodeSize = [&] {
    TypeInfo TI = Context.getTypeInfo(T);
    encodeULEB128(TI.Width, OS);
    encodeULEB128(TI.Align, OS);
  };
  if (const auto *AT = Context.getAsConstantArrayType(T)) {
    OS << 'a';
    encodeULEB128(AT->getZExtSize(), OS);
    encodeType(AT->getElementType(), OS);
    return;
  }
  if (const auto *AT = Context.getAsIncompleteArrayType(T)) {
    OS << 'x';
    encodeType(AT->getElementType(), OS);
    return;
  }
  if (const RecordDecl *RD = T->getAsRecordDecl()) {
    const RecordDecl *Def = RD->getDefinition();
    if (!Def || Def->hasAttr<NoABIWitnessAttr>()) {
      OS << 'o';
      EncodeSize();
    } else if (!Def->getIdentifier() && !Def->getTypedefNameForAnonDecl()) {
      encodeRecord(Def, OS);
    } else {
      Digest D = getRecordDigest(Def);
      OS << 'r' << llvm::toStringRef(llvm::ArrayRef(D));
    }
    return;
  }
  // An enumeration is its underlying type, which a declaration with a fixed
  // one gives as its definition does.
  if (const auto *ED = T->getAsEnumDecl()) {
    const EnumDecl *Def = ED->getDefinition();
    if (QualType Int = (Def ? Def : ED)->getIntegerType(); !Int.isNull())
      T = Context.getCanonicalType(Int);
  }
  if (T->isBooleanType()) {
    OS << 'b';
    EncodeSize();
  } else if (const auto *BT = T->getAs<BitIntType>()) {
    OS << (BT->isSigned() ? 'n' : 'N');
    encodeULEB128(BT->getNumBits(), OS);
    EncodeSize();
  } else if (T->isSpecificBuiltinType(BuiltinType::Char_S) ||
             T->isSpecificBuiltinType(BuiltinType::Char_U) ||
             T->isWideCharType()) {
    // Whether plain char and wchar_t are signed is an option that changes
    // neither the type, its layout nor its mangling, so objects built with and
    // without it agree, as they link together on ELF.
    OS << 'h';
    EncodeSize();
  } else if (T->isIntegerType()) {
    OS << (T->isSignedIntegerType() ? 'i' : 'u');
    EncodeSize();
  } else if (T->isRealFloatingType()) {
    OS << 'f' << char(getFloatFormat(Context.getFloatTypeSemantics(T)));
    EncodeSize();
  } else if (T->isAnyPointerType() || T->isReferenceType() ||
             T->isBlockPointerType() || T->isNullPtrType()) {
    OS << 'p';
    EncodeSize();
  } else if (T->isMemberPointerType()) {
    OS << (T->isMemberFunctionPointerType() ? 'M' : 'm');
    EncodeSize();
  } else if (const auto *CT = T->getAs<ComplexType>()) {
    OS << 'c';
    encodeType(CT->getElementType(), OS);
  } else if (const auto *VT = T->getAs<VectorType>()) {
    OS << 'v';
    encodeULEB128(VT->getNumElements(), OS);
    encodeType(VT->getElementType(), OS);
    EncodeSize();
  } else if (const auto *AT = T->getAs<AtomicType>()) {
    OS << 't';
    EncodeSize();
    encodeType(AT->getValueType(), OS);
  } else {
    OS << 'o';
    if (T->isIncompleteType() || T->isSizelessType())
      OS << '\0';
    else
      EncodeSize();
  }
}

// Encodes the layout of the record RD, a definition: whether it is a union,
// has a vtable, can be passed in registers and has a primary base, which may
// be virtual; its size, alignment and data size, and for a C++ class its
// non-virtual size and alignment; its non-virtual bases in declaration order
// and its virtual bases, each with its offset; its fields, each with its
// offset in bits, its width if a bit-field, and its type; and its vtable.
void CodeGenABIContracts::encodeRecord(const RecordDecl *RD,
                                       llvm::raw_ostream &OS) {
  const ASTRecordLayout &Layout = Context.getASTRecordLayout(RD);
  const auto *CRD = dyn_cast<CXXRecordDecl>(RD);
  bool Dynamic = CRD && CRD->isDynamicClass();
  uint64_t Flags = uint64_t(RD->isUnion()) | uint64_t(Dynamic) << 1 |
                   uint64_t(RD->canPassInRegisters()) << 2;
  if (CRD && Layout.getPrimaryBase())
    Flags |= uint64_t(1) << (Layout.isPrimaryBaseVirtual() ? 4 : 3);
  OS << 'R';
  encodeULEB128(Flags, OS);
  encodeULEB128(Layout.getSize().getQuantity(), OS);
  encodeULEB128(Layout.getAlignment().getQuantity(), OS);
  encodeULEB128(Layout.getDataSize().getQuantity(), OS);
  if (CRD) {
    encodeULEB128(Layout.getNonVirtualSize().getQuantity(), OS);
    encodeULEB128(Layout.getNonVirtualAlignment().getQuantity(), OS);
    SmallVector<const CXXBaseSpecifier *, 4> Bases;
    for (const CXXBaseSpecifier &B : CRD->bases())
      if (!B.isVirtual())
        Bases.push_back(&B);
    encodeULEB128(Bases.size(), OS);
    for (const CXXBaseSpecifier *B : Bases) {
      encodeULEB128(
          Layout.getBaseClassOffset(B->getType()->getAsCXXRecordDecl())
              .getQuantity(),
          OS);
      encodeType(B->getType(), OS);
    }
    encodeULEB128(CRD->getNumVBases(), OS);
    for (const CXXBaseSpecifier &B : CRD->vbases()) {
      encodeULEB128(
          Layout.getVBaseClassOffset(B.getType()->getAsCXXRecordDecl())
              .getQuantity(),
          OS);
      encodeType(B.getType(), OS);
    }
  }
  encodeULEB128(std::distance(RD->field_begin(), RD->field_end()), OS);
  for (const FieldDecl *FD : RD->fields()) {
    encodeULEB128(Layout.getFieldOffset(FD->getFieldIndex()), OS);
    encodeULEB128(FD->isBitField() ? FD->getBitWidthValue() + 1 : 0, OS);
    encodeType(FD->getType(), OS);
  }
  if (Dynamic)
    encodeVTable(CRD, OS);
}

// Encodes the vtable group of RD: each component in order, an offset with its
// value and an entry with the mangled name of the function or thunk it
// holds, and each vtable's start and address point.
void CodeGenABIContracts::encodeVTable(const CXXRecordDecl *RD,
                                       llvm::raw_ostream &OS) {
  const VTableLayout &VTLayout =
      CGM.getItaniumVTableContext().getVTableLayout(RD);
  ArrayRef<VTableComponent> Components = VTLayout.vtable_components();
  ArrayRef<VTableLayout::VTableThunkTy> Thunks = VTLayout.vtable_thunks();
  bool VectorDeletingDtors =
      Context.getTargetInfo().emitVectorDeletingDtors(Context.getLangOpts());
  encodeULEB128(Components.size(), OS);
  size_t NextThunk = 0;
  for (auto [I, C] : llvm::enumerate(Components)) {
    switch (C.getKind()) {
    case VTableComponent::CK_VCallOffset:
      OS << 'c';
      encodeSLEB128(C.getVCallOffset().getQuantity(), OS);
      continue;
    case VTableComponent::CK_VBaseOffset:
      OS << 'b';
      encodeSLEB128(C.getVBaseOffset().getQuantity(), OS);
      continue;
    case VTableComponent::CK_OffsetToTop:
      OS << 't';
      encodeSLEB128(C.getOffsetToTop().getQuantity(), OS);
      continue;
    case VTableComponent::CK_RTTI:
      OS << 'i';
      continue;
    case VTableComponent::CK_UnusedFunctionPointer:
    case VTableComponent::CK_FunctionPointer:
    case VTableComponent::CK_CompleteDtorPointer:
    case VTableComponent::CK_DeletingDtorPointer:
      break;
    }
    std::string Name;
    llvm::raw_string_ostream NameOS(Name);
    if (C.getKind() == VTableComponent::CK_UnusedFunctionPointer) {
      OS << 'u';
      const CXXMethodDecl *MD = C.getUnusedFunctionDecl();
      if (const auto *DD = dyn_cast<CXXDestructorDecl>(MD))
        Mangler.mangleName(GlobalDecl(DD, Dtor_Complete), NameOS);
      else
        Mangler.mangleName(GlobalDecl(MD), NameOS);
    } else {
      OS << 'f';
      GlobalDecl GD = C.getGlobalDecl(VectorDeletingDtors);
      if (NextThunk < Thunks.size() && Thunks[NextThunk].first == I) {
        const ThunkInfo &TI = Thunks[NextThunk++].second;
        if (const auto *DD = dyn_cast<CXXDestructorDecl>(GD.getDecl()))
          Mangler.mangleCXXDtorThunk(DD, GD.getDtorType(), TI,
                                     /*ElideOverrideInfo=*/true, NameOS);
        else
          Mangler.mangleThunk(cast<CXXMethodDecl>(GD.getDecl()), TI,
                              /*ElideOverrideInfo=*/true, NameOS);
      } else {
        Mangler.mangleName(GD, NameOS);
      }
    }
    encodeULEB128(Name.size(), OS);
    OS << Name;
  }
  encodeULEB128(VTLayout.getNumVTables(), OS);
  for (size_t I = 0, E = VTLayout.getNumVTables(); I != E; ++I) {
    encodeULEB128(VTLayout.getVTableOffset(I), OS);
    encodeULEB128(VTLayout.getAddressPointIndices()[I], OS);
  }
}

// The kinds of the items of a layout shape, in the low four bits of each
// item's first byte. The high four bits give the bits of padding ahead of the
// item, or, as 15, that a ULEB128 count of them follows the byte.
enum ShapeKind : uint8_t {
  ShapeInt8,
  ShapeInt16,
  ShapeInt32,
  ShapeInt64,
  ShapeInt128,
  ShapeFloat32,
  ShapeFloat64,
  ShapeFloat80,
  ShapePointer,
  ShapeVTablePointer,
  ShapeMemberFunctionPointer,
  ShapeBitField,
  ShapeArray,
  ShapeUnion,
  ShapeEnd,
  ShapeExtended,
};

// The kinds that follow ShapeExtended, in a byte of their own.
enum ShapeExtendedKind : uint8_t {
  ShapeFloat16,
  ShapeBFloat16,
  ShapeFloat128,
  ShapeMemberDataPointer,
  ShapeVector,
  ShapeBitInt,
  ShapeOpaque,
};

// An item of a layout shape: the bits it covers and its encoding, the
// padding ahead of it left out.
struct CodeGenABIContracts::ShapeItem {
  uint64_t Begin;
  uint64_t End;
  std::string Bytes;
};

// Encodes Items, a sequence's, after the bits of padding ahead of each. A run
// of two or more equal items with none between them, as the elements of an
// array are, is one array item, so that the encoding is canonical.
std::string
CodeGenABIContracts::encodeShapeItems(SmallVectorImpl<ShapeItem> &Items) {
  llvm::stable_sort(Items, [](const auto &A, const auto &B) {
    return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
  });
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  uint64_t Previous = 0;
  for (size_t I = 0; I != Items.size();) {
    size_t J = I + 1;
    while (J != Items.size() && Items[J].Bytes == Items[I].Bytes &&
           Items[J].Begin == Items[J - 1].End)
      ++J;
    std::string Run;
    StringRef Item = Items[I].Bytes;
    if (J - I > 1) {
      llvm::raw_string_ostream RunOS(Run);
      RunOS << char(ShapeArray);
      encodeULEB128(J - I, RunOS);
      RunOS << Item << char(ShapeEnd);
      Item = Run;
    }
    uint64_t Padding =
        Items[I].Begin > Previous ? Items[I].Begin - Previous : 0;
    OS << char(uint8_t(Item[0]) | std::min<uint64_t>(Padding, 15) << 4);
    if (Padding >= 15)
      encodeULEB128(Padding, OS);
    OS << Item.drop_front();
    Previous = Items[J - 1].End;
    I = J;
  }
  return Out;
}

// Adds the items of the layout of a T at Offset bits: a scalar's kind, every
// leaf of a record held by value, an array's count and its element's items, a
// union's alternatives. A pointer is a pointer only, whatever it points to.
void CodeGenABIContracts::addShapeItems(QualType T, uint64_t Offset,
                                        SmallVectorImpl<ShapeItem> &Items) {
  T = Context.getCanonicalType(T).getUnqualifiedType();
  uint64_t Width = Context.getTypeSize(T);
  auto Add = [&](std::string Bytes) {
    Items.push_back({Offset, Offset + Width, std::move(Bytes)});
  };
  auto AddKind = [&](ShapeKind Kind) { Add(std::string(1, char(Kind))); };
  auto AddExtended = [&](ShapeExtendedKind Kind, StringRef Operands = "") {
    Add((Twine(char(ShapeExtended)) + Twine(char(Kind)) + Operands).str());
  };
  auto Nested = [&](QualType Element) {
    SmallVector<ShapeItem, 4> ElementItems;
    addShapeItems(Element, 0, ElementItems);
    return encodeShapeItems(ElementItems) + char(ShapeEnd);
  };

  if (const auto *AT = Context.getAsConstantArrayType(T)) {
    if (AT->getZExtSize() == 0)
      return;
    std::string Bytes(1, char(ShapeArray));
    llvm::raw_string_ostream OS(Bytes);
    encodeULEB128(AT->getZExtSize(), OS);
    Add(Bytes + Nested(AT->getElementType()));
    return;
  }
  if (const RecordDecl *RD = T->getAsRecordDecl()) {
    const RecordDecl *Def = RD->getDefinition();
    if (!Def)
      return AddExtended(ShapeOpaque);
    if (!Def->isUnion())
      return addRecordShapeItems(Def, Offset, /*Complete=*/true, Items);
    std::string Bytes(1, char(ShapeUnion));
    llvm::raw_string_ostream OS(Bytes);
    encodeULEB128(std::distance(Def->field_begin(), Def->field_end()), OS);
    for (const FieldDecl *FD : Def->fields())
      OS << Nested(FD->getType());
    return Add(Bytes);
  }
  if (const auto *ED = T->getAsEnumDecl()) {
    const EnumDecl *Def = ED->getDefinition();
    if (QualType Int = (Def ? Def : ED)->getIntegerType(); !Int.isNull())
      return addShapeItems(Int, Offset, Items);
  }
  if (const auto *CT = T->getAs<ComplexType>()) {
    std::string Bytes(1, char(ShapeArray));
    llvm::raw_string_ostream OS(Bytes);
    encodeULEB128(2, OS);
    return Add(Bytes + Nested(CT->getElementType()));
  }
  if (const auto *VT = T->getAs<VectorType>()) {
    std::string Operands;
    llvm::raw_string_ostream OS(Operands);
    encodeULEB128(VT->getNumElements(), OS);
    return AddExtended(ShapeVector, Operands + Nested(VT->getElementType()));
  }
  if (const auto *AT = T->getAs<AtomicType>()) {
    if (Context.getTypeSize(AT->getValueType()) == Width)
      return addShapeItems(AT->getValueType(), Offset, Items);
  } else if (const auto *BT = T->getAs<BitIntType>()) {
    std::string Operands;
    llvm::raw_string_ostream OS(Operands);
    encodeULEB128(BT->getNumBits(), OS);
    return AddExtended(ShapeBitInt, Operands);
  } else if (T->isIntegerType()) {
    switch (Width) {
    case 8:
      return AddKind(ShapeInt8);
    case 16:
      return AddKind(ShapeInt16);
    case 32:
      return AddKind(ShapeInt32);
    case 64:
      return AddKind(ShapeInt64);
    case 128:
      return AddKind(ShapeInt128);
    }
  } else if (T->isRealFloatingType()) {
    const llvm::fltSemantics &S = Context.getFloatTypeSemantics(T);
    if (&S == &llvm::APFloat::IEEEhalf())
      return AddExtended(ShapeFloat16);
    if (&S == &llvm::APFloat::BFloat())
      return AddExtended(ShapeBFloat16);
    if (&S == &llvm::APFloat::IEEEsingle())
      return AddKind(ShapeFloat32);
    if (&S == &llvm::APFloat::IEEEdouble())
      return AddKind(ShapeFloat64);
    if (&S == &llvm::APFloat::x87DoubleExtended())
      return AddKind(ShapeFloat80);
    if (&S == &llvm::APFloat::IEEEquad())
      return AddExtended(ShapeFloat128);
  } else if (T->isAnyPointerType() || T->isReferenceType() ||
             T->isBlockPointerType() || T->isNullPtrType()) {
    return AddKind(ShapePointer);
  } else if (T->isMemberFunctionPointerType()) {
    return AddKind(ShapeMemberFunctionPointer);
  } else if (T->isMemberDataPointerType()) {
    return AddExtended(ShapeMemberDataPointer);
  }
  std::string Operands;
  llvm::raw_string_ostream OS(Operands);
  encodeULEB128(Width, OS);
  AddExtended(ShapeOpaque, Operands);
}

// Adds the items of the record RD at Offset bits: the vtable pointer it
// introduces, its bases' and fields' items at their offsets, and, as a
// complete object, its virtual bases'.
void CodeGenABIContracts::addRecordShapeItems(
    const RecordDecl *RD, uint64_t Offset, bool Complete,
    SmallVectorImpl<ShapeItem> &Items) {
  const ASTRecordLayout &Layout = Context.getASTRecordLayout(RD);
  if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD)) {
    if (CRD->isDynamicClass() && !Layout.getPrimaryBase())
      Items.push_back({Offset,
                       Offset + Context.getTargetInfo().getPointerWidth(
                                    LangAS::Default),
                       std::string(1, char(ShapeVTablePointer))});
    for (const CXXBaseSpecifier &B : CRD->bases())
      if (!B.isVirtual()) {
        const CXXRecordDecl *Base = B.getType()->getAsCXXRecordDecl();
        addRecordShapeItems(
            Base,
            Offset + Context.toBits(Layout.getBaseClassOffset(Base)),
            /*Complete=*/false, Items);
      }
    if (Complete)
      for (const CXXBaseSpecifier &B : CRD->vbases()) {
        const CXXRecordDecl *Base = B.getType()->getAsCXXRecordDecl();
        addRecordShapeItems(
            Base,
            Offset + Context.toBits(Layout.getVBaseClassOffset(Base)),
            /*Complete=*/false, Items);
      }
  }
  for (const FieldDecl *FD : RD->fields()) {
    uint64_t FieldOffset = Offset + Layout.getFieldOffset(FD->getFieldIndex());
    if (!FD->isBitField()) {
      addShapeItems(FD->getType(), FieldOffset, Items);
      continue;
    }
    unsigned Width = FD->getBitWidthValue();
    if (Width == 0)
      continue;
    std::string Bytes(1, char(ShapeBitField));
    llvm::raw_string_ostream OS(Bytes);
    encodeULEB128(Width, OS);
    Items.push_back({FieldOffset, FieldOffset + Width, Bytes});
  }
}

// The layout of RD as section 14's canonical form gives it, projected onto
// what an access to a complete object reads: its size and alignment, then
// every leaf, by-value members and bases flattened in, each a kind with the
// bits of padding ahead of it, and the vtable pointers. The definition alone
// fixes it, so every translation unit emits the same bytes.
StringRef CodeGenABIContracts::getLayoutShape(const RecordDecl *RD) {
  auto [It, Inserted] = LayoutShapes.try_emplace(RD);
  if (!Inserted)
    return It->second;
  const ASTRecordLayout &Layout = Context.getASTRecordLayout(RD);
  std::string Shape;
  llvm::raw_string_ostream OS(Shape);
  encodeULEB128(Layout.getSize().getQuantity(), OS);
  OS << char(llvm::Log2_64(Layout.getAlignment().getQuantity()));
  SmallVector<ShapeItem, 8> Items;
  if (RD->isUnion())
    addShapeItems(Context.getCanonicalTagType(RD), 0, Items);
  else
    addRecordShapeItems(RD, 0, /*Complete=*/true, Items);
  OS << encodeShapeItems(Items);
  // The walk adds no entry to the map, so It is still valid.
  return It->second = std::move(Shape);
}

llvm::MDNode *CodeGenABIContracts::getTypeContract(const RecordDecl *RD) {
  llvm::LLVMContext &Ctx = CGM.getLLVMContext();
  Digest D = getRecordDigest(RD);
  llvm::Metadata *Ops[] = {
      llvm::MDString::get(Ctx, getIdentity(RD)),
      llvm::MDString::get(Ctx, llvm::toStringRef(llvm::ArrayRef(D)))};
  return llvm::MDNode::get(Ctx, Ops);
}

llvm::MDNode *CodeGenABIContracts::getDataContract(const VarDecl *VD,
                                                   StringRef Identity) {
  llvm::LLVMContext &Ctx = CGM.getLLVMContext();
  Digest D = getDataDigest(VD, Identity);
  return llvm::MDNode::get(
      Ctx, llvm::MDString::get(Ctx, llvm::toStringRef(llvm::ArrayRef(D))));
}

// A provide row as the linker option names it: the contract's locator and the
// unpadded base64url form of its digest, 33 characters.
static std::string formatProvideRow(StringRef Identity,
                                    ArrayRef<uint8_t> Digest) {
  return llvm::COFF::getABIContractLocator(Identity) +
         llvm::encodeBase64URL(Digest);
}

// Whether GV has vague linkage, as an alias in the COMDAT of a definition with
// vague linkage does: it moves with the definition.
static bool isWeakForLinker(const llvm::GlobalValue *GV) {
  const llvm::GlobalObject *GO = GV->getAliaseeObject();
  return GV->isWeakForLinker() ||
         (GO && GO != GV && GO->isWeakForLinker() && GO->hasComdat());
}

void CodeGenABIContracts::emit() {
  // A record whose layout the code uses also uses the layouts of its bases
  // and of the records it holds by value.
  SmallVector<const RecordDecl *, 0> Worklist(
      CGM.getTypes().getLayoutUses().begin(),
      CGM.getTypes().getLayoutUses().end());
  while (!Worklist.empty()) {
    const RecordDecl *RD = Worklist.pop_back_val();
    if (!UsedLayouts.insert(RD).second)
      continue;
    if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD))
      for (const CXXBaseSpecifier &B : CRD->bases())
        if (const RecordDecl *Def = B.getType()->getAsRecordDecl())
          if ((Def = Def->getDefinition()))
            Worklist.push_back(Def);
    for (const FieldDecl *FD : RD->fields())
      if (const RecordDecl *Member =
              Context.getBaseElementType(FD->getType())->getAsRecordDecl())
        if (const RecordDecl *Def = Member->getDefinition())
          Worklist.push_back(Def);
  }

  llvm::Module &M = CGM.getModule();
  RecordSet ProvidedTypes;
  SmallVector<std::string, 0> ProvidedData;
  llvm::StringSet<> ProvidedDataRows;
  llvm::MapVector<llvm::GlobalValue *, SmallVector<llvm::MDNode *, 2>> Required;

  auto Require = [&](llvm::GlobalValue *GV, const RecordSet &Related,
                     const VarDecl *Datum) {
    RecordSet Covered;
    addCoveredRecords(Related, /*UsedOnly=*/true, Covered);
    SmallVector<llvm::MDNode *, 2> &Row = Required[GV];
    for (const RecordDecl *RD : Covered)
      Row.push_back(getTypeContract(RD));
    if (Datum)
      Row.push_back(getDataContract(
          Datum, llvm::GlobalValue::dropLLVMManglingEscape(GV->getName())));
  };

  for (const auto &[GD, Name] : CGM.getMangledDeclNames()) {
    const auto *D = dyn_cast<NamedDecl>(GD.getDecl());
    if (!D || !isa<FunctionDecl, VarDecl>(D))
      continue;
    // Another image reaches a thread-local variable through the record that
    // the variable's image exports, which carries the variable's contracts in
    // its place.
    const auto *VD = dyn_cast<VarDecl>(D);
    bool ThreadLocal = VD && VD->getTLSKind() != VarDecl::TLS_None;
    llvm::GlobalValue *GV =
        M.getNamedValue(ThreadLocal ? (Name + "$tls").str() : Name);
    if (!GV || GV->hasLocalLinkage())
      continue;
    const VarDecl *Datum = VD && hasDataContract(VD) ? VD : nullptr;
    RecordSet Related;
    addRelatedRecords(D, Related);

    // A declaration the module emits in import form, which the import of a
    // DLL may satisfy, or a variable with vague linkage whose instance a DLL
    // the image imports from may hold, requires the contracts of the records
    // it is about whose layout the code uses.
    bool ImportForm;
    if (ThreadLocal)
      ImportForm =
          GV->isDeclarationForLinker() && GV->hasDLLImportStorageClass();
    else if (GV->isDeclarationForLinker())
      ImportForm = D->hasAttr<DLLImportAttr>() ||
                   CGM.isMappedImportVisibility(D->getLinkageAndVisibility()) ||
                   (VD && !GV->isDSOLocal() &&
                    !CGM.getCodeGenOpts().DirectAccessExternalData);
    else
      ImportForm = VD && isWeakForLinker(GV) && GV->hasDefaultVisibility() &&
                   !GV->isDSOLocal();
    if (ImportForm && (Datum || !Related.empty()))
      Require(GV, Related, Datum);

    // A definition that the image may export, or that a DLL may offer to the
    // images that import from it, provides the contracts of the records it is
    // about.
    if (GV->isDeclarationForLinker() ||
        !(GV->hasDLLExportStorageClass() ||
          (isWeakForLinker(GV) && GV->hasDefaultVisibility())))
      continue;
    addCoveredRecords(Related, /*UsedOnly=*/false, ProvidedTypes);
    if (Datum) {
      StringRef Identity =
          llvm::GlobalValue::dropLLVMManglingEscape(GV->getName());
      std::string Row =
          formatProvideRow(Identity, getDataDigest(Datum, Identity));
      if (ProvidedDataRows.insert(Row).second)
        ProvidedData.push_back(std::move(Row));
    }
  }

  // A type_info object that the image may export, or offer to the images that
  // import from it, provides the contract of its class, so that every class
  // another image can name by its type_info has one, whether or not a
  // function or datum the image exports is about it.
  for (auto [GV, RD] : TypeInfos)
    if (!GV->isDeclarationForLinker() &&
        (GV->hasDLLExportStorageClass() ||
         (GV->isWeakForLinker() && GV->hasDefaultVisibility())))
      if (const RecordDecl *Def = RD->getDefinition())
        addCoveredRecords(RecordSet(&Def, &Def + 1), /*UsedOnly=*/false,
                          ProvidedTypes);

  // A vtable or VTT that the module emits in import form requires the
  // contract of its class.
  for (auto [GV, RD] : VTableImports)
    if (GV->isDeclarationForLinker() && !GV->isDSOLocal())
      if (const RecordDecl *Def = RD->getDefinition())
        Require(GV, RecordSet(&Def, &Def + 1), nullptr);

  // The provide rows travel as linker options, which the linker reads from
  // every object it loads, bitcode included, before it selects among COMDAT
  // copies.
  auto AddRows = [&](StringRef Directive, ArrayRef<std::string> Rows) {
    if (!Rows.empty())
      CGM.AppendLinkFact((Directive + llvm::join(Rows, ",")).str());
  };
  SmallVector<std::string, 0> TypeRows;
  for (const RecordDecl *RD : ProvidedTypes)
    TypeRows.push_back(formatProvideRow(getIdentity(RD), getRecordDigest(RD)));
  AddRows("/abiprovide:", TypeRows);
  AddRows("/abiprovidedata:", ProvidedData);

  llvm::LLVMContext &Ctx = CGM.getLLVMContext();

  llvm::NamedMDNode *Requirements = nullptr;
  for (auto &[GV, Contracts] : Required) {
    if (Contracts.empty())
      continue;
    if (!Requirements)
      Requirements = M.getOrInsertNamedMetadata("abi.requirements");
    SmallVector<llvm::Metadata *, 4> Ops = {llvm::ValueAsMetadata::get(GV)};
    Ops.append(Contracts.begin(), Contracts.end());
    Requirements->addOperand(llvm::MDNode::get(Ctx, Ops));
  }
}
