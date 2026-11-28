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
#include "Walker.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Timer.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include <atomic>
#include <mutex>

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
  // Classifies the references of code that its object does not type.
  Walker walker;
  // True once a file with InputFile::forkObject is part of the link.
  bool hasForkObject = false;
  // The function overrides of such files, by their default bodies.
  llvm::DenseMap<const Symbol *, FunctionOverride *> functionOverrideBodies;
  // Whether the link moves the code of a live section, whose size may then
  // depend on the layout.
  bool hasMovingCode = false;
  // The first file a link fact of which gives a function a type prefix.
  InputFile *typePrefixFile = nullptr;
  // SECTION-class symbols that name a section another object defines.
  std::vector<DefinedRegular *> sectionSymbols;
  std::map<std::string, PDBInputFile *> pdbInputFileInstances;
  std::vector<ImportFile *> importFileInstances;
  // The words of data that the loader binds in place, each section's together
  // and in the order of their offsets; once sections are laid out, run by run.
  std::vector<ImportSlot> importSlots;
  // The words of data that hold the address of a delay-loaded function which
  // is its DLL's own, each section's together and in the order of their
  // offsets.
  std::vector<ImportSlot> delayWords;
  // Each delay-bound section's words that hold an address of a delay-loaded
  // function or in such a section, and the DLLs whose gates guard it.
  struct DelayBound {
    llvm::SmallSetVector<StringRef, 1> dlls;
    SmallVector<uint32_t, 2> words;
  };
  llvm::DenseMap<const Chunk *, DelayBound> delayBound;
  // The leaves of delay-load guards, by what a site loads, its register with
  // 256 where it loads an address, and the instruction a leaf replays.
  llvm::MapVector<std::tuple<Symbol *, int32_t, unsigned, StringRef>,
                  Defined *>
      delayGuards;
  // The ARM64 guards, site by site, each placed after its section.
  std::vector<Chunk *> delayGuardsA64;
  // What the image writes for itself before any other of its code runs, in a
  // list that the runtime's fill walks.
  struct {
    // The words of data that hold the address of an import plus an offset
    // that the import's DLL exports no name for, the image's sealed values,
    // the pointers of the imports that it binds by name, and the words that
    // hold the address of an overridable default.
    std::vector<FillWord> words;
    // The read-only output sections whose words it writes where they are.
    std::vector<OutputSection *> ranges;
    bool empty() const { return words.empty(); }
  } fill;
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

  // The image's thread-local template, its .tls section, and the loader's,
  // the run of the foreign objects' .tls groups, each as its first chunk and
  // its bound: the image's last chunk, whose end it reaches, and the first
  // chunk of the loader's last group, link.exe's bound, which it does not.
  Chunk *tlsImage[2] = {};
  Chunk *tlsLoader[2] = {};

  // An RVA's offset from the start of the image's template, or with loader
  // of the loader's, if that template holds it.
  std::optional<uint64_t> tlsOffset(uint64_t rva, bool loader = false) const {
    Chunk *const *t = loader ? tlsLoader : tlsImage;
    if (!t[0] || rva < t[0]->getRVA() ||
        (loader ? rva >= t[1]->getRVA()
                : rva > t[1]->getRVA() + t[1]->getSize()))
      return std::nullopt;
    return rva - t[0]->getRVA();
  }

  // The chunks at which the link defines _tls_start and _tls_end where no
  // foreign object does.
  Chunk *tlsBounds[2] = {};

  // The thread-locals reached across the two templates, each reported once.
  std::mutex tlsMismatchMutex;
  llvm::DenseSet<Symbol *> tlsMismatched;

  // Whether every offset in the TLS groups fits the 16 bits of an ARM64
  // movz, which the scan finds from the sizes of their sections.
  bool tlsFitsMovz = false;

  // The runtime that owns the image's thread-local model, which its objects
  // state in their _TLS_MODEL key, "ntposix" or "wincrt", and so the image's
  // environment; empty where no object states the model.
  StringRef tlsRuntime() const {
    auto it = config.mustMatch.find("_TLS_MODEL");
    return it == config.mustMatch.end() ? StringRef()
                                        : it->second.first.split('.').first;
  }

  // The image's template's size, whether its form is wide, which it is above
  // a block, and its place where the link knows it.
  uint64_t tlsSize = 0;
  bool tlsWide = false;
  std::optional<uint64_t> tlsPlace;
  std::atomic<bool> tlsWideReported = false;

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
