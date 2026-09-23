//===- COFFLinkerContext.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_COFFLINKERCONTEXT_H
#define LLD_COFF_COFFLINKERCONTEXT_H

#include "Chunks.h"
#include "Config.h"
#include "DebugTypes.h"
#include "Driver.h"
#include "InputFiles.h"
#include "OutputFiles.h"
#include "PDB.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Timer.h"

namespace lld::coff {

class Partitioning;
class ImportInstructionDecoder;
struct OutputPartition;

class COFFLinkerContext : public CommonLinkerContext {
public:
  COFFLinkerContext();
  COFFLinkerContext(const COFFLinkerContext &) = delete;
  COFFLinkerContext &operator=(const COFFLinkerContext &) = delete;
  ~COFFLinkerContext();

  LinkerDriver driver;
  SymbolTable symtab;
  std::unique_ptr<Partitioning> partitions;
  std::unique_ptr<ImportInstructionDecoder> importInstructionDecoder;
  SymbolTable *outputSymtab = nullptr;
  OutputPartition *outputPartition = nullptr;
  SymbolTable &getOutputSymtab() {
    return outputSymtab ? *outputSymtab : symtab;
  }
  COFFOptTable optTable;

  // A native ARM64 symbol table on ARM64X target.
  std::optional<SymbolTable> hybridSymtab;

  // Returns the appropriate symbol table for the specified machine type.
  SymbolTable &getSymtab(llvm::COFF::MachineTypes machine) {
    if (hybridSymtab && machine == ARM64)
      return *hybridSymtab;
    return symtab;
  }

  // Invoke the specified callback for each symbol table.
  void forEachSymtab(std::function<void(SymbolTable &symtab)> f) {
    if (outputSymtab) {
      f(*outputSymtab);
      return;
    }
    // If present, process the native symbol table first.
    if (hybridSymtab)
      f(*hybridSymtab);
    f(symtab);
  }

  // Invoke the specified callback for each active symbol table,
  // skipping the native symbol table on pure ARM64EC targets.
  void forEachActiveSymtab(std::function<void(SymbolTable &symtab)> f) {
    if (outputSymtab) {
      f(*outputSymtab);
      return;
    }
    if (symtab.ctx.config.machine == ARM64X)
      f(*hybridSymtab);
    f(symtab);
  }

  std::vector<ObjFile *> objFileInstances;
  std::map<std::string, PDBInputFile *> pdbInputFileInstances;
  std::vector<ImportFile *> importFileInstances;
  std::int64_t consumedInputsSize = 0;

  MergeChunk *mergeChunkInstances[Log2MaxSectionAlignment + 1] = {};

  /// All sources of type information in the program.
  std::vector<TpiSource *> tpiSourceList;

  void addTpiSource(TpiSource *tpi) { tpiSourceList.push_back(tpi); }

  std::map<llvm::codeview::GUID, TpiSource *> typeServerSourceMappings;
  std::map<uint32_t, TpiSource *> precompSourceMappings;

  /// List of all output sections. After output sections are finalized, this
  /// can be indexed by getOutputSection.
  std::vector<OutputSection *> outputSections;

  // Context-owned so fatal diagnostics also discard staged output buffers
  // when LLD is invoked as a library or runs multiple test passes.
  std::vector<std::unique_ptr<llvm::FileOutputBuffer>> pendingOutputs;
  OutputFiles outputFiles;

  OutputSection *getOutputSection(const Chunk *c) const {
    return c->osidx == 0 ? nullptr : outputSections[c->osidx - 1];
  }

  // Fake sections for parsing bitcode files.
  FakeSection ltoTextSection;
  FakeSection ltoDataSection;
  FakeSectionChunk ltoTextSectionChunk;
  FakeSectionChunk ltoDataSectionChunk;

  // All timers used in the COFF linker.
  Timer rootTimer;
  Timer inputFileTimer;
  Timer ltoTimer;
  Timer gcTimer;
  Timer icfTimer;

  // Writer timers.
  Timer codeLayoutTimer;
  Timer outputCommitTimer;
  Timer totalMapTimer;
  Timer symbolGatherTimer;
  Timer symbolStringsTimer;
  Timer writeTimer;

  // PDB timers.
  Timer totalPdbLinkTimer;
  Timer addObjectsTimer;
  Timer typeMergingTimer;
  Timer loadGHashTimer;
  Timer mergeGHashTimer;
  Timer symbolMergingTimer;
  Timer publicsLayoutTimer;
  Timer tpiStreamLayoutTimer;
  Timer diskCommitTimer;

  std::optional<PDBStats> pdbStats;

  Configuration config;

  DynamicRelocsChunk *dynamicRelocs = nullptr;
};

} // namespace lld::coff

#endif
