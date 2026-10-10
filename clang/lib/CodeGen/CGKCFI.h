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

  /// Whether the module records which KCFI types it opens to functions
  /// without a prefix of ours, for the per-type routines on COFF.
  bool hasFacts() const { return HasFacts; }

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

  /// Record that a function of KCFI type Id may reach this module from code
  /// that carries no KCFI prefix of ours, so that a call of that type to a
  /// target without the marker proceeds at the strength of Control Flow
  /// Guard. Such a call, through a vtable or a member function pointer, calls
  /// a function of type FnType, so the types that it hands back open too.
  void addDynamicType(KCFITypeId Id, QualType FnType) {
    DynamicTypes.insert(Id);
    DynamicCalls.insert({Id, FnType.getCanonicalType().getTypePtr()});
  }

  /// Record the type of the function pointer that a conversion of a value of
  /// type From to type To creates, unless From already pointed to a function
  /// of that type, or that the conversion lets code store into or load from
  /// untyped, when it converts a pointer that reaches a function pointer
  /// through one or more levels to a pointer of another type at that level,
  /// or the reverse. With LValue, the conversion reinterprets an object of
  /// type From as one of type To. Operand, when given, is the converted
  /// expression, whose value a conversion to a pointer to a record may take
  /// from a declaration.
  void addConversionType(QualType From, QualType To, bool LValue = false,
                         const Expr *Operand = nullptr);

  /// Record the type of a function pointer that LValue reads from a member of
  /// a union that has a member of another type, which may have written it:
  /// the union's member itself, or a record or an array that holds it.
  void addUnionReadType(const Expr *LValue);

  /// Record that the assignment of RHS to LHS stores into a variable that
  /// only this translation unit stores into the value of a declaration, a
  /// call or a load, so that the records its value is converted to a pointer
  /// to belong to that declaration.
  void addAssignment(const Expr *LHS, const Expr *RHS);

  /// Record the type T that the code of D reads with va_arg: among the types
  /// that the callers of D hand it, when D is a variadic function, or else,
  /// from a va_list that D was handed, among the types of the functions that
  /// may reach this module from code without a prefix of ours.
  void addVAArgType(QualType T, const Decl *D);

  /// Record that this module calls a function of type FnType through a
  /// pointer, so that the types such a call hands back open when FnType
  /// opens.
  void addCalledType(QualType FnType);

  /// Record the objects that the arguments Args of a call to FD point to,
  /// when FD is not a library builtin and an argument is past the parameters
  /// of FD's prototype, at or after NumParams, or converted to a parameter of
  /// type void * or a pointer to a character type, so that the facts of FD
  /// include the function pointers those objects hold, as they include those
  /// of the records that its result is converted to a pointer to. A function
  /// pointer passed so is recorded too, so that FD's facts include the types
  /// that FD may hand to it, as for a parameter of its type.
  /// When FD is memcpy, memmove or one of their forms, record the record
  /// that its destination points to as a conversion of its source to a
  /// pointer to the record does.
  void addCallArguments(const FunctionDecl *FD,
                        llvm::iterator_range<CallExpr::const_arg_iterator> Args,
                        unsigned NumParams);

  /// Whether a call through a vtable of RD may reach a function that carries
  /// no KCFI prefix of ours: when RD or one of its bases says that another
  /// image or foreign code may create its objects, by a uuid, which COM
  /// objects implement, by dllimport or dllexport, or by an explicit default
  /// visibility that the visibility mapping exports.
  bool isVTableOpen(const CXXRecordDecl *RD);

  /// Mark the function declarations that are known imports and record the
  /// KCFI types that this module opens to functions without a prefix of
  /// ours.
  void emitFacts();

