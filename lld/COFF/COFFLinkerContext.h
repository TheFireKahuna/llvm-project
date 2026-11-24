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
#include "PDB.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Timer.h"
#include <atomic>

namespace lld::coff {

class COFFLinkerContext : public CommonLinkerContext {
public:
  COFFLinkerContext();
  COFFLinkerContext(const COFFLinkerContext &) = delete;
  COFFLinkerContext &operator=(const COFFLinkerContext &) = delete;
  ~COFFLinkerContext() = default;

  LinkerDriver driver;
  SymbolTable symtab;
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
    // If present, process the native symbol table first.
    if (hybridSymtab)
      f(*hybridSymtab);
    f(symtab);
  }

  // Invoke the specified callback for each active symbol table,
  // skipping the native symbol table on pure ARM64EC targets.
  void forEachActiveSymtab(std::function<void(SymbolTable &symtab)> f) {
    if (symtab.ctx.config.machine == ARM64X)
      f(*hybridSymtab);
    f(symtab);
  }

  std::vector<ObjFile *> objFileInstances;
  // True once a file with InputFile::forkObject is part of the link.
  bool hasForkObject = false;
  // The first file a link fact of which gives a function a type prefix.
  InputFile *typePrefixFile = nullptr;
  // SECTION-class symbols that name a section another object defines.
  std::vector<DefinedRegular *> sectionSymbols;
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

  OutputSection *getOutputSection(const Chunk *c) const {
    return c->osidx == 0 ? nullptr : outputSections[c->osidx - 1];
  }

  // The first chunks of the first and the last TLS group (.tls, .tls$*), and
  // the last chunk of the last.
  Chunk *tlsFirst = nullptr;
  Chunk *tlsLast = nullptr;
  Chunk *tlsEnd = nullptr;

  // An RVA's offset from the first TLS group if it lies in the TLS groups.
  // A SECREL's bound is link.exe's: past the first group's start and before
  // the last group's. The inclusive bound, from the first group's start to
  // the last group's end, needs no marker section around the variables.
  std::optional<uint64_t> tlsOffset(uint64_t rva,
                                    bool inclusive = false) const {
    if (!tlsFirst ||
        (inclusive ? rva < tlsFirst->getRVA() ||
                         rva > tlsEnd->getRVA() + tlsEnd->getSize()
                   : rva <= tlsFirst->getRVA() || rva >= tlsLast->getRVA()))
      return std::nullopt;
    return rva - tlsFirst->getRVA();
  }

  // Whether the image is of the thread-local model, "pe" or "ntposix", that
  // its objects state in their _TLS_MODEL key.
  bool isTlsModel(StringRef model) const {
    auto it = config.mustMatch.find("_TLS_MODEL");
    return it != config.mustMatch.end() &&
           it->second.first.split('.').first == model;
  }

  // The TLS groups' base in a thread is tlsScale times the thread pointer
  // plus the image's place, which the link knows for some images. An
  // executable of the ntposix model whose thread-locals exceed tlsSlotSize
  // has none; tlsOversize is their size, which its first TPOFF reports.
  static constexpr uint64_t tlsSlotSize = 1024;
  std::optional<uint64_t> tlsPlace;
  uint32_t tlsScale = 1;
  uint64_t tlsOversize = 0;
  std::atomic<bool> tlsOversizeReported = false;

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
