//===--- CGABIContracts.h - ABI contracts between images --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The ABI contracts of Windows Itanium and NT-POSIX: the layouts of the types
// that a module's exported definitions provide to other images, and those
// that its references to other images' symbols require, which the linker
// compares so that an image built against another layout fails to link or to
// load rather than corrupting memory.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_CGABICONTRACTS_H
#define LLVM_CLANG_LIB_CODEGEN_CGABICONTRACTS_H

#include "clang/AST/Type.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include <array>
#include <string>

namespace llvm {
class GlobalValue;
class MDNode;
} // namespace llvm

namespace clang {
class ASTContext;
class CXXRecordDecl;
class MangleContext;
class NamedDecl;
class RecordDecl;
class VarDecl;

namespace CodeGen {

class CodeGenModule;

class CodeGenABIContracts {
  CodeGenABIContracts(const CodeGenABIContracts &) = delete;
  void operator=(const CodeGenABIContracts &) = delete;

public:
  explicit CodeGenABIContracts(CodeGenModule &CGM);

  /// Records that the import-form declaration GV is the vtable or VTT of RD.
  void addVTableImport(llvm::GlobalValue *GV, const CXXRecordDecl *RD);

  /// Records that GV is the type_info object of RD.
  void addTypeInfo(llvm::GlobalValue *GV, const CXXRecordDecl *RD);

  /// Emits the contracts that the module's definitions provide, as linker
  /// options, and those that its references require, as the
  /// abi.requirements metadata.
  void emit();

  /// Returns the layout shape of RD, a definition, which a non-unique
  /// type_info object's name holds after its NUL.
  StringRef getLayoutShape(const RecordDecl *RD);

private:
  using Digest = std::array<uint8_t, 16>;
  using RecordSet = llvm::SmallSetVector<const RecordDecl *, 4>;

  bool isContracted(const RecordDecl *RD) const;
  bool isLayoutUsed(const RecordDecl *RD) const;
  void addRelatedRecords(const NamedDecl *D, RecordSet &Records) const;
  const RecordSet &getPointees(const RecordDecl *RD);
  void addCoveredRecords(const RecordSet &Related, bool UsedOnly,
                         RecordSet &Covered);
  bool hasDataContract(const VarDecl *VD) const;

  std::string getIdentity(const RecordDecl *RD) const;
  Digest getRecordDigest(const RecordDecl *RD);
  Digest getDataDigest(const VarDecl *VD, StringRef Identity);
  Digest finishDigest(StringRef Identity, StringRef Contract) const;
  void encodeRecord(const RecordDecl *RD, llvm::raw_ostream &OS);
  void encodeType(QualType T, llvm::raw_ostream &OS);
  void encodeVTable(const CXXRecordDecl *RD, llvm::raw_ostream &OS);

  struct ShapeItem;
  static std::string encodeShapeItems(llvm::SmallVectorImpl<ShapeItem> &Items);
  void addShapeItems(QualType T, uint64_t Offset,
                     llvm::SmallVectorImpl<ShapeItem> &Items);
  void addRecordShapeItems(const RecordDecl *RD, uint64_t Offset,
                           bool Complete,
                           llvm::SmallVectorImpl<ShapeItem> &Items);

  llvm::MDNode *getTypeContract(const RecordDecl *RD);
  llvm::MDNode *getDataContract(const VarDecl *VD, StringRef Identity);

  CodeGenModule &CGM;
  ASTContext &Context;
  MangleContext &Mangler;
  llvm::DenseMap<const RecordDecl *, Digest> RecordDigests;
  llvm::DenseMap<const RecordDecl *, std::string> LayoutShapes;
  llvm::DenseMap<const RecordDecl *, RecordSet> RecordPointees;
  llvm::DenseSet<const RecordDecl *> UsedLayouts;
  llvm::SmallVector<std::pair<llvm::GlobalValue *, const CXXRecordDecl *>, 0>
      VTableImports;
  llvm::SmallVector<std::pair<llvm::GlobalValue *, const CXXRecordDecl *>, 0>
      TypeInfos;
};

} // namespace CodeGen
} // namespace clang

#endif
