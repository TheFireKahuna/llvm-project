//===- Binding.h - Cross-image symbol ownership -------------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_BINDING_H
#define LLD_COFF_BINDING_H

#include "Chunks.h"
#include "llvm/Support/ABIContract.h"

namespace lld::coff {
class BindingProvider;

bool sameImportedTarget(DefinedImportData *a, DefinedImportData *b);
Symbol *getBindingTarget(Symbol *requirement);
bool isReplaceableBinding(DefinedRegular *symbol);
DefinedRegular *getCompilerStubTarget(SymbolTable &symtab, Symbol *symbol);
Symbol *makeImportAlias(DefinedRegular *symbol, DefinedImportData *import,
                        bool pointee);

struct ABIContract {
  StringRef bytes;
  StringRef digest;
  llvm::abi::Contract graph;
  InputFile *origin;
};

// Ownership is side information on the existing resolved symbol, not another
// symbol resolver. In-place symbol replacement must not discard this decision.
struct BindingEntity {
  enum Owner { Unresolved, Published, Imported, Private, Generated };

  Symbol *symbol;
  Symbol *requirement;
  StringRef identity;
  InputFile *origin;
  uint32_t flags;
  // Original requirements remain separate even when ordinary conditional
  // alias resolution currently selects the same symbol. In particular, a
  // provider offered for an alias cannot be hidden by its fallback's name.
  SmallVector<Symbol *, 2> requirements;
  Owner owner = Unresolved;
  DefinedImportData *import = nullptr;
  BindingProvider *provider = nullptr;
  bool hasNamedExport = false;
  ABIContract *contract = nullptr;
  StringRef lookupName;
};

// A provider is an output view of a selected immutable contribution. The input
// bytes and native dependency edges come from the jointly resolved graph; no
// archive search or symbol resolution is repeated to write this output.
class BindingProvider : public NonSectionChunk {
public:
  struct Reference {
    uint32_t offset;
    DefinedImportData *target;
  };

  BindingProvider(BindingEntity &entity, StringRef dllName)
      : entity(entity), dllName(dllName) {}
  size_t getSize() const override { return contents.size(); }
  void writeTo(uint8_t *buf) const override;

  BindingEntity &entity;
  StringRef dllName;
  ArrayRef<uint8_t> contents;
  SectionChunk *contribution = nullptr;
  SmallVector<Reference, 4> references;
  bool live = false;
};

} // namespace lld::coff

#endif
