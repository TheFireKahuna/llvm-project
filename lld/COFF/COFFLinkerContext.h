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
struct LoadClosure;

// A run of the vtables of one class and the classes derived from it through
// primary bases, laid out as consecutive stride-byte slots: a vtable pointer is
// in the run exactly when its offset from the start is a multiple of the stride
// below the extent. Each slot holds one vtable, address point apoff bytes in.
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
  // Each class the run covers, with its own slot's index and the number of
  // consecutive slots its subtree spans (a subtree is contiguous in slot
  // order).
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
  // a class and the classes derived through primary bases, as consecutive
  // slots. runChunkRun maps each vtable in a run to its run index and slot.
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

  // The runs of imported DLLs the image's checks test, by the symbol they read
  // through and the imports of the three words the IAT holds consecutively; and
  // the zero words a class whose owner the image does not bind reads instead.
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

  // Each import library member's vague-entity record, parsed once and keyed by
  // the member's buffer, so the passes read it without a reparse.
  llvm::DenseMap<const char *, LoadClosure *> vagueRecordCache;

  // As a member of a joint link: the members, in lowercase; the image's
  // identity export, whose name its final layout decides, and the exports it
  // lists with their addresses; and the loads of the import pointers of data
  // that the image makes lea of the distance from another load's register,
  // each with its site, import, distance and register.
  llvm::StringSet<> jointMembers;
  std::string *jointIdentity = nullptr;
  std::vector<std::pair<llvm::StringRef, uint32_t>> jointLayout;
  struct JointAnchor {
    SectionChunk *chunk;
    uint32_t offset;
    DefinedImportData *imp;
    int32_t delta;
    uint8_t reg;
  };
  std::vector<JointAnchor> jointAnchors;
  // The classes whose vtable runs the image exports for other members, each
  // with the name of the run's start that they import.
  llvm::StringMap<llvm::StringRef> jointRuns;
  // The functions the image exports that other members take the address of,
  // which it does not suppress; and the functions other members so export
  // that the image takes, by DLL and name, and their imports, whose entries
  // the loader need not grant.
  llvm::StringSet<> jointUnsuppressed;
  std::vector<std::pair<llvm::StringRef, llvm::StringRef>> jointGrants;
  llvm::DenseSet<DefinedImportData *> jointGranted;

  // The changes the linker makes to the code of each section chunk it
  // changes, and how it lays out the code of each it resizes.
  llvm::DenseMap<const SectionChunk *, Relaxation> relaxations;
  // The chunks whose functions run into them from a function whose jump to
  // them the image leaves out.
  llvm::DenseSet<const Chunk *> tailJumpTargets;

  // The state of the link's KCFI checks.
  KCFIState kcfi;

  // The ABI contracts of the link and their witnesses.
  ContractState contracts;
};

} // namespace lld::coff

#endif
