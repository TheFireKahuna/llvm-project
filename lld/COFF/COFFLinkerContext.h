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
#include "Contracts.h"
#include "DebugTypes.h"
#include "Driver.h"
#include "InputFiles.h"
#include "KCFI.h"
#include "PDB.h"
#include "Relax.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Timer.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace lld::coff {
struct TypePrefixLabels;
struct ParsedVagueRecord;

// A run of the vtables of one class and of the classes derived from it through
// primary bases, laid out as consecutive stride-byte slots so that a vtable
// pointer is one of the run's exactly when its offset from the run's start is a
// multiple of the stride below the run's extent. Each slot holds one vtable,
// whose primary address point is apoff bytes into its chunk.
struct VTableRun {
  uint64_t stride = 0;
  // The address point of the run's first slot, which assignAddresses sets and
  // the later slots are placed the stride apart from.
  uint64_t anchor = 0;
  // The read-only word holding the stride less one, which a guard thunk loads.
  Chunk *maskChunk = nullptr;
  struct Slot {
    SectionChunk *chunk;
    uint64_t apoff;
  };
  std::vector<Slot> slots;
  // Each class the run covers, with the index of its own slot and the number
  // of consecutive slots its subtree spans, since a subtree is contiguous in
  // the depth-first slot order.
  struct Class {
    std::string name;
    unsigned first;
    unsigned count;
  };
  std::vector<Class> classes;
};

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

  // The address each pinned chunk must start at.
  llvm::DenseMap<const Chunk *, ChunkPin> chunkPins;

  // The runs the vtable records ask the writer to lay out, each the vtables of
  // a class and of the classes derived from it through primary bases, placed
  // as consecutive slots. runChunkRun maps each vtable in a run to the run's
  // index and the vtable's slot in it.
  std::vector<VTableRun> vtableRuns;
  llvm::DenseMap<const Chunk *, std::pair<unsigned, unsigned>> runChunkRun;
  // The tagged vtables, which a required pin places, by the run of their
  // primary base, whose gaps the layout offers them first.
  llvm::DenseMap<const Chunk *, unsigned> taggedRun;

  // The runs a DLL exports, by class: the three words an importer reads, the
  // start, the end, and the start plus the stride less one, of which the
  // writer defines the last two once the runs are placed.
  struct VTableRunExport {
    std::string cls;
    Chunk *chunk;
    uint64_t apoff;
    StringRef names[3];
    Symbol *words[3];
  };
  std::vector<VTableRunExport> vtableRunExports;

  // The runs of DLLs the image imports that its checks test, by the symbol
  // they read them through and the imports of the three words, which the
  // import address table holds consecutively; and the zero words that a
  // class whose owner the image does not bind reads instead, an empty range.
  struct VTableOwnerRun {
    Symbol *sym;
    DefinedImportData *words[3];
  };
  std::vector<VTableOwnerRun> vtableOwnerRuns;
  Chunk *vtableOwnerEmpty = nullptr;

  // Whether an object says that its functions have KCFI prefixes with a
  // marker, which the writer then finds, seals and places.
  bool typePrefixRecords = false;

  // The KCFI prefixes with a marker that each object holds, which
  // TypePrefix.cpp finds the first time it is asked.
  llvm::DenseMap<ObjFile *, TypePrefixLabels *> typePrefixLabels;

  // The functions that /guardsym keeps listed as Control Flow Guard targets
  // but suppressed.
  std::vector<Defined *> guardSuppressed;

  // The in-place import slots of each section that has any, by offset, in
  // the order the sections were bound.
  llvm::MapVector<const SectionChunk *, std::vector<ImportSlot>> importSlots;

  // The instructions that take the address of an import, by section and the
  // offset of the relocation, which are rewritten to take it as static data
  // does (see SectionChunk::getImportSiteTarget).
  llvm::DenseSet<std::pair<const SectionChunk *, uint32_t>> importSites;

  // Each import library member's vague-entity record, parsed once and keyed by
  // the member's buffer, so the closures the passes build read it without a
  // reparse.
  llvm::DenseMap<const char *, ParsedVagueRecord *> vagueRecordCache;

  // The fused adds that a load takes the sum of, by section and the offset of
  // the load's relocation.
  llvm::DenseMap<std::pair<const SectionChunk *, uint32_t>,
                 const ObjFile::LinkFusion *>
      fusedAdds;

  // How the code of each covered section chunk the linker resizes is laid out.
  llvm::DenseMap<const SectionChunk *, Relaxation> relaxations;
  // The jumps that end their functions which the image leaves out, by chunk,
  // at the offset of each's displacement, and the chunks whose functions
  // those functions then run into.
  llvm::DenseMap<const SectionChunk *, uint32_t> tailJumps;
  llvm::DenseSet<const Chunk *> tailJumpTargets;

  // The state of the link's KCFI checks.
  KCFIState kcfi;

  // The ABI contracts of the link and their witnesses.
  ContractState contracts;
};

} // namespace lld::coff

#endif
