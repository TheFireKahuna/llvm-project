//===- Partitions.h - Final PE output views -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_PARTITIONS_H
#define LLD_COFF_PARTITIONS_H

#include "Chunks.h"
#include "SymbolTable.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/MapVector.h"

namespace lld::coff {
class DefinedSynthetic;

struct OutputPartition {
  OutputPartition(COFFLinkerContext &ctx, StringRef name, unsigned index);
  StringRef name;
  StringRef dllName;
  unsigned index;
  SymbolTable symbols;
  std::vector<Chunk *> chunks;
  std::vector<ImportFile *> imports;
  llvm::DenseSet<Chunk *> imageChunks;
  std::array<MergeChunk *, Log2MaxSectionAlignment + 1> merges = {};
  llvm::DenseMap<Symbol *, Symbol *> projections;
  llvm::DenseMap<ImportFile *, ImportFile *> importViews;
  llvm::DenseMap<Defined *, StringRef> exportNames;
};

// Deployment partitions are views of one resolved graph. No archive search,
// COMDAT selection or weak-alias resolution takes place in an output view.
class Partitioning {
public:
  explicit Partitioning(COFFLinkerContext &ctx) : ctx(ctx) {}
  void prepare();
  void prepareLTO();
  void assign();
  bool empty() const { return outputs.empty(); }
  unsigned owner(const Chunk *chunk) const;
  bool isRoot(Symbol *symbol) const { return roots.contains(symbol); }
  void freeze(Chunk *chunk, StringRef output) { frozenOwners[chunk] = output; }
  bool isImageLocal(const Chunk *chunk) const {
    return imageTemplates.contains(chunk);
  }
  void activate(OutputPartition &output);
  void deactivate();
  void setDebugView(bool debug);
  Symbol *originalSymbol(ObjFile *file, unsigned index) const;
  std::vector<OutputPartition *> outputs;

private:
  void collectRoots(InputFile &file);
  void collectPlacementConstraints();
  void placeSharedPrivateEntities();
  const Chunk *leader(const Chunk *chunk) const;
  void enqueue(Chunk *chunk, unsigned partition);
  void visit(Symbol *symbol, unsigned partition);
  void drain();
  Symbol *project(Symbol *symbol, OutputPartition &output);
  ImportFile *importView(ImportFile *file, OutputPartition &output);
  ImportFile *importDefinition(Defined *symbol, OutputPartition &output);
  void nameOutputs();
  void createViews();
  void normalizeExactReferences();
  void finalizePrivateImports();
  void createImageMetadata(OutputPartition &output);
  void instantiateImageRuntime(OutputPartition &output);
  void scopeMetadata(llvm::ArrayRef<SectionChunk *> chunks, unsigned index);
  void checkDependencies();

  COFFLinkerContext &ctx;
  llvm::MapVector<Symbol *, StringRef> roots;
  llvm::DenseMap<const Chunk *, unsigned> owners;
  llvm::DenseMap<const Chunk *, unsigned> fixed;
  llvm::DenseMap<Chunk *, StringRef> frozenOwners;
  llvm::DenseSet<DefinedSynthetic *> exactAliases;
  SmallVector<std::pair<ImportFile *, Defined *>, 16> privateImports;
  llvm::DenseSet<const Chunk *> imageTemplates;
  llvm::EquivalenceClasses<Chunk *> groups;
  SmallVector<Chunk *, 256> worklist;
  struct SavedFile {
    ObjFile *file;
    SmallVector<Symbol *, 0> symbols;
  };
  std::vector<SavedFile> savedFiles;
  llvm::DenseMap<ObjFile *, llvm::ArrayRef<Symbol *>> originalSymbols;
  std::vector<ImportFile *> savedImports;
  std::array<MergeChunk *, Log2MaxSectionAlignment + 1> savedMerges = {};
  llvm::DenseMap<SectionChunk *, bool> savedLiveness;
  llvm::DenseMap<CommonChunk *, bool> savedCommonLiveness;
};

bool canImportPartitionReference(SectionChunk &chunk,
                                 const llvm::object::coff_relocation &rel,
                                 Defined &target);

} // namespace lld::coff

#endif
