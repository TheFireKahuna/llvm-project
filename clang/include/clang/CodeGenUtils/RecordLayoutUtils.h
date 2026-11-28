//===--- RecordLayoutUtils.h - Shared record layout queries -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file holds the AST queries about record layout that both classic
// CodeGen and CIR CodeGen need while lowering a record to its target type.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CODEGENUTILS_RECORDLAYOUTUTILS_H
#define LLVM_CLANG_CODEGENUTILS_RECORDLAYOUTUTILS_H

#include "clang/AST/ASTContext.h"

namespace clang::CodeGenUtils {

/// Recursively searches all of the bases of \p Decl to find out whether
/// \p Query is not the primary vbase of some base class.
bool hasOwnStorage(const ASTContext &Ctx, const CXXRecordDecl *Decl,
                   const CXXRecordDecl *Query);

/// The Microsoft bitfield layout rule allocates discrete storage units of the
/// field's formal type and only combines adjacent fields of the same formal
/// type.  We want to emit a layout with these discrete storage units instead
/// of combining them into a continuous run.
bool isDiscreteBitFieldABI(const ASTContext &Ctx, const RecordDecl *RD);

/// Return true iff the field is "empty", that is, either a zero-width
/// bit-field or an \ref isEmptyRecordForLayout.
bool isEmptyFieldForLayout(const ASTContext &Ctx, const FieldDecl *FD);

/// Return true iff a structure contains only empty base classes (per \ref
/// isEmptyRecordForLayout) and fields (per \ref isEmptyFieldForLayout).  Note,
/// C++ record fields are considered empty if the [[no_unique_address]]
/// attribute would have made them empty, so this is not the same as \ref
/// isEmptyRecord.
bool isEmptyRecordForLayout(const ASTContext &Ctx, QualType T);

/// The Itanium base layout rule allows virtual bases to overlap other bases,
/// which complicates layout in specific ways.
///
/// Note specifically that the ms_struct attribute doesn't change this.
bool isOverlappingVBaseABI(const ASTContext &Ctx);

/// Walks the layout of a type as code compiled against the type depends on
/// it, reporting each part at its offset in bits. A derived class encodes the
/// parts, and decides for an array's element, a record held by value and a
/// base whether to walk it in turn, in place or as a part of its own.
class LayoutWalker {
public:
  enum class ScalarKind {
    Integer, // An integer or an enumeration.
    BitInt,
    Float,
    Pointer, // A pointer, a reference, a block pointer or std::nullptr_t.
    VTablePointer,
    MemberFunctionPointer,
    MemberDataPointer,
    Opaque, // Any other type, of which the walk knows only the size.
  };

  /// Reports a \p T at \p Offset.
  void walkType(QualType T, uint64_t Offset);

  /// Reports the parts of \p RD, a definition, at \p Offset: the vtable
  /// pointer it introduces, its non-virtual bases, as a complete object its
  /// virtual bases, and its fields.
  void walkRecord(const RecordDecl *RD, uint64_t Offset, bool IsCompleteObject);

protected:
  explicit LayoutWalker(const ASTContext &Ctx) : Ctx(Ctx) {}
  ~LayoutWalker() = default;

  virtual void scalar(uint64_t Offset, ScalarKind Kind, QualType T) = 0;
  /// \p FD is a bit-field of a width other than zero.
  virtual void bitField(uint64_t Offset, const FieldDecl *FD) = 0;
  /// \p T is an array, a complex or a vector of \p Count \p Element.
  virtual void array(uint64_t Offset, QualType T, uint64_t Count,
                     QualType Element) = 0;
  /// \p RD is held by value.
  virtual void record(uint64_t Offset, const RecordDecl *RD) = 0;
  virtual void base(uint64_t Offset, const CXXRecordDecl *Base,
                    bool IsVirtual) = 0;

  const ASTContext &Ctx;
};

} // namespace clang::CodeGenUtils

#endif // LLVM_CLANG_CODEGENUTILS_RECORDLAYOUTUTILS_H