private:
  CodeGenModule &CGM;
  const bool HasFacts;
  const bool HasVTableSlotTypes;

  /// The KCFI types of the functions that may reach this module from code
  /// that carries no KCFI prefix of ours.
  llvm::SetVector<KCFITypeId> DynamicTypes;

  /// The records that a value of a declaration, the result of a call to a
  /// function or a load of a variable, is converted to a pointer to, and
  /// those that a call to a function passes a pointer to in a variadic
  /// argument or as an untyped pointer parameter.
  llvm::MapVector<const NamedDecl *, llvm::SetVector<const RecordDecl *>>
      BoundaryRecords;

  /// The records that a value loaded from a variable that only this
  /// translation unit stores into is converted to a pointer to, and the
  /// declarations whose values its assignments store, which the records
  /// belong to.
  llvm::MapVector<const VarDecl *, llvm::SetVector<const RecordDecl *>>
      LocalBoundaryRecords;
  llvm::DenseMap<const VarDecl *, llvm::SetVector<const NamedDecl *>>
      LocalSources;

  /// The canonical types that a variadic function definition reads with
  /// va_arg.
  llvm::DenseMap<const FunctionDecl *, llvm::SetVector<const Type *>>
      VAArgTypes;

  /// The canonical types of the objects, function pointers or records, that a
  /// call to a function passes a pointer to in a variadic argument or as an
  /// untyped pointer parameter, and the function types of the function
  /// pointers it passes so.
  llvm::DenseMap<const FunctionDecl *, llvm::SetVector<const Type *>>
      UntypedArgTypes;

  /// The canonical function types that this module calls through pointers.
  llvm::SetVector<const Type *> CalledTypes;

  /// The KCFI types of the calls through vtables and member function pointers
  /// that may reach a function without a prefix of ours, each with the
  /// canonical function type that it calls.
  llvm::SetVector<std::pair<KCFITypeId, const Type *>> DynamicCalls;

  /// The kinds of identifier that TypeIds holds, each keyed by the canonical
  /// function type, slot or deleting class that it is computed from.
  enum TypeIdKind : unsigned { CallKind, VfnKind, SlotKind, DestructorKind };
  llvm::DenseMap<std::pair<const void *, unsigned>, KCFITypeId> TypeIds;

  /// The identifiers of a function of type FnType salted by Salt: its check
  /// identifier, and, when the module records link facts, its precise
  /// identifier, with pointers kept, which decides how an opening of the type
  /// propagates even where the check identifier generalises pointers.
  KCFITypeId createTypeIds(QualType FnType, StringRef Salt);

  /// Record the record Pointee, whose function pointers the facts of a
  /// declaration then include, when a pointer to it is converted from Operand
  /// and Operand is directly the result of a call to the function or a load
  /// of the variable.
  void addBoundaryRecord(QualType Pointee, const Expr *Operand);

  /// Give each declaration whose value a variable that only this translation
  /// unit stores into holds, through its initializer or its assignments, the
  /// records that a value loaded from the variable is converted to a pointer
  /// to.
  void resolveLocalBoundaryRecords();

  /// Collect into TypeIds the KCFI types of the function pointers that a call
  /// to FD hands back to its caller, through its return type and through the
  /// objects that its pointer parameters to non-const types point to, and
  /// the function pointers that calls pass untyped as addCallArguments
  /// records, when Params is false; the walk follows pointers and the fields
  /// and bases of records, and a polymorphic class adds the types of its
  /// vtable slots; and those that FD hands to the functions that the call
  /// hands it, in its parameters, held in the objects its parameters hold or
  /// point to, or in the objects that the call passes untyped, walked as
  /// their parameters are when Params is true. Or,
  /// when Params is true, that a caller hands to FD: its function pointer
  /// parameters and the function pointers held in the objects its parameters
  /// hold or point to, without following pointers further, and in the same
  /// way the variadic arguments it reads with va_arg and the parameters of
  /// the functions that FD hands back to its caller, in its result and
  /// through its out-parameters, the pointers to non-const pointers.
  /// With Records, the walk stops at the records it reaches and lists them
  /// there instead; when Params is false, it lists in Held those whose held
  /// function pointers alone count, which the parameters of the functions
  /// that the call hands FD reach.
  void
  collectInflowTypes(const FunctionDecl *FD, bool Params,
                     llvm::SetVector<KCFITypeId> &TypeIds,
                     llvm::SetVector<const RecordDecl *> *Records = nullptr,
                     llvm::SetVector<const RecordDecl *> *Held = nullptr);
};

} // namespace CodeGen
} // namespace clang

#endif // LLVM_CLANG_LIB_CODEGEN_CGKCFI_H
