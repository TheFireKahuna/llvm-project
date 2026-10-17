//===--- CGKCFI.h - KCFI types and link facts -------------------*- C++ -*-===//
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

#ifndef LLVM_CLANG_LIB_CODEGEN_CGKCFI_H
#define LLVM_CLANG_LIB_CODEGEN_CGKCFI_H

#include "clang/AST/Expr.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"

namespace llvm {
class ConstantInt;
class Function;
} // namespace llvm

namespace clang {
class CXXRecordDecl;
class Decl;
class FunctionDecl;
class NamedDecl;
class RecordDecl;
class VarDecl;

namespace CodeGen {

class CodeGenModule;

class CodeGenKCFI {
  CodeGenKCFI(const CodeGenKCFI &) = delete;
  void operator=(const CodeGenKCFI &) = delete;

public:
  /// A KCFI call type: its check identifier, with pointers generalised, which
  /// keys thunks, routines and the type checks; and its precise identifier,
  /// with pointers kept, which decides how an opening propagates.
  using KCFITypeId = std::pair<llvm::ConstantInt *, uint32_t>;

  explicit CodeGenKCFI(CodeGenModule &CGM);

  /// Whether a virtual call checks a KCFI type salted by the class that
  /// introduces the vtable slot, which every function that can occupy the
  /// slot carries, and destructors carry the type the runtime calls them
  /// through: with the Itanium C++ ABI.
  bool hasVTableSlotTypes() const { return HasVTableSlotTypes; }

  /// The identifiers of the functions that can occupy the vtable slot that
  /// Slot, a virtual member function or a variant of a virtual destructor,
  /// introduces.
  KCFITypeId createVTableSlotTypeIds(GlobalDecl Slot);

  /// The identifiers that a function of type FnType that can occupy a vtable
  /// slot carries besides its own, and that a call through a member function
  /// pointer to a virtual function checks: FnType salted "__vfn", which does
  /// not depend on the class that introduces the slot.
  KCFITypeId createVfnTypeIds(QualType FnType);

  /// The identifiers that a call through a pointer to a function of type
  /// FnType checks: FnType salted by its cfi_salt.
  KCFITypeId createCallTypeIds(QualType FnType);

  /// The identifiers of a destructor: void(void *) salted "__cxa_dtor", the
  /// type the runtime calls a destructor through, and further salted by
  /// DeletingClass, the class that introduces the vtable slot, for a deleting
  /// destructor.
  KCFITypeId
  createDestructorTypeIds(const CXXRecordDecl *DeletingClass = nullptr);

  /// Attach to F, which can occupy a vtable slot of type FnType, the type a
  /// call through a member function pointer checks.
  void setVfnType(llvm::Function *F, QualType FnType);

  /// When GD is a virtual member function or a destructor, attach to its
  /// function F the type of the vtable slot it occupies or the type the
  /// runtime calls a destructor through, and return true.
  bool setVTableSlotType(GlobalDecl GD, llvm::Function *F);

private:
  CodeGenModule &CGM;
  const bool HasVTableSlotTypes;

  /// The kinds of identifier that TypeIds holds, each keyed by the canonical
  /// function type, slot or deleting class that it is computed from.
  enum TypeIdKind : unsigned { CallKind, VfnKind, SlotKind, DestructorKind };
  llvm::DenseMap<std::pair<const void *, unsigned>, KCFITypeId> TypeIds;

  /// The identifiers of a function of type FnType salted by Salt: its check
  /// identifier, and, when the module records link facts, its precise
  /// identifier, with pointers kept, which decides how an opening of the type
  /// propagates even where the check identifier generalises pointers.
  KCFITypeId createTypeIds(QualType FnType, StringRef Salt);
};

} // namespace CodeGen
} // namespace clang

#endif // LLVM_CLANG_LIB_CODEGEN_CGKCFI_H
