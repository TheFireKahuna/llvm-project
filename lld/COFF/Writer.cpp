//===- Writer.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "Binding.h"
#include "Partitions.h"
#include "COFFLinkerContext.h"
#include "CallGraphSort.h"
#include "Config.h"
#include "DLL.h"
#include "InputFiles.h"
#include "LLDMapFile.h"
#include "MapFile.h"
#include "PDB.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Timer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/COFFBinding.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <utility>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::coff;

/* To re-generate DOSProgram:
$ cat > /tmp/DOSProgram.asm
org 0
        ; Copy cs to ds.
        push cs
        pop ds
        ; Point ds:dx at the $-terminated string.
        mov dx, str
        ; Int 21/AH=09h: Write string to standard output.
        mov ah, 0x9
        int 0x21
        ; Int 21/AH=4Ch: Exit with return code (in AL).
        mov ax, 0x4C01
        int 0x21
str:
        db 'This program cannot be run in DOS mode.$'
align 8, db 0
$ nasm -fbin /tmp/DOSProgram.asm -o /tmp/DOSProgram.bin
$ xxd -i /tmp/DOSProgram.bin
*/
static unsigned char dosProgram[] = {
  0x0e, 0x1f, 0xba, 0x0e, 0x00, 0xb4, 0x09, 0xcd, 0x21, 0xb8, 0x01, 0x4c,
  0xcd, 0x21, 0x54, 0x68, 0x69, 0x73, 0x20, 0x70, 0x72, 0x6f, 0x67, 0x72,
  0x61, 0x6d, 0x20, 0x63, 0x61, 0x6e, 0x6e, 0x6f, 0x74, 0x20, 0x62, 0x65,
  0x20, 0x72, 0x75, 0x6e, 0x20, 0x69, 0x6e, 0x20, 0x44, 0x4f, 0x53, 0x20,
  0x6d, 0x6f, 0x64, 0x65, 0x2e, 0x24, 0x00, 0x00
};
static_assert(sizeof(dosProgram) % 8 == 0,
              "DOSProgram size must be multiple of 8");
static_assert((sizeof(dos_header) + sizeof(dosProgram)) % 8 == 0,
              "DOSStub size must be multiple of 8");

static const int numberOfDataDirectory = 16;

namespace {

class DebugDirectoryChunk : public NonSectionChunk {
public:
  DebugDirectoryChunk(const COFFLinkerContext &c,
                      const std::vector<std::pair<COFF::DebugType, Chunk *>> &r,
                      bool writeRepro)
      : records(r), writeRepro(writeRepro), ctx(c) {}

  size_t getSize() const override {
    return (records.size() + int(writeRepro)) * sizeof(debug_directory);
  }

  void writeTo(uint8_t *b) const override {
    auto *d = reinterpret_cast<debug_directory *>(b);

    for (const std::pair<COFF::DebugType, Chunk *>& record : records) {
      Chunk *c = record.second;
      const OutputSection *os = ctx.getOutputSection(c);
      uint64_t offs = os->getFileOff() + (c->getRVA() - os->getRVA());
      fillEntry(d, record.first, c->getSize(), c->getRVA(), offs);
      ++d;
    }

    if (writeRepro) {
      // FIXME: The COFF spec allows either a 0-sized entry to just say
      // "the timestamp field is really a hash", or a 4-byte size field
      // followed by that many bytes containing a longer hash (with the
      // lowest 4 bytes usually being the timestamp in little-endian order).
      // Consider storing the full 8 bytes computed by xxh3_64bits here.
      fillEntry(d, COFF::IMAGE_DEBUG_TYPE_REPRO, 0, 0, 0);
    }
  }

  void setTimeDateStamp(uint32_t timeDateStamp) {
    for (support::ulittle32_t *tds : timeDateStamps)
      *tds = timeDateStamp;
  }

private:
  void fillEntry(debug_directory *d, COFF::DebugType debugType, size_t size,
                 uint64_t rva, uint64_t offs) const {
    d->Characteristics = 0;
    d->TimeDateStamp = 0;
    d->MajorVersion = 0;
    d->MinorVersion = 0;
    d->Type = debugType;
    d->SizeOfData = size;
    d->AddressOfRawData = rva;
    d->PointerToRawData = offs;

    timeDateStamps.push_back(&d->TimeDateStamp);
  }

  mutable std::vector<support::ulittle32_t *> timeDateStamps;
  const std::vector<std::pair<COFF::DebugType, Chunk *>> &records;
  bool writeRepro;
  const COFFLinkerContext &ctx;
};

class CVDebugRecordChunk : public NonSectionChunk {
public:
  CVDebugRecordChunk(const COFFLinkerContext &c) : ctx(c) {}

  size_t getSize() const override {
    return sizeof(codeview::DebugInfo) + ctx.config.pdbAltPath.size() + 1;
  }

  void writeTo(uint8_t *b) const override {
    // Save off the DebugInfo entry to backfill the file signature (build id)
    // in Writer::writeBuildId
    buildId = reinterpret_cast<codeview::DebugInfo *>(b);

    // variable sized field (PDB Path)
    char *p = reinterpret_cast<char *>(b + sizeof(*buildId));
    if (!ctx.config.pdbAltPath.empty())
      memcpy(p, ctx.config.pdbAltPath.data(), ctx.config.pdbAltPath.size());
    p[ctx.config.pdbAltPath.size()] = '\0';
  }

  mutable codeview::DebugInfo *buildId = nullptr;

private:
  const COFFLinkerContext &ctx;
};

class ExtendedDllCharacteristicsChunk : public NonSectionChunk {
public:
  ExtendedDllCharacteristicsChunk(uint32_t c) : characteristics(c) {}

  size_t getSize() const override { return 4; }

  void writeTo(uint8_t *buf) const override { write32le(buf, characteristics); }

  uint32_t characteristics = 0;
};

// PartialSection represents a group of chunks that contribute to an
// OutputSection. Collating a collection of PartialSections of same name and
// characteristics constitutes the OutputSection.
class PartialSectionKey {
public:
  StringRef name;
  unsigned characteristics;

  bool operator<(const PartialSectionKey &other) const {
    int c = name.compare(other.name);
    if (c > 0)
      return false;
    if (c == 0)
      return characteristics < other.characteristics;
    return true;
  }
};

struct ChunkRange {
  Chunk *first = nullptr, *last;
};

// The writer writes a SymbolTable result to a file.
class Writer {
public:
  Writer(COFFLinkerContext &c)
      : buffer(c.e.outputBuffer), strtab(StringTableBuilder::WinCOFF),
        delayIdata(c), ctx(c) {}
  void run();
  void runProvider(BindingProvider &provider, SymbolTable &symbols);

private:
  void calculateStubDependentSizes();
  void createSections();
  void createMiscChunks();
  void createImportTables();
  void appendImportThunks();
  void locateImportTables();
  void createExportTable();
  void mergeSection(const std::map<StringRef, StringRef>::value_type &p);
  void mergeSections();
  void sortECChunks();
  void appendECImportTables();
  void removeUnusedSections();
  void layoutSections();
  void assignAddresses();
  bool isInRange(uint16_t relType, uint64_t s, uint64_t p, int margin,
                 MachineTypes machine);
  std::pair<Defined *, bool> getThunk(DenseMap<uint64_t, Defined *> &lastThunks,
                                      Defined *target, uint64_t p,
                                      uint16_t type, int margin,
                                      MachineTypes machine);
  bool createThunks(OutputSection *os, int margin);
  bool verifyRanges(const std::vector<Chunk *> chunks);
  void createECCodeMap();
  void finalizeAddresses();
  void removeEmptySections();
  void assignOutputSectionIndices();
  void createSymbolAndStringTable();
  void openFile(StringRef outputPath);
  template <typename PEHeaderTy> void writeHeader();
  void createSEHTable();
  void createRuntimePseudoRelocs();
  bool findImportSlots(SectionChunk *sc, StringRef name);
  void addImportSlots();
  void selectImportStorage();
  void validateImportStorage();
  uint64_t getIATSize() const;
  void createImportFill();
  void createWeakPublishTable();
  void dropWeakInterposition();
  void padWeakInterposition(uint8_t *buf);
  void collectImportFuseCalls();
  void unfuseImportCalls(uint8_t *buf);
  void disposeOfDeadBytes(uint8_t *buf);
  void createECChunks();
  void insertCtorDtorSymbols();
  void insertBssDataStartEndSymbols();
  void markSymbolsWithRelocations(ObjFile *file, SymbolRVASet &usedSymbols,
                                  SymbolRVASet &usedImports);
  void createGuardCFTables();
  bool protectDelayIat() {
    return ctx.config.delayLoadProtect && !delayIdata.empty();
  }
  void markSymbolsForRVATable(ObjFile *file,
                              ArrayRef<SectionChunk *> symIdxChunks,
                              SymbolRVASet &tableSymbols);
  void getSymbolsFromSections(ObjFile *file,
                              ArrayRef<SectionChunk *> symIdxChunks,
                              std::vector<Symbol *> &symbols);
  void maybeAddRVATable(SymbolRVASet tableSymbols, StringRef tableSym,
                        StringRef countSym, bool hasFlag = false,
                        SymbolRVASet exportSuppressed = {});
  void setSectionPermissions();
  void validateRTTIBindings();
  void setECSymbols();
  void writeSections();
  void writeBuildId();
  void writePEChecksum();
  void sortSections();
  template <typename T> void sortExceptionTable(ChunkRange &exceptionTable);
  void sortExceptionTables();
  void sortCRTSectionChunks(std::vector<Chunk *> &chunks);
  void addSyntheticIdata();
  void sortBySectionOrder(std::vector<Chunk *> &chunks);
  void fixPartialSectionChars(StringRef name, uint32_t chars);
  bool fixGnuImportChunks();
  void fixTlsAlignment();
  PartialSection *createPartialSection(StringRef name, uint32_t outChars);
  PartialSection *findPartialSection(StringRef name, uint32_t outChars);

  std::optional<coff_symbol16> createSymbol(Defined *d);
  size_t addEntryToStringTable(StringRef str);

  OutputSection *findSection(StringRef name);
  void addBaserels();
  void addBaserelBlocks(std::vector<Baserel> &v);
  void createDynamicRelocs();

  uint32_t getSizeOfInitializedData();

  void prepareLoadConfig();
  template <typename T>
  void prepareLoadConfig(SymbolTable &symtab, T *loadConfig);

  void printSummary();

  std::unique_ptr<FileOutputBuffer> &buffer;
  std::map<PartialSectionKey, PartialSection *> partialSections;
  StringTableBuilder strtab;
  std::vector<llvm::object::coff_symbol16> outputSymtab;
  std::vector<ECCodeMapEntry> codeMap;
  IdataContents idata;
  Chunk *importTableStart = nullptr;
  uint64_t importTableSize = 0;
  Chunk *iatStart = nullptr;
  Chunk *iatEnd = nullptr;
  // In-place import slots (SectionChunk::getImportSlots), and the read-only
  // chunks holding them, which are laid out with the import address table,
  // keyed by the DLL order of their first slot.
  std::vector<ImportSlot> importSlots;
  llvm::DenseMap<SectionChunk *, int> iatSlotChunks;
  // Slots whose word holds an offset, which ImportFillChunk writes.
  std::vector<ImportSlot> filledSlots;
  ImportFillChunk *importFill = nullptr;
  DelayLoadContents delayIdata;
  bool setNoSEHCharacteristic = false;
  uint32_t tlsAlignment = 0;

  DebugDirectoryChunk *debugDirectory = nullptr;
  std::vector<std::pair<COFF::DebugType, Chunk *>> debugRecords;
  CVDebugRecordChunk *buildId = nullptr;
  ArrayRef<uint8_t> sectionTable;

  // List of Arm64EC export thunks.
  std::vector<std::pair<Chunk *, Defined *>> exportThunks;

  uint64_t fileSize;
  uint32_t pointerToSymbolTable = 0;
  uint64_t sizeOfImage;
  uint64_t sizeOfHeaders;

  uint32_t dosStubSize;
  uint32_t coffHeaderOffset;
  uint32_t peHeaderOffset;
  uint32_t dataDirOffset64;

  OutputSection *textSec;
  OutputSection *wowthkSec;
  OutputSection *hexpthkSec;
  OutputSection *bssSec;
  OutputSection *rdataSec;
  OutputSection *buildidSec;
  OutputSection *cvinfoSec;
  OutputSection *dataSec;
  OutputSection *pdataSec;
  OutputSection *idataSec;
  OutputSection *edataSec;
  OutputSection *didatSec;
  OutputSection *a64xrmSec;
  OutputSection *wkpubSec;

  // The entries this link replaces with padding (dropWeakInterposition), as
  // the chunk each one begins and its size.
  struct DeadInterposeEntry {
    SectionChunk *chunk;
    uint32_t offset;
    uint32_t size;
    bool bodyIsBranchTarget;
    uint32_t nextSize;  // zero when nothing was found that could carry it
    bool nextTransfers;
  };
  std::vector<DeadInterposeEntry> deadInterposeEntries;

  // The calls this link turns back into a call through a register
  // (collectImportFuseCalls), as the chunk each one is in, where it begins,
  // the register the compiler left the pointer in, and whether it is a jump.
  struct ImportFuseCall {
    SectionChunk *chunk;
    uint32_t offset;
    uint8_t reg;
    bool isJump;
  };
  std::vector<ImportFuseCall> importFuseCalls;

  // A run of bytes this link has learned nothing executes, with what is
  // known of the instruction that follows it. Where that instruction can take
  // the run as prefixes it costs nothing at all, since prefixes are decoded
  // as part of it; where it cannot, the run becomes a single no-operation,
  // which is one instruction however long the run is and a quarter of the
  // cost of the instruction it replaced. Nothing moves either way.
  struct DeadBytes {
    SectionChunk *chunk;
    uint32_t offset;
    uint32_t size;
    uint32_t nextSize;  // zero when nothing was found that could carry it
    bool nextTransfers; // that instruction is a branch, call or return
  };
  std::vector<DeadBytes> deadBytes;
  OutputSection *rsrcSec;
  OutputSection *relocSec;
  OutputSection *ctorsSec;
  OutputSection *dtorsSec;
  // Either .rdata section or .buildid section.
  OutputSection *debugInfoSec;

  // The range of .pdata sections in the output file.
  //
  // We need to keep track of the location of .pdata in whichever section it
  // gets merged into so that we can sort its contents and emit a correct data
  // directory entry for the exception table. This is also the case for some
  // other sections (such as .edata) but because the contents of those sections
  // are entirely linker-generated we can keep track of their locations using
  // the chunks that the linker creates. All .pdata chunks come from input
  // files, so we need to keep track of them separately.
  ChunkRange pdata;

  // x86_64 .pdata sections on ARM64EC/ARM64X targets.
  ChunkRange hybridPdata;

  // CHPE metadata symbol on ARM64C target.
  DefinedRegular *chpeSym = nullptr;

  COFFLinkerContext &ctx;
  SymbolTable *providerSymbols = nullptr;
};
} // anonymous namespace

static std::string partitionAuxPath(StringRef mainPath, StringRef dllName,
                                    StringRef extension) {
  if (mainPath.empty())
    return {};
  SmallString<256> path(sys::path::parent_path(mainPath));
  sys::path::append(path, sys::path::stem(dllName));
  sys::path::replace_extension(path, extension);
  return path.str().str();
}

void lld::coff::writeResult(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Write output(s)");
  if (ctx.partitions && !ctx.partitions->empty()) {
    std::string mainPath = ctx.config.outputFile;
    std::string mainImportName = ctx.config.importName;
    bool mainDLL = ctx.config.dll;
    bool mainNoEntry = ctx.config.noEntry;
    uint64_t mainBase = ctx.config.imageBase;
    SmallString<128> mainPDBPath = ctx.config.pdbPath;
    SmallString<128> mainPDBAltPath = ctx.config.pdbAltPath;
    std::string mainMapPath = ctx.config.mapFile;
    std::string mainLLDMapPath = ctx.config.lldmapFile;
    for (OutputPartition *output : drop_begin(ctx.partitions->outputs)) {
      SmallString<256> path(sys::path::parent_path(mainPath));
      sys::path::append(path, output->dllName);
      ctx.config.outputFile = path.str().str();
      ctx.config.importName = output->dllName.str();
      ctx.config.dll = true;
      ctx.config.noEntry = !output->symbols.entry;
      ctx.config.imageBase = ctx.config.is64() ? 0x180000000 : 0x10000000;
      ctx.config.pdbPath = partitionAuxPath(mainPDBPath, output->dllName, ".pdb");
      ctx.config.pdbAltPath =
          partitionAuxPath(mainPDBAltPath, output->dllName, ".pdb");
      ctx.config.mapFile = partitionAuxPath(mainMapPath, output->dllName, ".map");
      ctx.config.lldmapFile =
          partitionAuxPath(mainLLDMapPath, output->dllName, ".lldmap");
      ctx.outputSections.clear();
      ctx.partitions->activate(*output);
      Writer(ctx).run();
      ctx.partitions->deactivate();
      if (errorCount())
        return;
      ctx.pendingOutputs.push_back(std::move(ctx.e.outputBuffer));
    }
    ctx.config.outputFile = std::move(mainPath);
    ctx.config.importName = std::move(mainImportName);
    ctx.config.dll = mainDLL;
    ctx.config.noEntry = mainNoEntry;
    ctx.config.imageBase = mainBase;
    ctx.config.pdbPath = std::move(mainPDBPath);
    ctx.config.pdbAltPath = std::move(mainPDBAltPath);
    ctx.config.mapFile = std::move(mainMapPath);
    ctx.config.lldmapFile = std::move(mainLLDMapPath);
    ctx.outputSections.clear();
    ctx.partitions->activate(*ctx.partitions->outputs.front());
  }
  Writer(ctx).run();
  if (ctx.outputPartition)
    ctx.partitions->deactivate();
  if (errorCount())
    return;

  // Stage every output before publishing any of them. Provider output views
  // share semantic decisions with the main link, but never its IAT locations,
  // sections, entry point, TLS or application-specific linker configuration.
  auto &outputs = ctx.pendingOutputs;
  size_t mainOutput = outputs.size();
  outputs.push_back(std::move(ctx.e.outputBuffer));
  Configuration &mainConfig = *make<Configuration>(std::move(ctx.config));
  auto &mainSections =
      *make<std::vector<OutputSection *>>(std::move(ctx.outputSections));
  for (BindingProvider *provider : ctx.getOutputSymtab().bindingProviders) {
    if (!provider->live)
      continue;
    ctx.config = Configuration{};
    ctx.config.machine = mainConfig.machine;
    ctx.config.wordsize = mainConfig.wordsize;
    ctx.config.dll = true;
    ctx.config.noEntry = true;
    ctx.config.subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    ctx.config.majorOSVersion = mainConfig.majorOSVersion;
    ctx.config.majorSubsystemVersion = mainConfig.majorSubsystemVersion;
    ctx.config.imageBase = mainConfig.is64() ? 0x180000000 : 0x10000000;
    ctx.config.largeAddressAware = true;
    ctx.config.highEntropyVA = mainConfig.is64();
    SmallString<256> path(sys::path::parent_path(mainConfig.outputFile));
    sys::path::append(path, provider->dllName);
    ctx.config.outputFile = path.str().str();
    ctx.outputSections.clear();
    SymbolTable symbols(ctx, mainConfig.machine);
    Writer(ctx).runProvider(*provider, symbols);
    outputs.push_back(std::move(ctx.e.outputBuffer));
    if (errorCount())
      break;
  }
  ctx.config = std::move(mainConfig);
  ctx.outputSections = std::move(mainSections);
  if (errorCount())
    return;

  ScopedTimer t(ctx.outputCommitTimer);
  // Dependencies are ready before publishing the main image.
  for (size_t i = 0; i != outputs.size(); ++i) {
    size_t index = i + 1 == outputs.size() ? mainOutput
                    : i < mainOutput     ? i
                                         : i + 1;
    if (auto e = outputs[index]->commit())
      Fatal(ctx) << "failed to write output '" << outputs[index]->getPath()
                 << "': " << toString(std::move(e));
  }
  outputs.clear();
  ctx.outputFiles.publish(ctx.config.outputFile);
}

void OutputSection::addChunk(Chunk *c) {
  chunks.push_back(c);
}

void OutputSection::insertChunkAtStart(Chunk *c) {
  chunks.insert(chunks.begin(), c);
}

void OutputSection::setPermissions(uint32_t c) {
  header.Characteristics &= ~permMask;
  header.Characteristics |= c;
}

void OutputSection::merge(OutputSection *other) {
  chunks.insert(chunks.end(), other->chunks.begin(), other->chunks.end());
  other->chunks.clear();
  contribSections.insert(contribSections.end(), other->contribSections.begin(),
                         other->contribSections.end());
  other->contribSections.clear();

  // MS link.exe compatibility: when merging a code section into a data section,
  // mark the target section as a code section.
  if (other->header.Characteristics & IMAGE_SCN_CNT_CODE) {
    header.Characteristics |= IMAGE_SCN_CNT_CODE;
    header.Characteristics &=
        ~(IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_CNT_UNINITIALIZED_DATA);
  }
}

// Write the section header to a given buffer.
void OutputSection::writeHeaderTo(uint8_t *buf, bool isDebug) {
  auto *hdr = reinterpret_cast<coff_section *>(buf);
  *hdr = header;
  if (stringTableOff) {
    // If name is too long, write offset into the string table as a name.
    encodeSectionName(hdr->Name, stringTableOff);
  } else {
    assert(!isDebug || name.size() <= COFF::NameSize ||
           (hdr->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0);
    strncpy(hdr->Name, name.data(),
            std::min(name.size(), (size_t)COFF::NameSize));
  }
}

void OutputSection::addContributingPartialSection(PartialSection *sec) {
  contribSections.push_back(sec);
}

void OutputSection::splitECChunks() {
  llvm::stable_sort(chunks, [=](const Chunk *a, const Chunk *b) {
    return (a->getMachine() != ARM64) < (b->getMachine() != ARM64);
  });
}

// Check whether the target address S is in range from a relocation
// of type relType at address P.
bool Writer::isInRange(uint16_t relType, uint64_t s, uint64_t p, int margin,
                       MachineTypes machine) {
  if (machine == ARMNT) {
    int64_t diff = AbsoluteDifference(s, p + 4) + margin;
    switch (relType) {
    case IMAGE_REL_ARM_BRANCH20T:
      return isInt<21>(diff);
    case IMAGE_REL_ARM_BRANCH24T:
    case IMAGE_REL_ARM_BLX23T:
      return isInt<25>(diff);
    default:
      return true;
    }
  } else if (isAnyArm64(machine)) {
    int64_t diff = AbsoluteDifference(s, p) + margin;
    switch (relType) {
    case IMAGE_REL_ARM64_BRANCH26:
      return isInt<28>(diff);
    case IMAGE_REL_ARM64_BRANCH19:
      return isInt<21>(diff);
    case IMAGE_REL_ARM64_BRANCH14:
      return isInt<16>(diff);
    default:
      return true;
    }
  } else {
    return true;
  }
}

// Return the last thunk for the given target if it is in range,
// or create a new one.
std::pair<Defined *, bool>
Writer::getThunk(DenseMap<uint64_t, Defined *> &lastThunks, Defined *target,
                 uint64_t p, uint16_t type, int margin, MachineTypes machine) {
  Defined *&lastThunk = lastThunks[target->getRVA()];
  if (lastThunk && isInRange(type, lastThunk->getRVA(), p, margin, machine))
    return {lastThunk, false};
  Chunk *c;
  switch (getMachineArchType(machine)) {
  case Triple::thumb:
    c = make<RangeExtensionThunkARM>(ctx, target);
    break;
  case Triple::aarch64:
    c = make<RangeExtensionThunkARM64>(machine, target);
    break;
  default:
    llvm_unreachable("Unexpected architecture");
  }
  Defined *d = make<DefinedSynthetic>("range_extension_thunk", c);
  lastThunk = d;
  return {d, true};
}

// This checks all relocations, and for any relocation which isn't in range
// it adds a thunk after the section chunk that contains the relocation.
// If the latest thunk for the specific target is in range, that is used
// instead of creating a new thunk. All range checks are done with the
// specified margin, to make sure that relocations that originally are in
// range, but only barely, also get thunks - in case other added thunks makes
// the target go out of range.
//
// After adding thunks, we verify that all relocations are in range (with
// no extra margin requirements). If this failed, we restart (throwing away
// the previously created thunks) and retry with a wider margin.
bool Writer::createThunks(OutputSection *os, int margin) {
  bool addressesChanged = false;
  DenseMap<uint64_t, Defined *> lastThunks;
  DenseMap<std::pair<ObjFile *, Defined *>, uint32_t> thunkSymtabIndices;
  size_t thunksSize = 0;
  // Recheck Chunks.size() each iteration, since we can insert more
  // elements into it.
  for (size_t i = 0; i != os->chunks.size(); ++i) {
    SectionChunk *sc = dyn_cast<SectionChunk>(os->chunks[i]);
    if (!sc) {
      auto chunk = cast<NonSectionChunk>(os->chunks[i]);
      if (uint32_t size = chunk->extendRanges()) {
        thunksSize += size;
        addressesChanged = true;
      }
      continue;
    }
    MachineTypes machine = sc->getMachine();
    size_t thunkInsertionSpot = i + 1;

    // Try to get a good enough estimate of where new thunks will be placed.
    // Offset this by the size of the new thunks added so far, to make the
    // estimate slightly better.
    size_t thunkInsertionRVA = sc->getRVA() + sc->getSize() + thunksSize;
    ObjFile *file = sc->file;
    std::vector<std::pair<uint32_t, uint32_t>> relocReplacements;
    ArrayRef<coff_relocation> originalRelocs =
        file->getCOFFObj()->getRelocations(sc->header);
    for (size_t j = 0, e = originalRelocs.size(); j < e; ++j) {
      const coff_relocation &rel = originalRelocs[j];
      Symbol *relocTarget = file->getSymbol(rel.SymbolTableIndex);

      // The estimate of the source address P should be pretty accurate,
      // but we don't know whether the target Symbol address should be
      // offset by thunksSize or not (or by some of thunksSize but not all of
      // it), giving us some uncertainty once we have added one thunk.
      uint64_t p = sc->getRVA() + rel.VirtualAddress + thunksSize;

      Defined *sym = dyn_cast_or_null<Defined>(relocTarget);
      if (!sym)
        continue;

      uint64_t s = sym->getRVA();

      if (isInRange(rel.Type, s, p, margin, machine))
        continue;

      // If the target isn't in range, hook it up to an existing or new thunk.
      auto [thunk, wasNew] =
          getThunk(lastThunks, sym, p, rel.Type, margin, machine);
      if (wasNew) {
        Chunk *thunkChunk = thunk->getChunk();
        thunkChunk->setRVA(
            thunkInsertionRVA); // Estimate of where it will be located.
        os->chunks.insert(os->chunks.begin() + thunkInsertionSpot, thunkChunk);
        thunkInsertionSpot++;
        thunksSize += thunkChunk->getSize();
        thunkInsertionRVA += thunkChunk->getSize();
        addressesChanged = true;
      }

      // To redirect the relocation, add a symbol to the parent object file's
      // symbol table, and replace the relocation symbol table index with the
      // new index.
      auto insertion = thunkSymtabIndices.insert({{file, thunk}, ~0U});
      uint32_t &thunkSymbolIndex = insertion.first->second;
      if (insertion.second)
        thunkSymbolIndex = file->addSyntheticSymbol(thunk);
      relocReplacements.emplace_back(j, thunkSymbolIndex);
    }

    // Get a writable copy of this section's relocations so they can be
    // modified. If the relocations point into the object file, allocate new
    // memory. Otherwise, this must be previously allocated memory that can be
    // modified in place.
    ArrayRef<coff_relocation> curRelocs = sc->getRelocs();
    MutableArrayRef<coff_relocation> newRelocs;
    if (originalRelocs.data() == curRelocs.data()) {
      newRelocs = MutableArrayRef(
          bAlloc().Allocate<coff_relocation>(originalRelocs.size()),
          originalRelocs.size());
    } else {
      newRelocs = MutableArrayRef(
          const_cast<coff_relocation *>(curRelocs.data()), curRelocs.size());
    }

    // Copy each relocation, but replace the symbol table indices which need
    // thunks.
    auto nextReplacement = relocReplacements.begin();
    auto endReplacement = relocReplacements.end();
    for (size_t i = 0, e = originalRelocs.size(); i != e; ++i) {
      newRelocs[i] = originalRelocs[i];
      if (nextReplacement != endReplacement && nextReplacement->first == i) {
        newRelocs[i].SymbolTableIndex = nextReplacement->second;
        ++nextReplacement;
      }
    }

    sc->setRelocs(newRelocs);
  }
  return addressesChanged;
}

// Create a code map for CHPE metadata.
void Writer::createECCodeMap() {
  if (!ctx.getOutputSymtab().isEC())
    return;

  // Clear the map in case we were're recomputing the map after adding
  // a range extension thunk.
  codeMap.clear();

  std::optional<chpe_range_type> lastType;
  Chunk *first, *last;

  auto closeRange = [&]() {
    if (lastType) {
      codeMap.push_back({first, last, *lastType});
      lastType.reset();
    }
  };

  for (OutputSection *sec : ctx.outputSections) {
    for (Chunk *c : sec->chunks) {
      // Skip empty section chunks. MS link.exe does not seem to do that and
      // generates empty code ranges in some cases.
      if (isa<SectionChunk>(c) && !c->getSize())
        continue;

      std::optional<chpe_range_type> chunkType = c->getArm64ECRangeType();
      if (chunkType != lastType) {
        closeRange();
        first = c;
        lastType = chunkType;
      }
      last = c;
    }
  }

  closeRange();

  Symbol *tableCountSym = ctx.getOutputSymtab().findUnderscore("__hybrid_code_map_count");
  cast<DefinedAbsolute>(tableCountSym)->setVA(codeMap.size());
}

// Verify that all relocations are in range, with no extra margin requirements.
bool Writer::verifyRanges(const std::vector<Chunk *> chunks) {
  for (Chunk *c : chunks) {
    SectionChunk *sc = dyn_cast<SectionChunk>(c);
    if (!sc) {
      if (!cast<NonSectionChunk>(c)->verifyRanges())
        return false;
      continue;
    }
    MachineTypes machine = sc->getMachine();

    ArrayRef<coff_relocation> relocs = sc->getRelocs();
    for (const coff_relocation &rel : relocs) {
      Symbol *relocTarget = sc->file->getSymbol(rel.SymbolTableIndex);

      Defined *sym = dyn_cast_or_null<Defined>(relocTarget);
      if (!sym)
        continue;

      uint64_t p = sc->getRVA() + rel.VirtualAddress;
      uint64_t s = sym->getRVA();

      if (!isInRange(rel.Type, s, p, 0, machine))
        return false;
    }
  }
  return true;
}

// Assign addresses and add thunks if necessary.
void Writer::finalizeAddresses() {
  assignAddresses();
  if (ctx.config.machine != ARMNT && !isAnyArm64(ctx.config.machine))
    return;

  size_t origNumChunks = 0;
  for (OutputSection *sec : ctx.outputSections) {
    sec->origChunks = sec->chunks;
    origNumChunks += sec->chunks.size();
  }

  int pass = 0;
  int margin = 1024 * 100;
  while (true) {
    llvm::TimeTraceScope timeScope2("Add thunks pass");

    // First check whether we need thunks at all, or if the previous pass of
    // adding them turned out ok.
    bool rangesOk = true;
    size_t numChunks = 0;
    {
      llvm::TimeTraceScope timeScope3("Verify ranges");
      for (OutputSection *sec : ctx.outputSections) {
        if (!verifyRanges(sec->chunks)) {
          rangesOk = false;
          break;
        }
        numChunks += sec->chunks.size();
      }
    }
    if (rangesOk) {
      if (pass > 0)
        Log(ctx) << "Added " << (numChunks - origNumChunks) << " thunks with "
                 << "margin " << margin << " in " << pass << " passes";
      return;
    }

    if (pass >= 10)
      Fatal(ctx) << "adding thunks hasn't converged after " << pass
                 << " passes";

    if (pass > 0) {
      // If the previous pass didn't work out, reset everything back to the
      // original conditions before retrying with a wider margin. This should
      // ideally never happen under real circumstances.
      for (OutputSection *sec : ctx.outputSections)
        sec->chunks = sec->origChunks;
      margin *= 2;
    }

    // Try adding thunks everywhere where it is needed, with a margin
    // to avoid things going out of range due to the added thunks.
    bool addressesChanged = false;
    {
      llvm::TimeTraceScope timeScope3("Create thunks");
      for (OutputSection *sec : ctx.outputSections)
        addressesChanged |= createThunks(sec, margin);
    }
    // If the verification above thought we needed thunks, we should have
    // added some.
    assert(addressesChanged);
    (void)addressesChanged;

    // Recalculate the layout for the whole image (and verify the ranges at
    // the start of the next round).
    assignAddresses();

    pass++;
  }
}

void Writer::writePEChecksum() {
  if (!ctx.config.writeCheckSum) {
    return;
  }

  llvm::TimeTraceScope timeScope("PE checksum");

  // https://docs.microsoft.com/en-us/windows/win32/debug/pe-format#checksum
  uint32_t *buf = (uint32_t *)buffer->getBufferStart();
  uint32_t size = (uint32_t)(buffer->getBufferSize());

  pe32_header *peHeader = (pe32_header *)((uint8_t *)buf + coffHeaderOffset +
                                          sizeof(coff_file_header));

  uint64_t sum = 0;
  uint32_t count = size;
  ulittle16_t *addr = (ulittle16_t *)buf;

  // The PE checksum algorithm, implemented as suggested in RFC1071
  while (count > 1) {
    sum += *addr++;
    count -= 2;
  }

  // Add left-over byte, if any
  if (count > 0)
    sum += *(unsigned char *)addr;

  // Fold 32-bit sum to 16 bits
  while (sum >> 16) {
    sum = (sum & 0xffff) + (sum >> 16);
  }

  sum += size;
  peHeader->CheckSum = sum;
}

// The main function of the writer.
void Writer::run() {
  {
    llvm::TimeTraceScope timeScope("Write PE");
    ScopedTimer t1(ctx.codeLayoutTimer);

    calculateStubDependentSizes();
    if (ctx.config.machine == ARM64X)
      ctx.dynamicRelocs = make<DynamicRelocsChunk>();
    createImportTables();
    dropWeakInterposition();
    createSections();
    appendImportThunks();
    // Import thunks must be added before the Control Flow Guard tables are
    // added.
    createMiscChunks();
    createExportTable();
    mergeSections();
    sortECChunks();
    appendECImportTables();
    createDynamicRelocs();
    removeUnusedSections();
    layoutSections();
    finalizeAddresses();
    removeEmptySections();
    assignOutputSectionIndices();
    setSectionPermissions();
    validateRTTIBindings();
    validateImportStorage();
    setECSymbols();
    createSymbolAndStringTable();

    if (fileSize > UINT32_MAX)
      Fatal(ctx) << "image size (" << fileSize << ") "
                 << "exceeds maximum allowable size (" << UINT32_MAX << ")";

    openFile(ctx.config.outputFile);
    if (ctx.config.is64()) {
      writeHeader<pe32plus_header>();
    } else {
      writeHeader<pe32_header>();
    }
    writeSections();
    prepareLoadConfig();
    sortExceptionTables();

    // Fix up the alignment in the TLS Directory's characteristic field,
    // if a specific alignment value is needed
    if (tlsAlignment)
      fixTlsAlignment();
  }

  if (!ctx.config.pdbPath.empty() && ctx.config.debug) {
    assert(buildId);
    if (ctx.outputPartition)
      ctx.partitions->setDebugView(true);
    createPDB(ctx, sectionTable, buildId->buildId);
    if (ctx.outputPartition)
      ctx.partitions->setDebugView(false);
  }
  writeBuildId();

  writeLLDMapFile(ctx);
  writeMapFile(ctx);

  writePEChecksum();

  printSummary();

  if (errorCount())
    return;
}

void Writer::runProvider(BindingProvider &provider, SymbolTable &symbols) {
  providerSymbols = &symbols;
  calculateStubDependentSizes();
  const uint32_t ro = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  rdataSec = make<OutputSection>(".rdata", ro);
  edataSec = make<OutputSection>(".edata", ro);
  // Header emission also queries these sections. They are deliberately empty
  // and never enter the output section table of a metadata-only provider.
  textSec = make<OutputSection>(".text", 0);
  rsrcSec = make<OutputSection>(".rsrc", 0);
  relocSec = make<OutputSection>(".reloc", 0);
  ctx.outputSections = {rdataSec, edataSec};
  rdataSec->addChunk(&provider);

  DenseMap<ImportFile *, DefinedImportData *> imports;
  for (BindingProvider::Reference &reference : provider.references) {
    ImportFile *source = reference.target->file;
    DefinedImportData *&target = imports[source];
    if (!target) {
      // Output-local storage view of the already selected native import. Do
      // not parse it again or insert another definition in the shared graph.
      auto *file = make<ImportFile>(ctx, source->mb);
      file->hdr = source->hdr;
      file->dllName = source->dllName;
      file->externalName = source->externalName;
      file->needsIAT = false;
      file->live = true;
      ctx.config.dllOrder.try_emplace(StringRef(file->dllName).lower(),
                                      ctx.config.dllOrder.size());
      target = make<DefinedImportData>(reference.target->getName(), file,
                                       file->location);
      file->impSym = target;
      idata.add(target);
    }
    reference.target = target;
    if (!idata.slotRuns.empty()) {
      auto &run = idata.slotRuns.back();
      if (run.offset + run.syms.size() * ctx.config.wordsize ==
              reference.offset &&
          run.syms.front()->getDLLName() == target->getDLLName()) {
        run.syms.push_back(target);
        continue;
      }
    }
    idata.slotRuns.push_back({&provider, reference.offset, {target}});
  }
  if (!idata.empty()) {
    idata.create(ctx);
    for (const auto *chunks :
         {&idata.dirs, &idata.lookups, &idata.hints, &idata.dllNames})
      for (Chunk *chunk : *chunks)
        rdataSec->addChunk(chunk);
    importTableStart = idata.dirs.front();
    importTableSize = idata.dirs.size() * sizeof(ImportDirectoryTableEntry);
    iatStart = iatEnd = &provider;
  }

  Export exp;
  exp.name = provider.entity.identity;
  exp.exportName = provider.entity.lookupName;
  exp.sym = make<DefinedSynthetic>(exp.name, &provider);
  exp.ordinal = 1;
  exp.data = true;
  symbols.exports.push_back(exp);
  createEdataChunks(symbols, edataSec->chunks);
  symbols.edataStart = edataSec->chunks.front();
  symbols.edataEnd = edataSec->chunks.back();
  assignAddresses();
  assignOutputSectionIndices();
  if (fileSize > UINT32_MAX)
    Fatal(ctx) << "canonical provider exceeds the PE image size limit";
  openFile(ctx.config.outputFile);
  if (ctx.config.is64())
    writeHeader<pe32plus_header>();
  else
    writeHeader<pe32_header>();
  writeSections();
}

static StringRef getOutputSectionName(StringRef name) {
  StringRef s = name.split('$').first;

  // Treat a later period as a separator for MinGW, for sections like
  // ".ctors.01234".
  return s.substr(0, s.find('.', 1));
}

// For /order.
void Writer::sortBySectionOrder(std::vector<Chunk *> &chunks) {
  auto getPriority = [&ctx = ctx](const Chunk *c) {
    if (auto *sec = dyn_cast<SectionChunk>(c))
      if (sec->sym)
        return ctx.config.order.lookup(sec->sym->getName());
    return 0;
  };

  llvm::stable_sort(chunks, [=](const Chunk *a, const Chunk *b) {
    return getPriority(a) < getPriority(b);
  });
}

// Change the characteristics of existing PartialSections that belong to the
// section Name to Chars.
void Writer::fixPartialSectionChars(StringRef name, uint32_t chars) {
  for (auto it : partialSections) {
    PartialSection *pSec = it.second;
    StringRef curName = pSec->name;
    if (!curName.consume_front(name) ||
        (!curName.empty() && !curName.starts_with("$")))
      continue;
    if (pSec->characteristics == chars)
      continue;
    PartialSection *destSec = createPartialSection(pSec->name, chars);
    destSec->chunks.insert(destSec->chunks.end(), pSec->chunks.begin(),
                           pSec->chunks.end());
    pSec->chunks.clear();
  }
}

// Sort concrete section chunks from GNU import libraries.
//
// GNU binutils doesn't use short import files, but instead produces import
// libraries that consist of object files, with section chunks for the .idata$*
// sections. These are linked just as regular static libraries. Each import
// library consists of one header object, one object file for every imported
// symbol, and one trailer object. In order for the .idata tables/lists to
// be formed correctly, the section chunks within each .idata$* section need
// to be grouped by library, and sorted alphabetically within each library
// (which makes sure the header comes first and the trailer last).
bool Writer::fixGnuImportChunks() {
  uint32_t rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

  // Make sure all .idata$* section chunks are mapped as RDATA in order to
  // be sorted into the same sections as our own synthesized .idata chunks.
  fixPartialSectionChars(".idata", rdata);

  bool hasIdata = false;
  // Sort all .idata$* chunks, grouping chunks from the same library,
  // with alphabetical ordering of the object files within a library.
  for (auto it : partialSections) {
    PartialSection *pSec = it.second;
    if (!pSec->name.starts_with(".idata"))
      continue;

    if (!pSec->chunks.empty())
      hasIdata = true;
    llvm::stable_sort(pSec->chunks, [&](Chunk *s, Chunk *t) {
      SectionChunk *sc1 = dyn_cast<SectionChunk>(s);
      SectionChunk *sc2 = dyn_cast<SectionChunk>(t);
      if (!sc1 || !sc2) {
        // if SC1, order them ascending. If SC2 or both null,
        // S is not less than T.
        return sc1 != nullptr;
      }
      // Make a string with "libraryname/objectfile" for sorting, achieving
      // both grouping by library and sorting of objects within a library,
      // at once.
      std::string key1 =
          (sc1->file->parentName + "/" + sc1->file->getName()).str();
      std::string key2 =
          (sc2->file->parentName + "/" + sc2->file->getName()).str();
      return key1 < key2;
    });
  }
  return hasIdata;
}

// Add generated idata chunks, for imported symbols and DLLs, and a
// terminator in .idata$2.
void Writer::addSyntheticIdata() {
  uint32_t rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  idata.create(ctx);

  // Add the .idata content in the right section groups, to allow
  // chunks from other linked in object files to be grouped together.
  // See Microsoft PE/COFF spec 5.4 for details.
  auto add = [&](StringRef n, std::vector<Chunk *> &v) {
    PartialSection *pSec = createPartialSection(n, rdata);
    pSec->chunks.insert(pSec->chunks.end(), v.begin(), v.end());
  };

  // The loader assumes a specific order of data.
  // Add each type in the correct order.
  add(".idata$2", idata.dirs);
  add(".idata$4", idata.lookups);
  add(".idata$5", idata.addresses);
  // Read-only chunks with in-place import slots follow the import address
  // table, in the order addImportSlots gave their slots, so that the IAT data
  // directory covers them: the loader makes only that range writable while
  // it resolves imports, and a slot outside it would fault.
  if (!iatSlotChunks.empty()) {
    PartialSection *pSec = createPartialSection(".idata$5", rdata);
    for (const ImportSlot &s : importSlots)
      if (iatSlotChunks.contains(s.chunk) &&
          (pSec->chunks.empty() || pSec->chunks.back() != s.chunk))
        pSec->chunks.push_back(s.chunk);
  }
  if (!idata.hints.empty())
    add(".idata$6", idata.hints);
  add(".idata$7", idata.dllNames);
  if (!idata.auxIat.empty())
    add(".idata$9", idata.auxIat);
  if (!idata.auxIatCopy.empty())
    add(".idata$a", idata.auxIatCopy);
}

void Writer::appendECImportTables() {
  if (!isArm64EC(ctx.config.machine))
    return;

  const uint32_t rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

  // IAT is always placed at the beginning of .rdata section and its size
  // is aligned to 4KB. Insert it here, after all merges all done.
  if (PartialSection *importAddresses = findPartialSection(".idata$5", rdata)) {
    if (!rdataSec->chunks.empty())
      rdataSec->chunks.front()->setAlignment(
          std::max(0x1000u, rdataSec->chunks.front()->getAlignment()));

    rdataSec->chunks.insert(rdataSec->chunks.begin(),
                            importAddresses->chunks.begin(),
                            importAddresses->chunks.end());
    rdataSec->contribSections.insert(rdataSec->contribSections.begin(),
                                     importAddresses);
  }

  // The auxiliary IAT is always placed at the end of the .rdata section
  // and is aligned to 4KB.
  if (PartialSection *auxIat = findPartialSection(".idata$9", rdata)) {
    auxIat->chunks.front()->setAlignment(0x1000);
    rdataSec->chunks.insert(rdataSec->chunks.end(), auxIat->chunks.begin(),
                            auxIat->chunks.end());
    rdataSec->addContributingPartialSection(auxIat);
  }

  if (!delayIdata.getAuxIat().empty()) {
    delayIdata.getAuxIat().front()->setAlignment(0x1000);
    rdataSec->chunks.insert(rdataSec->chunks.end(),
                            delayIdata.getAuxIat().begin(),
                            delayIdata.getAuxIat().end());
  }
}

// Locate the first Chunk and size of the import directory list and the
// IAT.
void Writer::locateImportTables() {
  uint32_t rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

  if (PartialSection *importDirs = findPartialSection(".idata$2", rdata)) {
    if (!importDirs->chunks.empty())
      importTableStart = importDirs->chunks.front();
    for (Chunk *c : importDirs->chunks)
      importTableSize += c->getSize();
  }

  if (PartialSection *importAddresses = findPartialSection(".idata$5", rdata)) {
    if (!importAddresses->chunks.empty()) {
      iatStart = importAddresses->chunks.front();
      iatEnd = importAddresses->chunks.back();
    }
  }
}

// Records the in-place import slots of a chunk. A read-only chunk holding one
// is held back for the import address table region (addSyntheticIdata), or,
// if one of its slots holds an offset, moved to .data for ImportFillChunk to
// write. Only a chunk from a compiler-named read-only section can move: a
// user-named or $-grouped section is iterated by its bounds. Unwind/LSDA chunks
// in .xdata are addressed by relocations, not section bounds, and can move as
// intact contributions too. Their imported catch-type pointers then use the
// same native binding as vtable and descriptor fields.
bool Writer::findImportSlots(SectionChunk *sc, StringRef name) {
  size_t first = importSlots.size();
  sc->getImportSlots(importSlots);
  if (importSlots.size() == first)
    return false;
  DefinedImportData *sym = importSlots[first].sym;
  auto withOffset =
      std::stable_partition(importSlots.begin() + first, importSlots.end(),
                            [](const ImportSlot &s) { return s.addend == 0; });
  bool filled = withOffset != importSlots.end();
  filledSlots.insert(filledSlots.end(), withOffset, importSlots.end());
  importSlots.erase(withOffset, importSlots.end());
  if (sc->getOutputCharacteristics() & IMAGE_SCN_MEM_WRITE)
    return false;
  if (name != ".rdata" && name != ".xdata") {
    StringRef symName = sym->getName();
    symName.consume_front("__imp_");
    Err(ctx) << toString(sc->file) << ": section " << sc->getSectionName()
             << " holds the address of " << symName << ", imported from "
             << sym->getDLLName()
             << ", but is read-only and cannot be laid out with the import "
                "address table; make the section writable or take the "
                "address in code";
    return false;
  }
  if (filled)
    createPartialSection(".data", IMAGE_SCN_CNT_INITIALIZED_DATA |
                                      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)
        ->chunks.push_back(sc);
  else
    iatSlotChunks[sc] = ctx.config.dllOrder[sym->getDLLName().lower()];
  return true;
}

// Creates the code that writes the slots holding an offset (ImportFillChunk)
// and its C initializer table entry.
void Writer::createImportFill() {
  if (filledSlots.empty())
    return;
  auto fail = [&](StringRef why) {
    for (const ImportSlot &s : filledSlots) {
      StringRef symName = s.sym->getName();
      symName.consume_front("__imp_");
      Err(ctx) << toString(s.chunk->file) << ": static data holds the address "
               << "of " << symName << ", imported from " << s.sym->getDLLName()
               << ", plus " << s.addend << "; the loader writes only the plain "
               << "address, and " << why;
    }
  };
  if (ctx.config.machine != AMD64)
    return fail("the linker writes the offset only on x86-64");
  Symbol *xi = ctx.getOutputSymtab().findUnderscore("__xi_a");
  if (!xi || !isa<Defined>(xi))
    return fail("the image has no C initializer table (__xi_a) to write it "
                "from");
  importFill = make<ImportFillChunk>(filledSlots);
  // After .CRT$XIA, where the table begins, and before every C initializer.
  createPartialSection(".CRT$XIB",
                       IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ)
      ->chunks.push_back(make<ImportFillEntryChunk>(ctx, importFill));
}

// Groups the in-place import slots into runs (IdataContents::SlotRun). The
// read-only slot chunks are ordered by DLL so that single-pointer chunks of
// one DLL sit next to each other; the other chunks keep their place, and a run
// never continues past them.
void Writer::addImportSlots() {
  if (importSlots.empty())
    return;
  auto key = [&](const ImportSlot &s) {
    auto it = iatSlotChunks.find(s.chunk);
    return it == iatSlotChunks.end() ? -1 : it->second;
  };
  llvm::stable_sort(importSlots, [&](const ImportSlot &a, const ImportSlot &b) {
    return key(a) < key(b);
  });

  uint32_t wordsize = ctx.config.wordsize;
  const ImportSlot *prev = nullptr;
  for (const ImportSlot &s : importSlots) {
    bool joins = false;
    if (prev &&
        prev->sym->getDLLName().equals_insensitive(s.sym->getDLLName())) {
      if (s.chunk == prev->chunk)
        joins = s.offset == prev->offset + wordsize;
      else
        // Consecutive chunks of the table region, the second of which needs
        // no padding after the first.
        joins = key(s) >= 0 && key(*prev) >= 0 && s.offset == 0 &&
                prev->offset + wordsize == prev->chunk->getSize() &&
                s.chunk->getAlignment() <= prev->chunk->getAlignment() &&
                prev->chunk->getSize() % s.chunk->getAlignment() == 0;
    }
    if (!joins)
      idata.slotRuns.push_back({s.chunk, s.offset, {}});
    idata.slotRuns.back().syms.push_back(s.sym);
    prev = &s;
  }
}

// Preserve an ordinary cell for any use of its address or value. In-place
// imports are classified separately before this helper sees their symbols.
static void retainImportAddressTable(Symbol *sym) {
  SmallPtrSet<Symbol *, 4> seen;
  while (sym && seen.insert(sym).second) {
    Defined *def = sym->getDefined();
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(def)) {
      imp->file->needsIAT = true;
      return;
    }
    if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(def)) {
      thunk->wrappedSym->file->needsIAT = true;
      return;
    }
    auto *local = dyn_cast_or_null<DefinedLocalImport>(def);
    sym = local ? local->wrappedSym : nullptr;
  }
}

void Writer::selectImportStorage() {
  // ARM64EC tables carry distinct native/EC callable addresses and can have
  // loader-patched views. Their storage is governed by the hybrid table rules.
  if (importSlots.empty() || ctx.hybridSymtab || ctx.getOutputSymtab().isEC())
    return;

  // Keep the provider live. Only its unused ordinary cell is a candidate for
  // omission. Giving aliases a real destination also preserves symbol/debug
  // RVAs without making a discarded synthetic cell part of the image.
  SmallPtrSet<ImportFile *, 32> reusable;
  for (const ImportSlot &slot : importSlots) {
    ImportFile *file = slot.sym->file;
    if (file->needsIAT) {
      file->needsIAT = false;
      slot.sym->setLocation(slot.chunk, slot.offset);
    }
    // The contribution already survived GC: borrowing its immutable field
    // retains no otherwise dead object. Prefer a naturally aligned field over
    // a writable or unaligned destination, preserving ordinary load alignment.
    // Final permissions and IAT coverage are checked after output layout.
    if (iatSlotChunks.contains(slot.chunk) &&
        slot.chunk->getAlignment() >= ctx.config.wordsize &&
        slot.offset % ctx.config.wordsize == 0 && reusable.insert(file).second)
      slot.sym->setLocation(slot.chunk, slot.offset);
  }

  for (Symbol *root : ctx.config.gcroot)
    retainImportAddressTable(root);
  for (ImportFile *file : ctx.importFileInstances) {
    if (file->live && file->thunkSym && file->thunkSym->isLive() &&
        !reusable.contains(file))
      retainImportAddressTable(file->thunkSym);
  }
  for (const ImportSlot &slot : filledSlots)
    retainImportAddressTable(slot.sym);

  // Run after COMDAT selection, GC, ICF and local-import binding. Uses through
  // aliases and observable __imp_ cells reach the same ImportFile state; never
  // decide from a spelling or from the import member's live bit alone.
  for (Chunk *chunk : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(chunk);
    if (!sc || !sc->live)
      continue;
    for (const coff_relocation &rel : sc->getRelocs()) {
      if (sc->getImportSlot(rel))
        continue;
      Symbol *sym = sc->file->getSymbol(rel.SymbolTableIndex);
      // References to a thunk expose its code address, not the address of the
      // pointer it reads. Its synthetic jump can use the same immutable field.
      if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(sym))
        if (reusable.contains(thunk->wrappedSym->file))
          continue;
      auto *imp = dyn_cast_or_null<DefinedImportData>(sym);
      // Reuse the existing, exact import-reference classification. These x64
      // forms read the whole pointer without exposing the cell's address.
      // ADRP and unknown forms remain conservative: a partial address sequence
      // alone does not prove what the remaining instructions do with the cell.
      if (imp && reusable.contains(imp->file) &&
          sc->getMachine() == AMD64 &&
          (sc->getOutputCharacteristics() & IMAGE_SCN_CNT_CODE)) {
        // The equal-size LEA alternative acquires the imported value, not
        // the cell's address. It can use the same immutable destination as
        // an explicitly emitted import load once normalization is complete.
        if (sc->isImportAddressLoad(rel))
          continue;
        SectionChunk::ImportRefForm form = sc->getImportRefForm(rel);
        if (!imp->isRuntimePseudoReloc &&
            (form == SectionChunk::ImportRefForm::Load ||
             form == SectionChunk::ImportRefForm::Call ||
             form == SectionChunk::ImportRefForm::Jump))
          continue;
      }
      retainImportAddressTable(sym);
    }
  }
}

// Return whether a SectionChunk's suffix (the dollar and any trailing
// suffix) should be removed and sorted into the main suffixless
// PartialSection.
static bool shouldStripSectionSuffix(SectionChunk *sc, StringRef name,
                                     bool isMinGW) {
  // On MinGW, comdat groups are formed by putting the comdat group name
  // after the '$' in the section name. For .eh_frame$<symbol>, that must
  // still be sorted before the .eh_frame trailer from crtend.o, thus just
  // strip the section name trailer. For other sections, such as
  // .tls$$<symbol> (where non-comdat .tls symbols are otherwise stored in
  // ".tls$"), they must be strictly sorted after .tls. And for the
  // hypothetical case of comdat .CRT$XCU, we definitely need to keep the
  // suffix for sorting. Thus, to play it safe, only strip the suffix for
  // the standard sections.
  if (!isMinGW)
    return false;
  if (!sc || !sc->isCOMDAT())
    return false;
  return name.starts_with(".text$") || name.starts_with(".data$") ||
         name.starts_with(".rdata$") || name.starts_with(".pdata$") ||
         name.starts_with(".xdata$") || name.starts_with(".eh_frame$");
}

void Writer::sortSections() {
  if (!ctx.config.callGraphProfile.empty()) {
    DenseMap<const SectionChunk *, int> order =
        computeCallGraphProfileOrder(ctx);
    for (auto it : order) {
      if (DefinedRegular *sym = it.first->sym)
        ctx.config.order[sym->getName()] = it.second;
    }
  }
  if (!ctx.config.order.empty())
    for (auto it : partialSections)
      sortBySectionOrder(it.second->chunks);
}

void Writer::calculateStubDependentSizes() {
  if (ctx.config.dosStub)
    dosStubSize = alignTo(ctx.config.dosStub->getBufferSize(), 8);
  else
    dosStubSize = sizeof(dos_header) + sizeof(dosProgram);

  coffHeaderOffset = dosStubSize + sizeof(PEMagic);
  peHeaderOffset = coffHeaderOffset + sizeof(coff_file_header);
  dataDirOffset64 = peHeaderOffset + sizeof(pe32plus_header);
}

// Create output section objects and add them to OutputSections.
void Writer::createSections() {
  llvm::TimeTraceScope timeScope("Output sections");
  // First, create the builtin sections.
  const uint32_t data = IMAGE_SCN_CNT_INITIALIZED_DATA;
  const uint32_t bss = IMAGE_SCN_CNT_UNINITIALIZED_DATA;
  const uint32_t code = IMAGE_SCN_CNT_CODE;
  const uint32_t discardable = IMAGE_SCN_MEM_DISCARDABLE;
  const uint32_t r = IMAGE_SCN_MEM_READ;
  const uint32_t w = IMAGE_SCN_MEM_WRITE;
  const uint32_t x = IMAGE_SCN_MEM_EXECUTE;

  SmallDenseMap<std::pair<StringRef, uint32_t>, OutputSection *> sections;
  auto createSection = [&](StringRef name, uint32_t outChars) {
    OutputSection *&sec = sections[{name, outChars}];
    if (!sec) {
      sec = make<OutputSection>(name, outChars);
      ctx.outputSections.push_back(sec);
    }
    return sec;
  };

  // Try to match the section order used by link.exe.
  textSec = createSection(".text", code | r | x);
  if (isArm64EC(ctx.config.machine)) {
    wowthkSec = createSection(".wowthk", code | r | x);
    hexpthkSec = createSection(".hexpthk", code | r | x);
  }
  bssSec = createSection(".bss", bss | r | w);
  rdataSec = createSection(".rdata", data | r);
  buildidSec = createSection(".buildid", data | r);
  cvinfoSec = createSection(".cvinfo", data | r);
  dataSec = createSection(".data", data | r | w);
  pdataSec = createSection(".pdata", data | r);
  idataSec = createSection(".idata", data | r);
  edataSec = createSection(".edata", data | r);
  didatSec = createSection(".didat", data | r | (protectDelayIat() ? w : 0));
  if (isArm64EC(ctx.config.machine))
    a64xrmSec = createSection(".a64xrm", data | r);
  wkpubSec = createSection(".wkpub", data | r);
  rsrcSec = createSection(".rsrc", data | r);
  relocSec = createSection(".reloc", data | discardable | r);
  ctorsSec = createSection(".ctors", data | r | w);
  dtorsSec = createSection(".dtors", data | r | w);

  // Then bin chunks by name and output characteristics.
  for (Chunk *c : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(c);
    if (sc && !sc->live) {
      if (ctx.config.verbose)
        sc->printDiscardedMessage();
      continue;
    }
    if (auto *cc = dyn_cast<CommonChunk>(c)) {
      if (!cc->live)
        continue;
    }
    StringRef name = c->getSectionName();
    if (shouldStripSectionSuffix(sc, name, ctx.config.mingw))
      name = name.split('$').first;

    if (name.starts_with(".tls"))
      tlsAlignment = std::max(tlsAlignment, c->getAlignment());

    if (sc && ctx.config.importSlots && findImportSlots(sc, name))
      continue;

    PartialSection *pSec = createPartialSection(name,
                                                c->getOutputCharacteristics());
    pSec->chunks.push_back(c);
  }

  createImportFill();

  fixPartialSectionChars(".rsrc", data | r);
  fixPartialSectionChars(".edata", data | r);
  // Even in non MinGW cases, we might need to link against GNU import
  // libraries.
  bool hasIdata = fixGnuImportChunks();
  if (!idata.empty())
    hasIdata = true;

  sortSections();

  // After sorting: the read-only slot chunks keep the order their runs need.
  if (hasIdata) {
    addImportSlots();
    selectImportStorage();
    addSyntheticIdata();
    locateImportTables();
  }

  for (auto thunk : ctx.getOutputSymtab().sameAddressThunks)
    wowthkSec->addChunk(thunk);

  if (importFill)
    textSec->addChunk(importFill);

  // Then create an OutputSection for each section.
  // '$' and all following characters in input section names are
  // discarded when determining output section. So, .text$foo
  // contributes to .text, for example. See PE/COFF spec 3.2.
  for (auto it : partialSections) {
    PartialSection *pSec = it.second;
    StringRef name = getOutputSectionName(pSec->name);
    uint32_t outChars = pSec->characteristics;

    if (name == ".CRT") {
      // In link.exe, there is a special case for the I386 target where .CRT
      // sections are treated as if they have output characteristics DATA | R if
      // their characteristics are DATA | R | W. This implements the same
      // special case for all architectures.
      outChars = data | r;

      Log(ctx) << "Processing section " << pSec->name << " -> " << name;

      sortCRTSectionChunks(pSec->chunks);
    }

    // ARM64EC has specific placement and alignment requirements for the IAT.
    // Delay adding its chunks until appendECImportTables.
    if (isArm64EC(ctx.config.machine) &&
        (pSec->name == ".idata$5" || pSec->name == ".idata$9"))
      continue;

    OutputSection *sec = createSection(name, outChars);
    for (Chunk *c : pSec->chunks)
      sec->addChunk(c);

    sec->addContributingPartialSection(pSec);
  }

  if (ctx.hybridSymtab) {
    if (OutputSection *sec = findSection(".CRT"))
      sec->splitECChunks();
  }

  // Finally, move some output sections to the end.
  auto sectionOrder = [&](const OutputSection *s) {
    // Move DISCARDABLE (or non-memory-mapped) sections to the end of file
    // because the loader cannot handle holes. Stripping can remove other
    // discardable ones than .reloc, which is first of them (created early).
    if (s->header.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
      // Move discardable sections named .debug_ to the end, after other
      // discardable sections. Stripping only removes the sections named
      // .debug_* - thus try to avoid leaving holes after stripping.
      if (s->name.starts_with(".debug_"))
        return 3;
      return 2;
    }
    // .rsrc should come at the end of the non-discardable sections because its
    // size may change by the Win32 UpdateResources() function, causing
    // subsequent sections to move (see https://crbug.com/827082).
    if (s == rsrcSec)
      return 1;
    return 0;
  };
  llvm::stable_sort(ctx.outputSections,
                    [&](const OutputSection *s, const OutputSection *t) {
                      return sectionOrder(s) < sectionOrder(t);
                    });
}

void Writer::createMiscChunks() {
  llvm::TimeTraceScope timeScope("Misc chunks");
  Configuration *config = &ctx.config;

  for (MergeChunk *p : ctx.mergeChunkInstances) {
    if (p) {
      p->finalizeContents();
      rdataSec->addChunk(p);
    }
  }

  // Create thunks for locally-dllimported symbols.
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    if (!symtab.localImportChunks.empty()) {
      for (Chunk *c : symtab.localImportChunks)
        rdataSec->addChunk(c);
    }
  });

  // Create Debug Information Chunks
  if (config->mingw) {
    debugInfoSec = buildidSec;
  } else if (!config->mergeDebugDirectory) {
    debugInfoSec = cvinfoSec;
  } else {
    debugInfoSec = rdataSec;
  }
  if (config->buildIDHash != BuildIDHash::None || config->debug ||
      config->repro || config->cetCompat || config->cetCompatStrict ||
      config->cetCompatIpValidationRelaxed ||
      config->cetCompatDynamicApisInProcOnly || config->hotpatchCompat) {
    debugDirectory =
        make<DebugDirectoryChunk>(ctx, debugRecords, config->repro);
    debugDirectory->setAlignment(4);
    debugInfoSec->addChunk(debugDirectory);
  }

  if (config->debug || config->buildIDHash != BuildIDHash::None) {
    // Make a CVDebugRecordChunk even when /DEBUG:CV is not specified.  We
    // output a PDB no matter what, and this chunk provides the only means of
    // allowing a debugger to match a PDB and an executable.  So we need it even
    // if we're ultimately not going to write CodeView data to the PDB.
    buildId = make<CVDebugRecordChunk>(ctx);
    debugRecords.emplace_back(COFF::IMAGE_DEBUG_TYPE_CODEVIEW, buildId);
    ctx.forEachSymtab([&](SymbolTable &symtab) {
      if (Symbol *buildidSym = symtab.findUnderscore("__buildid"))
        replaceSymbol<DefinedSynthetic>(buildidSym, buildidSym->getName(),
                                        buildId, 4);
    });
  }

  uint16_t ex_characteristics_flags = 0;
  if (config->cetCompat)
    ex_characteristics_flags |= IMAGE_DLL_CHARACTERISTICS_EX_CET_COMPAT;
  if (config->cetCompatStrict)
    ex_characteristics_flags |=
        IMAGE_DLL_CHARACTERISTICS_EX_CET_COMPAT_STRICT_MODE;
  if (config->cetCompatIpValidationRelaxed)
    ex_characteristics_flags |=
        IMAGE_DLL_CHARACTERISTICS_EX_CET_SET_CONTEXT_IP_VALIDATION_RELAXED_MODE;
  if (config->cetCompatDynamicApisInProcOnly)
    ex_characteristics_flags |=
        IMAGE_DLL_CHARACTERISTICS_EX_CET_DYNAMIC_APIS_ALLOW_IN_PROC_ONLY;
  if (config->hotpatchCompat)
    ex_characteristics_flags |=
        IMAGE_DLL_CHARACTERISTICS_EX_HOTPATCH_COMPATIBLE;

  if (ex_characteristics_flags) {
    debugRecords.emplace_back(
        COFF::IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS,
        make<ExtendedDllCharacteristicsChunk>(ex_characteristics_flags));
  }

  // Align and add each chunk referenced by the debug data directory.
  for (std::pair<COFF::DebugType, Chunk *> r : debugRecords) {
    r.second->setAlignment(4);
    debugInfoSec->addChunk(r.second);
  }

  // Create SEH table. x86-only.
  if (config->safeSEH)
    createSEHTable();

  // Create /guard:cf tables if requested.
  createGuardCFTables();

  createECChunks();

  if (config->autoImport)
    createRuntimePseudoRelocs();
  createWeakPublishTable();
  collectImportFuseCalls();

  if (config->mingw) {
    insertCtorDtorSymbols();
    insertBssDataStartEndSymbols();
  }
}

// Create .idata section for the DLL-imported symbol table.
// The format of this section is inherently Windows-specific.
// IdataContents class abstracted away the details for us,
// so we just let it create chunks and add them to the section.
void Writer::createImportTables() {
  llvm::TimeTraceScope timeScope("Import tables");
  // Initialize DLLOrder so that import entries are ordered in
  // the same order as in the command line. (That affects DLL
  // initialization order, and this ordering is MSVC-compatible.)
  for (ImportFile *file : ctx.importFileInstances) {
    if (!file->live)
      continue;

    std::string dll = StringRef(file->dllName).lower();
    ctx.config.dllOrder.try_emplace(dll, ctx.config.dllOrder.size());

    if (file->impSym && !isa<DefinedImportData>(file->impSym))
      Fatal(ctx) << file->symtab.printSymbol(file->impSym) << " was replaced";
    DefinedImportData *impSym = cast_or_null<DefinedImportData>(file->impSym);
    if (ctx.config.delayLoads.contains(StringRef(file->dllName).lower())) {
      if (!file->thunkSym)
        Fatal(ctx) << "cannot delay-load " << toString(file)
                   << " due to import of data: "
                   << file->symtab.printSymbol(impSym);
      delayIdata.add(impSym);
    } else {
      idata.add(impSym);
    }
  }
}

void Writer::appendImportThunks() {
  if (ctx.importFileInstances.empty())
    return;

  llvm::TimeTraceScope timeScope("Import thunks");
  for (ImportFile *file : ctx.importFileInstances) {
    if (!file->live)
      continue;

    if (file->thunkSym) {
      if (!isa<DefinedImportThunk>(file->thunkSym))
        Fatal(ctx) << file->symtab.printSymbol(file->thunkSym)
                   << " was replaced";
      auto *chunk = cast<DefinedImportThunk>(file->thunkSym)->getChunk();
      if (chunk->live)
        textSec->addChunk(chunk);
    }

    if (file->auxThunkSym) {
      if (!isa<DefinedImportThunk>(file->auxThunkSym))
        Fatal(ctx) << file->symtab.printSymbol(file->auxThunkSym)
                   << " was replaced";
      auto *chunk = cast<DefinedImportThunk>(file->auxThunkSym)->getChunk();
      if (chunk->live)
        textSec->addChunk(chunk);
    }

    if (file->impchkThunk)
      textSec->addChunk(file->impchkThunk);
  }

  if (!delayIdata.empty()) {
    delayIdata.create();
    // A protected image lays the tables out as link.exe does: the import
    // address table is alone in .didat, which the loader makes read-only and
    // writable again only while it resolves an import, so nothing else may
    // share a page with it. The descriptors and the name table are never
    // written and belong with the rest of the read-only data; the module
    // handles are written on the unprotected path and cannot go in .didat.
    OutputSection *tableSec = didatSec;
    OutputSection *iatSec = dataSec;
    if (protectDelayIat()) {
      if (!didatSec->chunks.empty())
        Err(ctx) << "-delayload-protect: .didat holds input sections";
      ctx.config.merge.erase(".didat");
      tableSec = rdataSec;
      iatSec = didatSec;
    }
    for (Chunk *c : delayIdata.getChunks())
      tableSec->addChunk(c);
    for (Chunk *c : delayIdata.getDataChunks())
      dataSec->addChunk(c);
    for (Chunk *c : delayIdata.getIat())
      iatSec->addChunk(c);
    for (Chunk *c : delayIdata.getCodeChunks())
      textSec->addChunk(c);
    for (Chunk *c : delayIdata.getCodePData())
      pdataSec->addChunk(c);
    for (Chunk *c : delayIdata.getAuxIatCopy())
      rdataSec->addChunk(c);
    for (Chunk *c : delayIdata.getCodeUnwindInfo())
      rdataSec->addChunk(c);
  }
}

void Writer::createExportTable() {
  llvm::TimeTraceScope timeScope("Export table");
  if (!edataSec->chunks.empty()) {
    // Allow using a custom built export table from input object files, instead
    // of having the linker synthesize the tables.
    if (!ctx.hybridSymtab) {
      ctx.getOutputSymtab().edataStart = edataSec->chunks.front();
      ctx.getOutputSymtab().edataEnd = edataSec->chunks.back();
    } else {
      // On hybrid target, split EC and native chunks.
      llvm::stable_sort(edataSec->chunks, [=](const Chunk *a, const Chunk *b) {
        return (a->getMachine() != ARM64) < (b->getMachine() != ARM64);
      });

      for (auto chunk : edataSec->chunks) {
        if (chunk->getMachine() != ARM64) {
          ctx.getOutputSymtab().edataStart = chunk;
          ctx.getOutputSymtab().edataEnd = edataSec->chunks.back();
          break;
        }

        if (!ctx.hybridSymtab->edataStart)
          ctx.hybridSymtab->edataStart = chunk;
        ctx.hybridSymtab->edataEnd = chunk;
      }
    }
  }
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    if (symtab.edataStart) {
      if (symtab.hadExplicitExports)
        Warn(ctx) << "literal .edata sections override exports";
    } else if (!symtab.exports.empty()) {
      std::vector<Chunk *> edataChunks;
      createEdataChunks(symtab, edataChunks);
      for (Chunk *c : edataChunks)
        edataSec->addChunk(c);
      symtab.edataStart = edataChunks.front();
      symtab.edataEnd = edataChunks.back();
    }

    // Warn on exported deleting destructor.
    for (auto e : symtab.exports)
      if (e.sym && e.sym->getName().starts_with("??_G"))
        Warn(ctx) << "export of deleting dtor: " << toString(ctx, *e.sym);
  });
}

void Writer::removeUnusedSections() {
  llvm::TimeTraceScope timeScope("Remove unused sections");
  // Remove sections that we can be sure won't get content, to avoid
  // allocating space for their section headers.
  auto isUnused = [this](OutputSection *s) {
    if (s == relocSec)
      return false; // This section is populated later.
    // MergeChunks have zero size at this point, as their size is finalized
    // later. Only remove sections that have no Chunks at all.
    return s->chunks.empty();
  };
  llvm::erase_if(ctx.outputSections, isUnused);
}

void Writer::layoutSections() {
  llvm::TimeTraceScope timeScope("Layout sections");
  if (ctx.config.sectionOrder.empty())
    return;

  llvm::stable_sort(ctx.outputSections,
                    [this](const OutputSection *a, const OutputSection *b) {
                      auto itA = ctx.config.sectionOrder.find(a->name.str());
                      auto itB = ctx.config.sectionOrder.find(b->name.str());
                      bool aInOrder = itA != ctx.config.sectionOrder.end();
                      bool bInOrder = itB != ctx.config.sectionOrder.end();

                      // Put unspecified sections after all specified sections
                      if (aInOrder && bInOrder) {
                        return itA->second < itB->second;
                      } else if (aInOrder && !bInOrder) {
                        return true; // ordered sections come before unordered
                      } else {
                        // (!aInOrder && bInOrder): unordered comes after
                        // ordered
                        // (!aInOrder && !bInOrder): both unspecified, preserve
                        // the original order
                        return false;
                      }
                    });
}

// The Windows loader doesn't seem to like empty sections,
// so we remove them if any.
void Writer::removeEmptySections() {
  llvm::TimeTraceScope timeScope("Remove empty sections");
  auto isEmpty = [](OutputSection *s) { return s->getVirtualSize() == 0; };
  llvm::erase_if(ctx.outputSections, isEmpty);
}

void Writer::assignOutputSectionIndices() {
  llvm::TimeTraceScope timeScope("Output sections indices");
  // Assign final output section indices, and assign each chunk to its output
  // section.
  uint32_t idx = 1;
  for (OutputSection *os : ctx.outputSections) {
    os->sectionIndex = idx;
    for (Chunk *c : os->chunks)
      c->setOutputSectionIdx(idx);
    ++idx;
  }

  // Merge chunks are containers of chunks, so assign those an output section
  // too.
  if (providerSymbols)
    return;
  for (MergeChunk *mc : ctx.mergeChunkInstances)
    if (mc)
      for (SectionChunk *sc : mc->sections)
        if (sc && sc->live)
          sc->setOutputSectionIdx(mc->getOutputSectionIdx());
}

std::optional<coff_symbol16> Writer::createSymbol(Defined *def) {
  coff_symbol16 sym;
  switch (def->kind()) {
  case Symbol::DefinedAbsoluteKind: {
    auto *da = dyn_cast<DefinedAbsolute>(def);
    // Note: COFF symbol can only store 32-bit values, so 64-bit absolute
    // values will be truncated.
    sym.Value = da->getVA();
    sym.SectionNumber = IMAGE_SYM_ABSOLUTE;
    break;
  }
  default: {
    // Don't write symbols that won't be written to the output to the symbol
    // table.
    // We also try to write DefinedSynthetic as a normal symbol. Some of these
    // symbols do point to an actual chunk, like __safe_se_handler_table. Others
    // like __ImageBase are outside of sections and thus cannot be represented.
    Chunk *c = def->getChunk();
    if (!c)
      return std::nullopt;
    OutputSection *os = ctx.getOutputSection(c);
    if (!os)
      return std::nullopt;

    sym.Value = def->getRVA() - os->getRVA();
    sym.SectionNumber = os->sectionIndex;
    break;
  }
  }

  // Symbols that are runtime pseudo relocations don't point to the actual
  // symbol data itself (as they are imported), but points to the IAT entry
  // instead. Avoid emitting them to the symbol table, as they can confuse
  // debuggers.
  if (def->isRuntimePseudoReloc)
    return std::nullopt;

  StringRef name = def->getName();
  if (name.size() > COFF::NameSize) {
    sym.Name.Offset.Zeroes = 0;
    sym.Name.Offset.Offset = 0; // Filled in later.
    strtab.add(name);
  } else {
    memset(sym.Name.ShortName, 0, COFF::NameSize);
    memcpy(sym.Name.ShortName, name.data(), name.size());
  }

  if (auto *d = dyn_cast<DefinedCOFF>(def)) {
    COFFSymbolRef ref = d->getCOFFSymbol();
    sym.Type = ref.getType();
    sym.StorageClass = ref.getStorageClass();
  } else if (def->kind() == Symbol::DefinedImportThunkKind) {
    sym.Type = (IMAGE_SYM_DTYPE_FUNCTION << SCT_COMPLEX_TYPE_SHIFT) |
               IMAGE_SYM_TYPE_NULL;
    sym.StorageClass = IMAGE_SYM_CLASS_EXTERNAL;
  } else {
    sym.Type = IMAGE_SYM_TYPE_NULL;
    sym.StorageClass = IMAGE_SYM_CLASS_EXTERNAL;
  }
  sym.NumberOfAuxSymbols = 0;
  return sym;
}

void Writer::createSymbolAndStringTable() {
  llvm::TimeTraceScope timeScope("Symbol and string table");
  // PE/COFF images are limited to 8 byte section names. Longer names can be
  // supported by writing a non-standard string table, but this string table is
  // not mapped at runtime and the long names will therefore be inaccessible.
  // link.exe always truncates section names to 8 bytes, whereas binutils always
  // preserves long section names via the string table. LLD adopts a hybrid
  // solution where discardable sections have long names preserved and
  // non-discardable sections have their names truncated, to ensure that any
  // section which is mapped at runtime also has its name mapped at runtime.
  SmallVector<OutputSection *> longNameSections;
  for (OutputSection *sec : ctx.outputSections) {
    if (sec->name.size() <= COFF::NameSize)
      continue;
    if ((sec->header.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0)
      continue;
    if (ctx.config.warnLongSectionNames) {
      Warn(ctx)
          << "section name " << sec->name
          << " is longer than 8 characters and will use a non-standard string "
             "table";
    }
    // Put the section name in the begin of strtab so that its offset is less
    // than Max7DecimalOffset otherwise lldb/gdb will not read it.
    strtab.add(sec->name, /*Priority=*/UINT8_MAX);
    longNameSections.push_back(sec);
  }

  std::vector<std::pair<size_t, StringRef>> longNameSymbols;
  if (ctx.config.writeSymtab) {
    for (ObjFile *file : ctx.objFileInstances) {
      for (Symbol *b : file->getSymbols()) {
        auto *d = dyn_cast_or_null<Defined>(b);
        if (!d || d->writtenToSymtab)
          continue;
        d->writtenToSymtab = true;
        if (auto *dc = dyn_cast_or_null<DefinedCOFF>(d)) {
          COFFSymbolRef symRef = dc->getCOFFSymbol();
          if (symRef.isSectionDefinition() ||
              symRef.getStorageClass() == COFF::IMAGE_SYM_CLASS_LABEL)
            continue;
        }

        if (std::optional<coff_symbol16> sym = createSymbol(d)) {
          if (d->getName().size() > COFF::NameSize)
            longNameSymbols.emplace_back(outputSymtab.size(), d->getName());
          outputSymtab.push_back(*sym);
        }

        if (auto *dthunk = dyn_cast<DefinedImportThunk>(d)) {
          if (!dthunk->wrappedSym->writtenToSymtab) {
            dthunk->wrappedSym->writtenToSymtab = true;
            if (std::optional<coff_symbol16> sym =
                    createSymbol(dthunk->wrappedSym)) {
              if (dthunk->wrappedSym->getName().size() > COFF::NameSize)
                longNameSymbols.emplace_back(outputSymtab.size(),
                                             dthunk->wrappedSym->getName());
              outputSymtab.push_back(*sym);
            }
          }
        }
      }
    }
  }

  if (outputSymtab.empty() && strtab.empty())
    return;

  strtab.finalize();
  for (OutputSection *sec : longNameSections)
    sec->setStringTableOff(strtab.getOffset(sec->name));
  for (auto P : longNameSymbols) {
    coff_symbol16 &sym = outputSymtab[P.first];
    sym.Name.Offset.Offset = strtab.getOffset(P.second);
  }

  // We position the symbol table to be adjacent to the end of the last section.
  uint64_t fileOff = fileSize;
  pointerToSymbolTable = fileOff;
  fileOff += outputSymtab.size() * sizeof(coff_symbol16);
  fileOff += strtab.getSize();
  fileSize = alignTo(fileOff, ctx.config.fileAlign);
}

void Writer::mergeSection(const std::map<StringRef, StringRef>::value_type &p) {
  StringRef toName = p.second;
  if (p.first == toName)
    return;
  StringSet<> names;
  while (true) {
    if (!names.insert(toName).second)
      Fatal(ctx) << "/merge: cycle found for section '" << p.first << "'";
    auto i = ctx.config.merge.find(toName);
    if (i == ctx.config.merge.end())
      break;
    toName = i->second;
  }
  OutputSection *from = findSection(p.first);
  OutputSection *to = findSection(toName);
  if (!from)
    return;
  if (!to) {
    from->name = toName;
    return;
  }
  to->merge(from);
}

void Writer::mergeSections() {
  llvm::TimeTraceScope timeScope("Merge sections");
  if (!pdataSec->chunks.empty()) {
    if (isArm64EC(ctx.config.machine)) {
      // On ARM64EC .pdata may contain both ARM64 and X64 data. Split them by
      // sorting and store their regions separately.
      llvm::stable_sort(pdataSec->chunks, [=](const Chunk *a, const Chunk *b) {
        return (a->getMachine() == AMD64) < (b->getMachine() == AMD64);
      });

      for (auto chunk : pdataSec->chunks) {
        if (chunk->getMachine() == AMD64) {
          hybridPdata.first = chunk;
          hybridPdata.last = pdataSec->chunks.back();
          break;
        }

        if (!pdata.first)
          pdata.first = chunk;
        pdata.last = chunk;
      }
    } else {
      pdata.first = pdataSec->chunks.front();
      pdata.last = pdataSec->chunks.back();
    }
  }

  for (auto &p : ctx.config.merge) {
    if (p.first != ".bss")
      mergeSection(p);
  }

  // Because .bss contains all zeros, it should be merged at the end of
  // whatever section it is being merged into (usually .data) so that the image
  // need not actually contain all of the zeros.
  auto it = ctx.config.merge.find(".bss");
  if (it != ctx.config.merge.end())
    mergeSection(*it);
}

// EC targets may have chunks of various architectures mixed together at this
// point. Group code chunks of the same architecture together by sorting chunks
// by their EC range type.
void Writer::sortECChunks() {
  if (!isArm64EC(ctx.config.machine))
    return;

  for (OutputSection *sec : ctx.outputSections) {
    if (sec->isCodeSection())
      llvm::stable_sort(sec->chunks, [=](const Chunk *a, const Chunk *b) {
        std::optional<chpe_range_type> aType = a->getArm64ECRangeType(),
                                       bType = b->getArm64ECRangeType();
        return bType && (!aType || *aType < *bType);
      });
  }
}

// Visits all sections to assign incremental, non-overlapping RVAs and
// file offsets.
void Writer::assignAddresses() {
  llvm::TimeTraceScope timeScope("Assign addresses");
  Configuration *config = &ctx.config;

  // We need to create EC code map so that ECCodeMapChunk knows its size.
  // We do it here to make sure that we account for range extension chunks.
  createECCodeMap();

  sizeOfHeaders = dosStubSize + sizeof(PEMagic) + sizeof(coff_file_header) +
                  sizeof(data_directory) * numberOfDataDirectory +
                  sizeof(coff_section) * ctx.outputSections.size();
  sizeOfHeaders +=
      config->is64() ? sizeof(pe32plus_header) : sizeof(pe32_header);
  sizeOfHeaders = alignTo(sizeOfHeaders, config->fileAlign);
  fileSize = sizeOfHeaders;

  // The first page is kept unmapped.
  uint64_t rva = alignTo(sizeOfHeaders, config->align);

  for (OutputSection *sec : ctx.outputSections) {
    llvm::TimeTraceScope timeScope("Section: ", sec->name);
    if (sec == relocSec) {
      sec->chunks.clear();
      addBaserels();
      if (ctx.dynamicRelocs) {
        ctx.dynamicRelocs->finalize();
        relocSec->addChunk(ctx.dynamicRelocs);
      }
    }
    uint64_t rawSize = 0, virtualSize = 0;
    sec->header.VirtualAddress = rva;

    // If /FUNCTIONPADMIN is used, functions are padded in order to create a
    // hotpatchable image.
    uint32_t padding = sec->isCodeSection() ? config->functionPadMin : 0;
    std::optional<chpe_range_type> prevECRange;

    for (Chunk *c : sec->chunks) {
      // Alignment EC code range baudaries.
      if (isArm64EC(ctx.config.machine) && sec->isCodeSection()) {
        std::optional<chpe_range_type> rangeType = c->getArm64ECRangeType();
        if (rangeType != prevECRange) {
          virtualSize = alignTo(virtualSize, 4096);
          prevECRange = rangeType;
        }
      }
      if (padding && c->isHotPatchable())
        virtualSize += padding;
      // If chunk has EC entry thunk, reserve a space for an offset to the
      // thunk.
      if (c->getEntryThunk())
        virtualSize += sizeof(uint32_t);
      virtualSize = alignTo(virtualSize, c->getAlignment());
      c->setRVA(rva + virtualSize);
      virtualSize += c->getSize();
      if (c->hasData)
        rawSize = alignTo(virtualSize, config->fileAlign);
    }
    if (virtualSize > UINT32_MAX)
      Err(ctx) << "section larger than 4 GiB: " << sec->name;
    sec->header.VirtualSize = virtualSize;
    sec->header.SizeOfRawData = rawSize;
    if (rawSize != 0)
      sec->header.PointerToRawData = fileSize;
    rva += alignTo(virtualSize, config->align);
    fileSize += alignTo(rawSize, config->fileAlign);
  }
  sizeOfImage = alignTo(rva, config->align);

  // Assign addresses to sections in MergeChunks.
  if (providerSymbols)
    return;
  for (MergeChunk *mc : ctx.mergeChunkInstances)
    if (mc)
      mc->assignSubsectionRVAs();
}

template <typename PEHeaderTy> void Writer::writeHeader() {
  // Write DOS header. For backwards compatibility, the first part of a PE/COFF
  // executable consists of an MS-DOS MZ executable. If the executable is run
  // under DOS, that program gets run (usually to just print an error message).
  // When run under Windows, the loader looks at AddressOfNewExeHeader and uses
  // the PE header instead.
  Configuration *config = &ctx.config;

  uint8_t *buf = buffer->getBufferStart();
  auto *dos = reinterpret_cast<dos_header *>(buf);

  // Write DOS program.
  if (config->dosStub) {
    memcpy(buf, config->dosStub->getBufferStart(),
           config->dosStub->getBufferSize());
    // MS link.exe accepts an invalid `e_lfanew` (AddressOfNewExeHeader) and
    // updates it automatically. Replicate the same behaviour.
    dos->AddressOfNewExeHeader = alignTo(config->dosStub->getBufferSize(), 8);
    // Unlike MS link.exe, LLD accepts non-8-byte-aligned stubs.
    // In that case, we add zero paddings ourselves.
    buf += alignTo(config->dosStub->getBufferSize(), 8);
  } else {
    buf += sizeof(dos_header);
    dos->Magic[0] = 'M';
    dos->Magic[1] = 'Z';
    dos->UsedBytesInTheLastPage = dosStubSize % 512;
    dos->FileSizeInPages = divideCeil(dosStubSize, 512);
    dos->HeaderSizeInParagraphs = sizeof(dos_header) / 16;

    dos->AddressOfRelocationTable = sizeof(dos_header);
    dos->AddressOfNewExeHeader = dosStubSize;

    memcpy(buf, dosProgram, sizeof(dosProgram));
    buf += sizeof(dosProgram);
  }

  // Make sure DOS stub is aligned to 8 bytes at this point
  assert((buf - buffer->getBufferStart()) % 8 == 0);

  // Write PE magic
  memcpy(buf, PEMagic, sizeof(PEMagic));
  buf += sizeof(PEMagic);

  // Write COFF header
  assert(coffHeaderOffset ==
         static_cast<size_t>(buf - buffer->getBufferStart()));
  auto *coff = reinterpret_cast<coff_file_header *>(buf);
  buf += sizeof(*coff);
  SymbolTable &symtab = providerSymbols                ? *providerSymbols
                        : ctx.config.machine == ARM64X ? *ctx.hybridSymtab
                                                       : ctx.getOutputSymtab();
  coff->Machine = symtab.isEC() ? AMD64 : symtab.machine;
  coff->NumberOfSections = ctx.outputSections.size();
  coff->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
  if (config->largeAddressAware)
    coff->Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
  if (!config->is64())
    coff->Characteristics |= IMAGE_FILE_32BIT_MACHINE;
  if (config->dll)
    coff->Characteristics |= IMAGE_FILE_DLL;
  if (config->driverUponly)
    coff->Characteristics |= IMAGE_FILE_UP_SYSTEM_ONLY;
  if (!config->relocatable)
    coff->Characteristics |= IMAGE_FILE_RELOCS_STRIPPED;
  if (config->swaprunCD)
    coff->Characteristics |= IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP;
  if (config->swaprunNet)
    coff->Characteristics |= IMAGE_FILE_NET_RUN_FROM_SWAP;
  coff->SizeOfOptionalHeader =
      sizeof(PEHeaderTy) + sizeof(data_directory) * numberOfDataDirectory;

  // Write PE header
  assert(peHeaderOffset == static_cast<size_t>(buf - buffer->getBufferStart()));
  auto *pe = reinterpret_cast<PEHeaderTy *>(buf);
  buf += sizeof(*pe);
  pe->Magic = config->is64() ? PE32Header::PE32_PLUS : PE32Header::PE32;

  // If {Major,Minor}LinkerVersion is left at 0.0, then for some
  // reason signing the resulting PE file with Authenticode produces a
  // signature that fails to validate on Windows 7 (but is OK on 10).
  // Set it to 14.0, which is what VS2015 outputs, and which avoids
  // that problem.
  pe->MajorLinkerVersion = 14;
  pe->MinorLinkerVersion = 0;

  pe->ImageBase = config->imageBase;
  pe->SectionAlignment = config->align;
  pe->FileAlignment = config->fileAlign;
  pe->MajorImageVersion = config->majorImageVersion;
  pe->MinorImageVersion = config->minorImageVersion;
  pe->MajorOperatingSystemVersion = config->majorOSVersion;
  pe->MinorOperatingSystemVersion = config->minorOSVersion;
  pe->MajorSubsystemVersion = config->majorSubsystemVersion;
  pe->MinorSubsystemVersion = config->minorSubsystemVersion;
  pe->Subsystem = config->subsystem;
  pe->SizeOfImage = sizeOfImage;
  pe->SizeOfHeaders = sizeOfHeaders;
  if (!config->noEntry) {
    Defined *entry = cast<Defined>(symtab.entry);
    pe->AddressOfEntryPoint = entry->getRVA();
    // Pointer to thumb code must have the LSB set, so adjust it.
    if (config->machine == ARMNT)
      pe->AddressOfEntryPoint |= 1;
  }
  pe->SizeOfStackReserve = config->stackReserve;
  pe->SizeOfStackCommit = config->stackCommit;
  pe->SizeOfHeapReserve = config->heapReserve;
  pe->SizeOfHeapCommit = config->heapCommit;
  if (config->appContainer)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_APPCONTAINER;
  if (config->driverWdm)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_WDM_DRIVER;
  if (config->dynamicBase)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE;
  if (config->highEntropyVA)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA;
  if (!config->allowBind)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_NO_BIND;
  if (config->nxCompat)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_NX_COMPAT;
  if (!config->allowIsolation)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_NO_ISOLATION;
  if (config->guardCF != GuardCFLevel::Off)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_GUARD_CF;
  if (config->integrityCheck)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_FORCE_INTEGRITY;
  if (setNoSEHCharacteristic || config->noSEH)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_NO_SEH;
  if (config->terminalServerAware)
    pe->DLLCharacteristics |= IMAGE_DLL_CHARACTERISTICS_TERMINAL_SERVER_AWARE;
  pe->NumberOfRvaAndSize = numberOfDataDirectory;
  if (textSec->getVirtualSize()) {
    pe->BaseOfCode = textSec->getRVA();
    pe->SizeOfCode = textSec->getRawSize();
  }
  pe->SizeOfInitializedData = getSizeOfInitializedData();

  // Write data directory
  assert(!ctx.config.is64() ||
         dataDirOffset64 ==
             static_cast<size_t>(buf - buffer->getBufferStart()));
  auto *dir = reinterpret_cast<data_directory *>(buf);
  buf += sizeof(*dir) * numberOfDataDirectory;
  if (symtab.edataStart) {
    dir[EXPORT_TABLE].RelativeVirtualAddress = symtab.edataStart->getRVA();
    dir[EXPORT_TABLE].Size = symtab.edataEnd->getRVA() +
                             symtab.edataEnd->getSize() -
                             symtab.edataStart->getRVA();
  }
  if (importTableStart) {
    dir[IMPORT_TABLE].RelativeVirtualAddress = importTableStart->getRVA();
    dir[IMPORT_TABLE].Size = importTableSize;
  }
  if (iatStart) {
    dir[IAT].RelativeVirtualAddress = iatStart->getRVA();
    dir[IAT].Size = getIATSize();
  }
  if (rsrcSec->getVirtualSize()) {
    dir[RESOURCE_TABLE].RelativeVirtualAddress = rsrcSec->getRVA();
    dir[RESOURCE_TABLE].Size = rsrcSec->getVirtualSize();
  }
  // ARM64EC (but not ARM64X) contains x86_64 exception table in data directory.
  ChunkRange &exceptionTable =
      ctx.config.machine == ARM64EC ? hybridPdata : pdata;
  if (exceptionTable.first) {
    dir[EXCEPTION_TABLE].RelativeVirtualAddress =
        exceptionTable.first->getRVA();
    dir[EXCEPTION_TABLE].Size = exceptionTable.last->getRVA() +
                                exceptionTable.last->getSize() -
                                exceptionTable.first->getRVA();
  }
  size_t relocSize = relocSec->getVirtualSize();
  if (ctx.dynamicRelocs)
    relocSize -= ctx.dynamicRelocs->getSize();
  if (relocSize) {
    dir[BASE_RELOCATION_TABLE].RelativeVirtualAddress = relocSec->getRVA();
    dir[BASE_RELOCATION_TABLE].Size = relocSize;
  }
  if (Symbol *sym = symtab.findUnderscore("_tls_used")) {
    if (Defined *b = dyn_cast<Defined>(sym)) {
      dir[TLS_TABLE].RelativeVirtualAddress = b->getRVA();
      dir[TLS_TABLE].Size = config->is64()
                                ? sizeof(object::coff_tls_directory64)
                                : sizeof(object::coff_tls_directory32);
    }
  }
  if (debugDirectory) {
    dir[DEBUG_DIRECTORY].RelativeVirtualAddress = debugDirectory->getRVA();
    dir[DEBUG_DIRECTORY].Size = debugDirectory->getSize();
  }
  if (symtab.loadConfigSym) {
    dir[LOAD_CONFIG_TABLE].RelativeVirtualAddress =
        symtab.loadConfigSym->getRVA();
    dir[LOAD_CONFIG_TABLE].Size = symtab.loadConfigSize;
  }
  if (!delayIdata.empty()) {
    dir[DELAY_IMPORT_DESCRIPTOR].RelativeVirtualAddress =
        delayIdata.getDirRVA();
    dir[DELAY_IMPORT_DESCRIPTOR].Size = delayIdata.getDirSize();
  }

  // Write section table
  for (OutputSection *sec : ctx.outputSections) {
    sec->writeHeaderTo(buf, config->debug);
    buf += sizeof(coff_section);
  }
  sectionTable = ArrayRef<uint8_t>(
      buf - ctx.outputSections.size() * sizeof(coff_section), buf);

  if (outputSymtab.empty() && strtab.empty())
    return;

  coff->PointerToSymbolTable = pointerToSymbolTable;
  uint32_t numberOfSymbols = outputSymtab.size();
  coff->NumberOfSymbols = numberOfSymbols;
  auto *symbolTable = reinterpret_cast<coff_symbol16 *>(
      buffer->getBufferStart() + coff->PointerToSymbolTable);
  for (size_t i = 0; i != numberOfSymbols; ++i)
    symbolTable[i] = outputSymtab[i];
  // Create the string table, it follows immediately after the symbol table.
  // The first 4 bytes is length including itself.
  buf = reinterpret_cast<uint8_t *>(&symbolTable[numberOfSymbols]);
  strtab.write(buf);
}

void Writer::openFile(StringRef path) {
  buffer = CHECK(
      FileOutputBuffer::create(ctx.outputFiles.stage(path), fileSize,
                               FileOutputBuffer::F_executable),
      "failed to open " + path);
}

void Writer::createSEHTable() {
  SymbolRVASet handlers;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!file->hasSafeSEH())
      Err(ctx) << "/safeseh: " << file->getName()
               << " is not compatible with SEH";
    markSymbolsForRVATable(file, file->getSXDataChunks(), handlers);
  }

  // Set the "no SEH" characteristic if there really were no handlers, or if
  // there is no load config object to point to the table of handlers.
  setNoSEHCharacteristic =
      handlers.empty() || !ctx.getOutputSymtab().findUnderscore("_load_config_used");

  maybeAddRVATable(std::move(handlers), "__safe_se_handler_table",
                   "__safe_se_handler_count");
}

// Add a symbol to an RVA set. Two symbols may have the same RVA, but an RVA set
// cannot contain duplicates. Therefore, the set is uniqued by Chunk and the
// symbol's offset into that Chunk.
static void addSymbolToRVASet(SymbolRVASet &rvaSet, Defined *s) {
  Chunk *c = s->getChunk();
  if (!c)
    return;
  if (auto *sc = dyn_cast<SectionChunk>(c))
    c = sc->repl; // Look through ICF replacement.
  uint32_t off = s->getRVA() - (c ? c->getRVA() : 0);
  rvaSet.insert({c, off});
}

// Given a symbol, add it to the GFIDs table if it is a live, defined, function
// symbol in an executable section.
static void maybeAddAddressTakenFunction(SymbolRVASet &addressTakenSyms,
                                         Symbol *s) {
  if (!s)
    return;

  switch (s->kind()) {
  case Symbol::DefinedLocalImportKind:
  case Symbol::DefinedImportDataKind:
    // Defines an __imp_ pointer, so it is data, so it is ignored.
    break;
  case Symbol::DefinedCommonKind:
    // Common is always data, so it is ignored.
    break;
  case Symbol::DefinedAbsoluteKind:
    // Absolute is never code, synthetic generally isn't and usually isn't
    // determinable.
    break;
  case Symbol::DefinedSyntheticKind:
    // For EC export thunks, mark both the thunk itself and its target.
    if (auto expChunk = dyn_cast_or_null<ECExportThunkChunk>(
            cast<Defined>(s)->getChunk())) {
      addSymbolToRVASet(addressTakenSyms, cast<Defined>(s));
      addSymbolToRVASet(addressTakenSyms, expChunk->target);
    }
    break;
  case Symbol::LazyArchiveKind:
  case Symbol::LazyObjectKind:
  case Symbol::LazyDLLSymbolKind:
  case Symbol::UndefinedKind:
    // Undefined symbols resolve to zero, so they don't have an RVA. Lazy
    // symbols shouldn't have relocations.
    break;

  case Symbol::DefinedImportThunkKind:
    // Thunks are always code, include them.
    addSymbolToRVASet(addressTakenSyms, cast<Defined>(s));
    break;

  case Symbol::DefinedRegularKind: {
    // This is a regular, defined, symbol from a COFF file. Mark the symbol as
    // address taken if the symbol type is function and it's in an executable
    // section.
    auto *d = cast<DefinedRegular>(s);
    if (d->getCOFFSymbol().getComplexType() == COFF::IMAGE_SYM_DTYPE_FUNCTION) {
      SectionChunk *sc = dyn_cast<SectionChunk>(d->getChunk());
      if (sc && sc->live &&
          sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE)
        addSymbolToRVASet(addressTakenSyms, d);
    }
    break;
  }
  }
}

// Visit all relocations from all section contributions of this object file and
// mark the relocation target as address-taken.
void Writer::markSymbolsWithRelocations(ObjFile *file,
                                        SymbolRVASet &usedSymbols,
                                        SymbolRVASet &usedImports) {
  for (Chunk *c : file->getChunks()) {
    // We only care about live section chunks. Common chunks and other chunks
    // don't generally contain relocations.
    SectionChunk *sc = dyn_cast<SectionChunk>(c);
    if (!sc || !sc->live)
      continue;

    for (const coff_relocation &reloc : sc->getRelocs()) {
      if (ctx.config.machine == I386 &&
          reloc.Type == COFF::IMAGE_REL_I386_REL32)
        // Ignore relative relocations on x86. On x86_64 they can't be ignored
        // since they're also used to compute absolute addresses.
        continue;

      Symbol *ref = sc->file->getSymbol(reloc.SymbolTableIndex);
      // An import pointer read by anything but a call or jump through it
      // hands the imported address to code that may check it.
      if (auto *imp = dyn_cast_or_null<DefinedImportData>(ref)) {
        SectionChunk::ImportRefForm form = sc->getImportRefForm(reloc);
        if (form != SectionChunk::ImportRefForm::Call &&
            form != SectionChunk::ImportRefForm::Jump)
          addSymbolToRVASet(usedImports, imp);
        continue;
      }
      maybeAddAddressTakenFunction(usedSymbols, ref);
    }
  }
}

// Create the guard function id table. This is a table of RVAs of all
// address-taken functions. It is sorted and uniqued, just like the safe SEH
// table.
void Writer::createGuardCFTables() {
  Configuration *config = &ctx.config;

  // The delay-load import address table is alone in a section, so the loader
  // can keep it read-only and open it only while it resolves an import. Both
  // flags are needed: the first alone leaves the table writable until the
  // first resolution, which then makes its pages read-only for good.
  uint32_t guardFlags = 0;
  if (protectDelayIat())
    guardFlags = uint32_t(GuardFlags::PROTECT_DELAYLOAD_IAT) |
                 uint32_t(GuardFlags::DELAYLOAD_IAT_IN_ITS_OWN_SECTION);

  if (config->guardCF == GuardCFLevel::Off) {
    // MSVC marks the entire image as instrumented if any input object was built
    // with /guard:cf.
    for (ObjFile *file : ctx.objFileInstances) {
      if (file->hasGuardCF()) {
        guardFlags |= uint32_t(GuardFlags::CF_INSTRUMENTED);
        break;
      }
    }
    if (guardFlags)
      ctx.forEachSymtab([&](SymbolTable &symtab) {
        Symbol *flagSym = symtab.findUnderscore("__guard_flags");
        cast<DefinedAbsolute>(flagSym)->setVA(guardFlags);
      });
    return;
  }

  SymbolRVASet addressTakenSyms;
  SymbolRVASet giatsRVASet;
  std::vector<Symbol *> giatsSymbols;
  SymbolRVASet longJmpTargets;
  SymbolRVASet ehContTargets;
  for (ObjFile *file : ctx.objFileInstances) {
    // If the object was compiled with /guard:cf, the address taken symbols
    // are in .gfids$y sections, and the longjmp targets are in .gljmp$y
    // sections. If the object was not compiled with /guard:cf, we assume there
    // were no setjmp targets, and that all code symbols with relocations are
    // possibly address-taken.
    if (file->hasGuardCF()) {
      markSymbolsForRVATable(file, file->getGuardFidChunks(), addressTakenSyms);
      markSymbolsForRVATable(file, file->getGuardIATChunks(), giatsRVASet);
      getSymbolsFromSections(file, file->getGuardIATChunks(), giatsSymbols);
      markSymbolsForRVATable(file, file->getGuardLJmpChunks(), longJmpTargets);
    } else {
      markSymbolsWithRelocations(file, addressTakenSyms, giatsRVASet);
    }
    // If the object was compiled with /guard:ehcont, the ehcont targets are in
    // .gehcont$y sections.
    if (file->hasGuardEHCont())
      markSymbolsForRVATable(file, file->getGuardEHContChunks(), ehContTargets);
  }

  // Mark the image entry as address-taken.
  SymbolRVASet exportedSyms;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    if (symtab.entry)
      maybeAddAddressTakenFunction(addressTakenSyms, symtab.entry);

    // Mark exported symbols in executable sections as address-taken.
    for (Export &e : symtab.exports)
      if (e.forwardTo.empty())
        maybeAddAddressTakenFunction(exportedSyms, e.sym);
  });

  // Import entries whose address code takes through a rewritten thunk
  // reference (SymbolTable::bindLocalImports), and in-place import slots that
  // hold a function's address.
  for (ImportFile *file : ctx.importFileInstances)
    if (file->live && file->impSym && file->impSym->addressTaken)
      addSymbolToRVASet(giatsRVASet, file->impSym);
  for (const ImportSlot &s : importSlots)
    if (s.sym->file->thunkSym)
      giatsRVASet.insert({s.chunk, s.offset});
  // The C runtime calls ImportFillChunk through its initializer table.
  if (importFill)
    addressTakenSyms.insert({importFill, 0});

  // For each entry in the .giats table, check if it has a corresponding load
  // thunk (e.g. because the DLL that defines it will be delay-loaded) and, if
  // so, add the load thunk to the address taken (.gfids) table.
  for (Symbol *s : giatsSymbols) {
    if (auto *di = dyn_cast<DefinedImportData>(s)) {
      if (di->loadThunkSym)
        addSymbolToRVASet(addressTakenSyms, di->loadThunkSym);
    }
  }

  // Ensure sections referenced in the gfid table are 16-byte aligned.
  for (const SymbolRVASet *set : {&addressTakenSyms, &exportedSyms})
    for (const ChunkAndOffset &c : *set)
      if (c.inputChunk->getAlignment() < 16)
        c.inputChunk->setAlignment(16);

  // A function that is a target only because it is exported is marked as
  // export-suppressed: a process that enables the suppression accepts it once
  // GetProcAddress has handed it out. The mark needs a 16-byte aligned entry.
  SymbolRVASet exportSuppressed;
  for (const ChunkAndOffset &c : exportedSyms)
    if (!addressTakenSyms.contains(c))
      (c.offset % 16 == 0 ? exportSuppressed : addressTakenSyms).insert(c);

  maybeAddRVATable(std::move(addressTakenSyms), "__guard_fids_table",
                   "__guard_fids_count", true, std::move(exportSuppressed));

  // Add the Guard Address Taken IAT Entry Table (.giats).
  maybeAddRVATable(std::move(giatsRVASet), "__guard_iat_table",
                   "__guard_iat_count", true);

  // Add the longjmp target table unless the user told us not to.
  if (config->guardCF & GuardCFLevel::LongJmp)
    maybeAddRVATable(std::move(longJmpTargets), "__guard_longjmp_table",
                     "__guard_longjmp_count", true);

  // Add the ehcont target table unless the user told us not to.
  if (config->guardCF & GuardCFLevel::EHCont)
    maybeAddRVATable(std::move(ehContTargets), "__guard_eh_cont_table",
                     "__guard_eh_cont_count", true);

  // Set __guard_flags, which will be used in the load config to indicate that
  // /guard:cf was enabled. Every table has a flag byte per entry.
  guardFlags |= uint32_t(GuardFlags::CF_INSTRUMENTED) |
                uint32_t(GuardFlags::CF_FUNCTION_TABLE_PRESENT) |
                uint32_t(GuardFlags::CF_FUNCTION_TABLE_SIZE_5BYTES);
  if (config->guardCF & GuardCFLevel::LongJmp)
    guardFlags |= uint32_t(GuardFlags::CF_LONGJUMP_TABLE_PRESENT);
  if (config->guardCF & GuardCFLevel::EHCont)
    guardFlags |= uint32_t(GuardFlags::EH_CONTINUATION_TABLE_PRESENT);
  if (config->guardCF & GuardCFLevel::ExportSuppress)
    guardFlags |= uint32_t(GuardFlags::CF_ENABLE_EXPORT_SUPPRESSION);
  // The export marks and the address-taken IAT table are complete, which
  // matters only where the load configuration reaches that table.
  size_t giatsEnd =
      config->is64()
          ? offsetof(coff_load_configuration64, GuardLongJumpTargetTable)
          : offsetof(coff_load_configuration32, GuardLongJumpTargetTable);
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    uint32_t flags = guardFlags;
    if (symtab.loadConfigSize >= giatsEnd)
      flags |= uint32_t(GuardFlags::CF_EXPORT_SUPPRESSION_INFO_PRESENT);
    Symbol *flagSym = symtab.findUnderscore("__guard_flags");
    cast<DefinedAbsolute>(flagSym)->setVA(flags);
  });
}

// Take a list of input sections containing symbol table indices and add those
// symbols to a vector. The challenge is that symbol RVAs are not known and
// depend on the table size, so we can't directly build a set of integers.
void Writer::getSymbolsFromSections(ObjFile *file,
                                    ArrayRef<SectionChunk *> symIdxChunks,
                                    std::vector<Symbol *> &symbols) {
  for (SectionChunk *c : symIdxChunks) {
    // Skip sections discarded by linker GC. This comes up when a .gfids section
    // is associated with something like a vtable and the vtable is discarded.
    // In this case, the associated gfids section is discarded, and we don't
    // mark the virtual member functions as address-taken by the vtable.
    if (!c->live)
      continue;

    // Validate that the contents look like symbol table indices.
    ArrayRef<uint8_t> data = c->getContents();
    if (data.size() % 4 != 0) {
      Warn(ctx) << "ignoring " << c->getSectionName()
                << " symbol table index section in object " << file;
      continue;
    }

    // Read each symbol table index and check if that symbol was included in the
    // final link. If so, add it to the vector of symbols.
    ArrayRef<ulittle32_t> symIndices(
        reinterpret_cast<const ulittle32_t *>(data.data()), data.size() / 4);
    ArrayRef<Symbol *> objSymbols = file->getSymbols();
    for (uint32_t symIndex : symIndices) {
      if (symIndex >= objSymbols.size()) {
        Warn(ctx) << "ignoring invalid symbol table index in section "
                  << c->getSectionName() << " in object " << file;
        continue;
      }
      Symbol *s = objSymbols[symIndex];
      if (ctx.outputPartition) {
        Symbol *original = ctx.partitions->originalSymbol(file, symIndex);
        if (auto *def = dyn_cast_or_null<Defined>(original))
          if (isa<DefinedRegular, DefinedCommon>(def) &&
              ctx.partitions->owner(def->getChunk()) != ctx.outputPartition->index)
            continue;
        // A symbol-index record does not make a new native import live. Only
        // the surviving input relocations created this output's import view.
        if (isa_and_nonnull<DefinedImportData, DefinedImportThunk>(original) &&
            !ctx.outputPartition->projections.contains(original))
          continue;
      }
      if (s) {
        if (s->isLive())
          symbols.push_back(cast<Symbol>(s));
      }
    }
  }
}

// Take a list of input sections containing symbol table indices and add those
// symbols to an RVA table.
void Writer::markSymbolsForRVATable(ObjFile *file,
                                    ArrayRef<SectionChunk *> symIdxChunks,
                                    SymbolRVASet &tableSymbols) {
  std::vector<Symbol *> syms;
  getSymbolsFromSections(file, symIdxChunks, syms);

  for (Symbol *s : syms)
    addSymbolToRVASet(tableSymbols, cast<Defined>(s));
}

// Replace the absolute table symbol with a synthetic symbol pointing to
// tableChunk so that we can emit base relocations for it and resolve section
// relative relocations.
void Writer::maybeAddRVATable(SymbolRVASet tableSymbols, StringRef tableSym,
                              StringRef countSym, bool hasFlag,
                              SymbolRVASet exportSuppressed) {
  if (tableSymbols.empty() && exportSuppressed.empty())
    return;

  NonSectionChunk *tableChunk;
  if (hasFlag)
    tableChunk = make<RVAFlagTableChunk>(std::move(tableSymbols),
                                         std::move(exportSuppressed));
  else
    tableChunk = make<RVATableChunk>(std::move(tableSymbols));
  rdataSec->addChunk(tableChunk);

  ctx.forEachSymtab([&](SymbolTable &symtab) {
    Symbol *t = symtab.findUnderscore(tableSym);
    Symbol *c = symtab.findUnderscore(countSym);
    replaceSymbol<DefinedSynthetic>(t, t->getName(), tableChunk);
    cast<DefinedAbsolute>(c)->setVA(tableChunk->getSize() / (hasFlag ? 5 : 4));
  });
}

// Create CHPE metadata chunks.
void Writer::createECChunks() {
  if (!ctx.getOutputSymtab().isEC())
    return;

  for (Symbol *s : ctx.getOutputSymtab().expSymbols) {
    auto sym = dyn_cast<Defined>(s);
    if (!sym || !sym->getChunk())
      continue;
    if (auto thunk = dyn_cast<ECExportThunkChunk>(sym->getChunk())) {
      hexpthkSec->addChunk(thunk);
      exportThunks.push_back({thunk, thunk->target});
    } else if (auto def = dyn_cast<DefinedRegular>(sym)) {
      // Allow section chunk to be treated as an export thunk if it looks like
      // one.
      SectionChunk *chunk = def->getChunk();
      if (!chunk->live || chunk->getMachine() != AMD64)
        continue;
      assert(sym->getName().starts_with("EXP+"));
      StringRef targetName = sym->getName().substr(strlen("EXP+"));
      // If EXP+#foo is an export thunk of a hybrid patchable function,
      // we should use the #foo$hp_target symbol as the redirection target.
      // First, try to look up the $hp_target symbol. If it can't be found,
      // assume it's a regular function and look for #foo instead.
      Symbol *targetSym = ctx.getOutputSymtab().find((targetName + "$hp_target").str());
      if (!targetSym)
        targetSym = ctx.getOutputSymtab().find(targetName);
      Defined *t = dyn_cast_or_null<Defined>(targetSym);
      if (t && isArm64EC(t->getChunk()->getMachine()))
        exportThunks.push_back({chunk, t});
    }
  }

  auto codeMapChunk = make<ECCodeMapChunk>(codeMap);
  rdataSec->addChunk(codeMapChunk);
  Symbol *codeMapSym = ctx.getOutputSymtab().findUnderscore("__hybrid_code_map");
  replaceSymbol<DefinedSynthetic>(codeMapSym, codeMapSym->getName(),
                                  codeMapChunk);

  CHPECodeRangesChunk *ranges = make<CHPECodeRangesChunk>(exportThunks);
  rdataSec->addChunk(ranges);
  Symbol *rangesSym =
      ctx.getOutputSymtab().findUnderscore("__x64_code_ranges_to_entry_points");
  replaceSymbol<DefinedSynthetic>(rangesSym, rangesSym->getName(), ranges);

  CHPERedirectionChunk *entryPoints = make<CHPERedirectionChunk>(exportThunks);
  a64xrmSec->addChunk(entryPoints);
  Symbol *entryPointsSym =
      ctx.getOutputSymtab().findUnderscore("__arm64x_redirection_metadata");
  replaceSymbol<DefinedSynthetic>(entryPointsSym, entryPointsSym->getName(),
                                  entryPoints);

  for (auto thunk : ctx.getOutputSymtab().sameAddressThunks) {
    // Relocation values are set later in setECSymbols.
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           thunk);
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           Arm64XRelocVal(thunk, sizeof(uint32_t)));
  }
}

// MinGW specific. Gather all relocations that are imported from a DLL even
// though the code didn't expect it to, produce the table that the runtime
// uses for fixing them up, and provide the synthetic symbols that the
// runtime uses for finding the table.
void Writer::createRuntimePseudoRelocs() {
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    std::vector<RuntimePseudoReloc> rels;

    for (Chunk *c : ctx.driver.getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live || !symtab.owns(sc->file))
        continue;
      // Don't create pseudo relocations for sections that won't be
      // mapped at runtime.
      if (sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
        continue;
      sc->getRuntimePseudoRelocs(rels);
    }

    if (!ctx.config.pseudoRelocs) {
      // Not writing any pseudo relocs; if some were needed, error out and
      // indicate what required them. With in-place import slots, the loader
      // fills static data and only a reference from code is left over.
      for (const RuntimePseudoReloc &rpr : rels) {
        if (ctx.config.importSlots)
          Err(ctx) << toString(rpr.target->file) << ": " << rpr.sym->getName()
                   << " is imported, but is referenced as if it were local; "
                      "mark its declaration as imported, or compile with "
                      "-fauto-import";
        else
          Err(ctx) << "automatic dllimport of " << rpr.sym->getName() << " in "
                   << toString(rpr.target->file)
                   << " requires pseudo relocations";
      }
      return;
    }

    if (!rels.empty()) {
      Log(ctx) << "Writing " << Twine(rels.size())
               << " runtime pseudo relocations";
      const char *symbolName = "_pei386_runtime_relocator";
      Symbol *relocator = symtab.findUnderscore(symbolName);
      if (!relocator)
        Err(ctx)
            << "output image has runtime pseudo relocations, but the function "
            << symbolName
            << " is missing; it is needed for fixing the relocations at "
               "runtime";
    }

    PseudoRelocTableChunk *table = make<PseudoRelocTableChunk>(rels);
    rdataSec->addChunk(table);
    EmptyChunk *endOfList = make<EmptyChunk>();
    rdataSec->addChunk(endOfList);

    Symbol *headSym = symtab.findUnderscore("__RUNTIME_PSEUDO_RELOC_LIST__");
    Symbol *endSym = symtab.findUnderscore("__RUNTIME_PSEUDO_RELOC_LIST_END__");
    replaceSymbol<DefinedSynthetic>(headSym, headSym->getName(), table);
    replaceSymbol<DefinedSynthetic>(endSym, endSym->getName(), endOfList);
  });
}

// The table a program publishes for the libraries it loads (WeakPublishChunk).
// A library reaches its own weak definition of a name directly and forwards to
// the program's when this table holds it, so a definition the program replaced
// is the one the whole program uses. The set is the image's exports, which is
// where a definition makes itself reachable to another image, and a program
// that replaced nothing exports nothing and publishes nothing. The C runtime
// contributes the bookends of the record section, so an image linked by other
// tools carries no table and no library binds anything to it.
void Writer::createWeakPublishTable() {
  // The startup code that binds the records is what names these, so an image
  // linked by other tools takes no part. Both stay absolute and equal when
  // the image has no records, an empty range.
  Symbol *start = ctx.getOutputSymtab().findUnderscore("__wkintp_start");
  Symbol *end = ctx.getOutputSymtab().findUnderscore("__wkintp_end");
  if (llvm::none_of(ctx.objFileInstances, [&](ObjFile *f) {
        return llvm::is_contained(f->getSymbols(), start);
      }))
    return;

  if (OutputSection *records = findSection(".wkintp")) {
    auto *first = make<EmptyChunk>();
    auto *last = make<EmptyChunk>();
    records->insertChunkAtStart(first);
    records->addChunk(last);
    replaceSymbol<DefinedSynthetic>(start, start->getName(), first);
    replaceSymbol<DefinedSynthetic>(end, end->getName(), last);
  }

  // Only a program interposes, so only a program publishes.
  if (ctx.config.dll)
    return;

  std::vector<WeakPublish> entries;
  for (const Export &e : ctx.getOutputSymtab().exports) {
    // A forwarder names a symbol of another image, which this one cannot
    // publish an address for.
    if (!e.forwardTo.empty())
      continue;
    auto *def = dyn_cast_or_null<Defined>(e.sym);
    if (!def)
      continue;
    // The name the export directory carries, which is the name a weak
    // definition elsewhere in the program was written with.
    XXH128_hash_t hash = xxh3_128bits(arrayRefFromStringRef(e.exportName));
    entries.push_back({hash.low64, hash.high64, def});
  }
  if (entries.empty())
    return;

  wkpubSec->addChunk(make<WeakPublishChunk>(std::move(entries)));
}

// Canonical multi-byte no-ops, by length.
// The sequences the assembler uses, and above ten bytes its rule for reaching
// fifteen: operand-size prefixes in front of the ten-byte form. One of them
// is one instruction however long it is, where a run of shorter ones is one
// apiece, and fifteen is what the tuning for this baseline calls fast.
static void writeNops(uint8_t *buf, uint32_t size) {
  static const uint8_t Nops[10][10] = {
      {0x90},
      {0x66, 0x90},
      {0x0F, 0x1F, 0x00},
      {0x0F, 0x1F, 0x40, 0x00},
      {0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}};
  while (size) {
    uint32_t n = std::min<uint32_t>(size, 15);
    uint32_t prefixes = n > 10 ? n - 10 : 0;
    memset(buf, 0x66, prefixes);
    memcpy(buf + prefixes, Nops[n - prefixes - 1], n - prefixes);
    buf += n;
    size -= n;
  }
}

// An executable is final: nothing it loads supersedes a definition it holds,
// so the entry every weak definition carries in front of its body can never
// take its branch, and an ordinary program pays for it on every allocation.
// Where every one of them is the entry the compiler emits, they are replaced
// with padding. One that is not -- another architecture, another producer --
// leaves all of them alone, because a definition whose entry was removed and
// one whose entry was not would not agree on which allocator a block came
// from.
// Done before the sections are made, because a program that drops every
// entry has no use for the records they read either: nothing reaches them,
// and the startup code returns before it would look. Dropping them takes a
// section, and with it a page of the image, out of every program.
void Writer::dropWeakInterposition() {
  if (ctx.config.dll)
    return;

  std::vector<DeadInterposeEntry> entries;
  std::vector<SectionChunk *> records;
  for (Chunk *c : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(c);
    if (!sc || !sc->live)
      continue;
    if (sc->getSectionName() == ".wkintp") {
      records.push_back(sc);
      continue;
    }
    if (!(sc->getOutputCharacteristics() & IMAGE_SCN_CNT_CODE))
      continue;
    for (const coff_relocation &rel : sc->getRelocs()) {
      auto *sym =
          dyn_cast_or_null<Defined>(sc->file->getSymbol(rel.SymbolTableIndex));
      auto *target =
          sym ? dyn_cast_or_null<SectionChunk>(sym->getChunk()) : nullptr;
      if (!target || target->getSectionName() != ".wkintp")
        continue;
      bool bodyIsBranchTarget = false;
      uint32_t size = sc->getWeakInterposeEntrySize(rel, bodyIsBranchTarget);
      if (!size)
        return;
      entries.push_back(
          {sc, rel.VirtualAddress - 3, size, bodyIsBranchTarget, 0, false});
    }
  }
  if (records.empty())
    return;

  // What follows each entry, as the compiler measured it. Padding an entry
  // out costs nothing where the instruction after it can carry the bytes, and
  // one instruction where it cannot, so this only ever improves the result.
  DenseMap<std::pair<SectionChunk *, uint32_t>, std::array<uint32_t, 3>> after;
  for (ObjFile *file : ctx.objFileInstances) {
    ArrayRef<Symbol *> syms = file->getSymbols();
    for (SectionChunk *c : file->getWeakInterposeNextChunks()) {
      ArrayRef<uint8_t> data = c->getContents();
      if (data.size() % 20)
        continue;
      for (size_t i = 0; i + 20 <= data.size(); i += 20) {
        uint32_t funcIndex = read32le(data.data() + i);
        if (funcIndex >= syms.size())
          continue;
        auto *at = dyn_cast_or_null<DefinedRegular>(syms[funcIndex]);
        auto *chunk = at ? dyn_cast_or_null<SectionChunk>(at->getChunk())
                         : nullptr;
        if (!chunk)
          continue;
        uint32_t off = at->getValue() + read32le(data.data() + i + 4);
        after[{chunk, off}] = {read32le(data.data() + i + 8),
                               read32le(data.data() + i + 12),
                               read32le(data.data() + i + 16)};
      }
    }
  }
  for (DeadInterposeEntry &entry : entries) {
    auto it = after.find({entry.chunk, entry.offset});
    // The distance has to be the entry itself, or something was aligned in
    // between and what follows the entry is padding of an unknown length.
    if (it == after.end() || it->second[0] != entry.size)
      continue;
    entry.nextSize = it->second[1];
    entry.nextTransfers = it->second[2] & 1;
  }
  deadInterposeEntries = std::move(entries);
  for (SectionChunk *sc : records)
    sc->live = false;
}

// The compiler folds a call that reads an import pointer back onto the
// pointer, having undone the lift of that read out of a loop, and records
// where it did so. Where the function really is in another image, reading the
// table once and calling through a register is cheaper than reading it at
// every call, so the fold is undone again here; where the function is in this
// image the call becomes a direct one instead (SectionChunk::applyRelocation)
// and the read that is left costs a single instruction on entry to the loop.
void Writer::collectImportFuseCalls() {
  for (ObjFile *file : ctx.objFileInstances) {
    ArrayRef<Symbol *> syms = file->getSymbols();
    for (SectionChunk *c : file->getImportFuseChunks()) {
      ArrayRef<uint8_t> data = c->getContents();
      if (data.size() % 16) {
        Warn(ctx) << "ignoring malformed " << c->getSectionName()
                  << " in object " << file;
        continue;
      }
      for (size_t i = 0; i + 16 <= data.size(); i += 16) {
        uint32_t funcIndex = read32le(data.data() + i);
        uint32_t impIndex = read32le(data.data() + i + 4);
        uint32_t reg = read32le(data.data() + i + 8);
        uint32_t inFunction = read32le(data.data() + i + 12);
        if (funcIndex >= syms.size() || impIndex >= syms.size() || reg > 15)
          continue;

        // Only a pointer the loader fills is worth reading once; a pointer
        // this image writes is the address of something in it, and the call
        // to it is about to become direct.
        auto *imp = dyn_cast_or_null<DefinedImportData>(syms[impIndex]);
        if (!imp)
          continue;
        // A delay-loaded entry holds the loader's stub until the first call,
        // so a copy of it taken beforehand would go on reaching the stub.
        if (ctx.config.delayLoads.contains(
                StringRef(imp->file->dllName).lower()))
          continue;

        auto *at = dyn_cast_or_null<DefinedRegular>(syms[funcIndex]);
        if (!at)
          continue;
        auto *chunk = dyn_cast_or_null<SectionChunk>(at->getChunk());
        if (!chunk || !chunk->live)
          continue;

        // The bytes have to be the ones the record describes, a call or a
        // jump through the pointer. Anything else and the record does not
        // describe this code, so the call stays as it is, which is correct
        // and is what MSVC emits.
        uint64_t off = uint64_t(at->getValue()) + inFunction;
        ArrayRef<uint8_t> code = chunk->getContents();
        if (off + 6 > code.size() || code[off] != 0xFF)
          continue;
        if (code[off + 1] != 0x15 && code[off + 1] != 0x25)
          continue;
        importFuseCalls.push_back({chunk, static_cast<uint32_t>(off),
                                   static_cast<uint8_t>(reg),
                                   code[off + 1] == 0x25});
      }
    }

    // The read the calls were folded out of. Where every one of them became a
    // direct call nothing reads it, and seven segment overrides put in its
    // place are decoded as part of the instruction that follows, so nothing
    // is left to execute and no address in the image moves.
    for (SectionChunk *c : file->getImportLoadChunks()) {
      ArrayRef<uint8_t> data = c->getContents();
      if (data.size() % 24) {
        Warn(ctx) << "ignoring malformed " << c->getSectionName()
                  << " in object " << file;
        continue;
      }
      for (size_t i = 0; i + 24 <= data.size(); i += 24) {
        uint32_t funcIndex = read32le(data.data() + i);
        uint32_t impIndex = read32le(data.data() + i + 4);
        uint32_t inFunction = read32le(data.data() + i + 8);
        uint32_t toNext = read32le(data.data() + i + 12);
        uint32_t nextLen = read32le(data.data() + i + 16);
        bool intoControlTransfer = read32le(data.data() + i + 20) & 1;
        if (funcIndex >= syms.size() || impIndex >= syms.size())
          continue;

        // A pointer the loader fills is still read, because the call that
        // reads it is still a call through a register.
        if (isa_and_nonnull<DefinedImportData>(syms[impIndex]))
          continue;
        // The overrides and the instruction they belong to are one
        // instruction, and a processor decodes fifteen bytes of it at most.
        // Anything between the read and that instruction is padding whose
        // length nothing here knows. Failing either, the read goes as a
        // no-operation, which needs nothing of what follows it.
        bool couldAbsorb = toNext == 7 && nextLen && nextLen <= 8;

        auto *at = dyn_cast_or_null<DefinedRegular>(syms[funcIndex]);
        if (!at)
          continue;
        auto *chunk = dyn_cast_or_null<SectionChunk>(at->getChunk());
        if (!chunk || !chunk->live)
          continue;

        // mov r64, [rip + disp32]: REX.W with or without REX.R, then the
        // opcode, then ModRM with mod=00 rm=101.
        uint64_t off = uint64_t(at->getValue()) + inFunction;
        ArrayRef<uint8_t> code = chunk->getContents();
        if (off + 7 > code.size() || (code[off] & 0xFB) != 0x48 ||
            code[off + 1] != 0x8B || (code[off + 2] & 0xC7) != 0x05)
          continue;
        deadBytes.push_back({chunk, static_cast<uint32_t>(off), 7,
                             couldAbsorb ? nextLen : 0, intoControlTransfer});
      }
    }
  }

}

// Done once the chunks are written, because the relocation the call carries
// has been applied over these bytes by then.
void Writer::unfuseImportCalls(uint8_t *buf) {
  for (const ImportFuseCall &call : importFuseCalls) {
    OutputSection *os = ctx.getOutputSection(call.chunk);
    if (!os)
      continue;
    uint8_t *at = buf + os->getFileOff() + call.chunk->getRVA() - os->getRVA() +
                  call.offset;

    // Six bytes either way, so the call ends where it ended and the address
    // it returns to, the entry the unwinder holds for it and every other
    // address in the image are the ones they were. What is spare in front is
    // segment overrides, which this processor ignores and which cost nothing
    // to decode; landing on one of them decodes the same instruction as
    // landing on the first.
    uint8_t reg = call.reg;
    uint8_t modrm = 0xC0 | ((call.isJump ? 4 : 2) << 3) | (reg & 7);
    if (reg & 8) {
      memset(at, 0x2E, 3);
      at[3] = 0x41; // REX.B
      at[4] = 0xFF;
      at[5] = modrm;
    } else {
      memset(at, 0x2E, 4);
      at[4] = 0xFF;
      at[5] = modrm;
    }
  }

}

// Bytes nothing executes are made to cost as little as a processor allows:
// nothing, where the instruction after them can take them as prefixes, and
// one instruction otherwise. Done once the chunks are written, because the
// relocations over these bytes have been applied by then.
void Writer::disposeOfDeadBytes(uint8_t *buf) {
  llvm::sort(deadBytes, [](const DeadBytes &a, const DeadBytes &b) {
    return std::tie(a.chunk, a.offset) < std::tie(b.chunk, b.offset);
  });

  for (size_t i = 0; i != deadBytes.size(); ++i) {
    const DeadBytes &run = deadBytes[i];
    OutputSection *os = ctx.getOutputSection(run.chunk);
    if (!os)
      continue;
    uint8_t *at = buf + os->getFileOff() + run.chunk->getRVA() - os->getRVA() +
                  run.offset;

    // A run whose own successor is another dead run would put its prefixes in
    // front of whatever that one leaves, and two of them stacked can pass the
    // fifteen bytes a processor decodes. The first takes the no-operation.
    bool runFollows = i + 1 != deadBytes.size() &&
                      deadBytes[i + 1].chunk == run.chunk &&
                      deadBytes[i + 1].offset == run.offset + run.size;

    // Prefixes and the instruction they belong to are one instruction, and a
    // processor decodes fifteen bytes of it. They leave it ending where it
    // ended and beginning where the run began, so a transfer of control can
    // only be carried across a thirty-two byte boundary it was not already
    // crossing, and only when the run itself straddles one.
    uint64_t rva = run.chunk->getRVA() + run.offset;
    if (!runFollows && run.nextSize && run.size + run.nextSize <= 15 &&
        (!run.nextTransfers || (rva & 31) + run.size <= 31))
      memset(at, 0x2E, run.size);
    else
      writeNops(at, run.size);
  }
}

// Done once the chunks are written, because the relocation inside the entry
// has been applied over the bytes by then.
void Writer::padWeakInterposition(uint8_t *buf) {
  for (auto &entry : deadInterposeEntries) {
    OutputSection *os = ctx.getOutputSection(entry.chunk);
    if (!os)
      continue;
    uint8_t *at = buf + os->getFileOff() + entry.chunk->getRVA() -
                  os->getRVA() + entry.offset;
    if (!entry.bodyIsBranchTarget) {
      // The body falls through, so the whole entry is bytes nothing runs, and
      // the instruction after it is the body's first.
      deadBytes.push_back({entry.chunk, entry.offset, entry.size,
                           entry.nextSize, entry.nextTransfers});
      continue;
    }
    // The body is the branch's target, so the branch stays and becomes
    // unconditional. An unconditional jump that ends where the conditional
    // one ended takes the same displacement, which leaves the relocated
    // field where it was written, and what precedes it is the dead run: the
    // jump can carry it, since prefixes leave it ending where it ends.
    uint32_t jump = entry.size == 12 ? 2 : 5;
    at[entry.size - jump] = entry.size == 12 ? 0xEB : 0xE9;
    deadBytes.push_back(
        {entry.chunk, entry.offset, entry.size - jump, jump, true});
  }
}

// MinGW specific.
// The MinGW .ctors and .dtors lists have sentinels at each end;
// a (uintptr_t)-1 at the start and a (uintptr_t)0 at the end.
// There's a symbol pointing to the start sentinel pointer, __CTOR_LIST__
// and __DTOR_LIST__ respectively.
void Writer::insertCtorDtorSymbols() {
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    AbsolutePointerChunk *ctorListHead = make<AbsolutePointerChunk>(symtab, -1);
    AbsolutePointerChunk *ctorListEnd = make<AbsolutePointerChunk>(symtab, 0);
    AbsolutePointerChunk *dtorListHead = make<AbsolutePointerChunk>(symtab, -1);
    AbsolutePointerChunk *dtorListEnd = make<AbsolutePointerChunk>(symtab, 0);
    ctorsSec->insertChunkAtStart(ctorListHead);
    ctorsSec->addChunk(ctorListEnd);
    dtorsSec->insertChunkAtStart(dtorListHead);
    dtorsSec->addChunk(dtorListEnd);

    Symbol *ctorListSym = symtab.findUnderscore("__CTOR_LIST__");
    Symbol *dtorListSym = symtab.findUnderscore("__DTOR_LIST__");
    replaceSymbol<DefinedSynthetic>(ctorListSym, ctorListSym->getName(),
                                    ctorListHead);
    replaceSymbol<DefinedSynthetic>(dtorListSym, dtorListSym->getName(),
                                    dtorListHead);
  });

  if (ctx.hybridSymtab) {
    ctorsSec->splitECChunks();
    dtorsSec->splitECChunks();
  }
}

// MinGW (really, Cygwin) specific.
// The Cygwin startup code uses __data_start__ __data_end__ __bss_start__
// and __bss_end__ to know what to copy during fork emulation.
void Writer::insertBssDataStartEndSymbols() {
  if (!dataSec->chunks.empty()) {
    Symbol *dataStartSym = ctx.getOutputSymtab().find("__data_start__");
    Symbol *dataEndSym = ctx.getOutputSymtab().find("__data_end__");
    Chunk *endChunk = dataSec->chunks.back();
    replaceSymbol<DefinedSynthetic>(dataStartSym, dataStartSym->getName(),
                                    dataSec->chunks.front());
    replaceSymbol<DefinedSynthetic>(dataEndSym, dataEndSym->getName(), endChunk,
                                    endChunk->getSize());
  }

  if (!bssSec->chunks.empty()) {
    Symbol *bssStartSym = ctx.getOutputSymtab().find("__bss_start__");
    Symbol *bssEndSym = ctx.getOutputSymtab().find("__bss_end__");
    Chunk *endChunk = bssSec->chunks.back();
    replaceSymbol<DefinedSynthetic>(bssStartSym, bssStartSym->getName(),
                                    bssSec->chunks.front());
    replaceSymbol<DefinedSynthetic>(bssEndSym, bssEndSym->getName(), endChunk,
                                    endChunk->getSize());
  }
}

// Handles /section options to allow users to overwrite
// section attributes.
uint64_t Writer::getIATSize() const {
  if (!iatStart)
    return 0;
  uint64_t end = iatEnd->getRVA() + iatEnd->getSize();
  if (end < iatStart->getRVA())
    Fatal(ctx) << "import address table has an invalid final range";
  uint64_t size = end - iatStart->getRVA();
  return isArm64EC(ctx.config.machine) ? alignTo(size, 0x1000) : size;
}

void Writer::validateImportStorage() {
  if (errorCount() || importSlots.empty())
    return;
  constexpr uint32_t permissions =
      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;
  uint64_t begin = iatStart ? iatStart->getRVA() : 0;
  uint64_t size = getIATSize();
  uint64_t end = begin + size;
  if (size > UINT32_MAX || end > uint64_t(UINT32_MAX) + 1)
    Fatal(ctx) << "import address table exceeds the PE directory range";

  // Windows restores one saved protection over the page-rounded IAT span.
  // Validate actual output sections, including /merge and /section changes,
  // rather than assuming the original read-only input flags still apply.
  if (iatStart) {
    const OutputSection *first = ctx.getOutputSection(iatStart);
    uint32_t protection = first->header.Characteristics & permissions;
    uint64_t pageBegin = alignDown(begin, pageSize);
    uint64_t pageEnd = alignTo(end, pageSize);
    for (OutputSection *os : ctx.outputSections) {
      if (os->getRVA() >= pageEnd ||
          os->getRVA() + os->getVirtualSize() <= pageBegin)
        continue;
      if ((os->header.Characteristics & permissions) != protection)
        Fatal(ctx) << "import address table protection span crosses section "
                   << os->name << " with incompatible permissions";
    }
  }

  size_t index = 0;
  for (const IdataContents::SlotRun &run : idata.slotRuns) {
    uint64_t next = run.chunk->getRVA() + run.offset;
    for (DefinedImportData *sym : run.syms) {
      if (index == importSlots.size())
        Fatal(ctx) << "import destination run exceeds its recorded slots";
      const ImportSlot &slot = importSlots[index++];
      uint64_t rva = slot.chunk->getRVA() + slot.offset;
      if (slot.sym != sym || rva != next)
        Fatal(ctx) << "import destination run for " << sym->getDLLName()
                   << " is not contiguous after final layout";
      next += ctx.config.wordsize;
      const OutputSection *os = ctx.getOutputSection(slot.chunk);
      uint32_t protection = os->header.Characteristics & permissions;
      if (protection & IMAGE_SCN_MEM_EXECUTE)
        Fatal(ctx) << toString(slot.chunk->file)
                   << ": native data import destination is executable in "
                   << os->name;
      if (iatSlotChunks.contains(slot.chunk) &&
          protection != IMAGE_SCN_MEM_READ)
        Fatal(ctx) << toString(slot.chunk->file)
                   << ": immutable import storage must remain read-only in "
                   << os->name;
      if (!(protection & IMAGE_SCN_MEM_WRITE) &&
          (!iatStart || rva < begin || next > end))
        Fatal(ctx) << toString(slot.chunk->file)
                   << ": read-only import destination is outside the final "
                      "import address table span";
    }
  }
  if (index != importSlots.size())
    Fatal(ctx) << "import destination slots are missing from final runs";
}

void Writer::validateRTTIBindings() {
  SmallPtrSet<Chunk *, 32> descriptors;
  SmallPtrSet<ImportFile *, 32> canonicalImports;
  constexpr uint32_t permissions =
      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;
  for (ObjFile *file : ctx.objFileInstances) {
    for (const auto &binding : file->getBindingRequirements()) {
      Symbol *symbol = binding.symbol;
      if (ctx.outputPartition) {
        auto it = ctx.outputPartition->projections.find(symbol);
        if (it != ctx.outputPartition->projections.end())
          symbol = it->second;
      }
      Defined *def = symbol->getDefined();
      if (auto *imp = dyn_cast_or_null<DefinedImportData>(def)) {
        if (binding.flags & COFF::BindingCanonical)
          canonicalImports.insert(imp->file);
        continue;
      }
      auto *regular = dyn_cast_or_null<DefinedRegular>(def);
      if (!regular || !regular->getChunk()->live)
        continue;
      Chunk *chunk = regular->getChunk();
      if (binding.flags & COFF::BindingRTTI)
        descriptors.insert(chunk);
      const OutputSection *os = ctx.getOutputSection(chunk);
      if (!os ||
          (os->header.Characteristics & permissions) != IMAGE_SCN_MEM_READ)
        Fatal(ctx) << toString(file) << ": RTTI binding " << def->getName()
                   << " must reside in read-only, non-executable output";
    }
  }
  for (const ImportSlot &slot : filledSlots)
    if (descriptors.contains(slot.chunk) ||
        canonicalImports.contains(slot.sym->file))
      Fatal(ctx) << toString(slot.chunk->file) << ": RTTI binding of "
                 << slot.sym->getName() << " with addend " << slot.addend
                 << " requires an exact native export; runtime initialization "
                    "is forbidden";

  // Check the final graph, including alias copies, after import classification.
  // /runtime-pseudo-reloc is not an escape from canonical RTTI binding.
  if (canonicalImports.empty())
    return;
  for (Chunk *chunk : ctx.driver.getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(chunk);
    if (!sc || !sc->live || sc->isDWARF() || sc->isCodeView())
      continue;
    for (const coff_relocation &rel : sc->getRelocs()) {
      auto *imp = dyn_cast_or_null<DefinedImportData>(
          sc->file->getSymbol(rel.SymbolTableIndex));
      if (imp && imp->isRuntimePseudoReloc &&
          canonicalImports.contains(imp->file) && !sc->getImportSlot(rel) &&
          !sc->isImportAddressLoad(rel))
        Fatal(ctx) << toString(sc->file)
                   << ": unredirectable canonical reference to "
                   << imp->getName() << " in " << sc->getSectionName()
                   << " at offset " << rel.VirtualAddress;
    }
  }
}

void Writer::setSectionPermissions() {
  llvm::TimeTraceScope timeScope("Sections permissions");
  for (auto &p : ctx.config.section) {
    StringRef name = p.first;
    uint32_t perm = p.second;
    for (OutputSection *sec : ctx.outputSections)
      if (sec->name == name)
        sec->setPermissions(perm);
  }
}

// Set symbols used by ARM64EC metadata.
void Writer::setECSymbols() {
  if (!ctx.getOutputSymtab().isEC())
    return;

  llvm::stable_sort(exportThunks, [](const std::pair<Chunk *, Defined *> &a,
                                     const std::pair<Chunk *, Defined *> &b) {
    return a.first->getRVA() < b.first->getRVA();
  });

  ChunkRange &chpePdata = ctx.config.machine == ARM64X ? hybridPdata : pdata;
  Symbol *rfeTableSym = ctx.getOutputSymtab().findUnderscore("__arm64x_extra_rfe_table");
  replaceSymbol<DefinedSynthetic>(rfeTableSym, "__arm64x_extra_rfe_table",
                                  chpePdata.first);

  if (chpePdata.first) {
    Symbol *rfeSizeSym =
        ctx.getOutputSymtab().findUnderscore("__arm64x_extra_rfe_table_size");
    cast<DefinedAbsolute>(rfeSizeSym)
        ->setVA(chpePdata.last->getRVA() + chpePdata.last->getSize() -
                chpePdata.first->getRVA());
  }

  Symbol *rangesCountSym =
      ctx.getOutputSymtab().findUnderscore("__x64_code_ranges_to_entry_points_count");
  cast<DefinedAbsolute>(rangesCountSym)->setVA(exportThunks.size());

  Symbol *entryPointCountSym =
      ctx.getOutputSymtab().findUnderscore("__arm64x_redirection_metadata_count");
  cast<DefinedAbsolute>(entryPointCountSym)->setVA(exportThunks.size());

  Symbol *iatSym = ctx.getOutputSymtab().findUnderscore("__hybrid_auxiliary_iat");
  replaceSymbol<DefinedSynthetic>(iatSym, "__hybrid_auxiliary_iat",
                                  idata.auxIat.empty() ? nullptr
                                                       : idata.auxIat.front());

  Symbol *iatCopySym = ctx.getOutputSymtab().findUnderscore("__hybrid_auxiliary_iat_copy");
  replaceSymbol<DefinedSynthetic>(
      iatCopySym, "__hybrid_auxiliary_iat_copy",
      idata.auxIatCopy.empty() ? nullptr : idata.auxIatCopy.front());

  Symbol *delayIatSym =
      ctx.getOutputSymtab().findUnderscore("__hybrid_auxiliary_delayload_iat");
  replaceSymbol<DefinedSynthetic>(
      delayIatSym, "__hybrid_auxiliary_delayload_iat",
      delayIdata.getAuxIat().empty() ? nullptr
                                     : delayIdata.getAuxIat().front());

  Symbol *delayIatCopySym =
      ctx.getOutputSymtab().findUnderscore("__hybrid_auxiliary_delayload_iat_copy");
  replaceSymbol<DefinedSynthetic>(
      delayIatCopySym, "__hybrid_auxiliary_delayload_iat_copy",
      delayIdata.getAuxIatCopy().empty() ? nullptr
                                         : delayIdata.getAuxIatCopy().front());

  if (ctx.config.machine == ARM64X) {
    // For the hybrid image, set the alternate entry point to the EC entry
    // point. In the hybrid view, it is swapped to the native entry point
    // using ARM64X relocations.
    if (auto altEntrySym = cast_or_null<Defined>(ctx.getOutputSymtab().entry)) {
      // If the entry is an EC export thunk, use its target instead.
      if (auto thunkChunk =
              dyn_cast<ECExportThunkChunk>(altEntrySym->getChunk()))
        altEntrySym = thunkChunk->target;
      ctx.getOutputSymtab().findUnderscore("__arm64x_native_entrypoint")
          ->replaceKeepingName(altEntrySym, sizeof(SymbolUnion));
    }

    if (ctx.getOutputSymtab().edataStart)
      ctx.dynamicRelocs->set(
          dataDirOffset64 + EXPORT_TABLE * sizeof(data_directory) +
              offsetof(data_directory, Size),
          ctx.getOutputSymtab().edataEnd->getRVA() - ctx.getOutputSymtab().edataStart->getRVA() +
              ctx.getOutputSymtab().edataEnd->getSize());
    if (hybridPdata.first)
      ctx.dynamicRelocs->set(
          dataDirOffset64 + EXCEPTION_TABLE * sizeof(data_directory) +
              offsetof(data_directory, Size),
          hybridPdata.last->getRVA() - hybridPdata.first->getRVA() +
              hybridPdata.last->getSize());
    if (chpeSym && pdata.first)
      ctx.dynamicRelocs->set(
          chpeSym->getRVA() + offsetof(chpe_metadata, ExtraRFETableSize),
          pdata.last->getRVA() + pdata.last->getSize() - pdata.first->getRVA());
  }

  for (SameAddressThunkARM64EC *thunk : ctx.getOutputSymtab().sameAddressThunks)
    thunk->setDynamicRelocs(ctx);
}

// Write section contents to a mmap'ed file.
void Writer::writeSections() {
  llvm::TimeTraceScope timeScope("Write sections");
  uint8_t *buf = buffer->getBufferStart();
  for (OutputSection *sec : ctx.outputSections) {
    uint8_t *secBuf = buf + sec->getFileOff();
    // Fill gaps between functions in .text with INT3 instructions
    // instead of leaving as NUL bytes (which can be interpreted as
    // ADD instructions). Only fill the gaps between chunks. Most
    // chunks overwrite it anyway, but uninitialized data chunks
    // merged into a code section don't.
    if ((sec->header.Characteristics & IMAGE_SCN_CNT_CODE) &&
        (ctx.config.machine == AMD64 || ctx.config.machine == I386)) {
      uint32_t prevEnd = 0;
      uint32_t rawSize = sec->getRawSize();
      for (Chunk *c : sec->chunks) {
        uint32_t off = c->getRVA() - sec->getRVA();
        // Chunks without data (e.g., .bss) have virtual addresses beyond
        // rawSize; stop filling when we reach the end of raw data.
        if (off >= rawSize)
          break;
        memset(secBuf + prevEnd, 0xCC, off - prevEnd);
        prevEnd = std::min(off + static_cast<uint32_t>(c->getSize()), rawSize);
      }
      memset(secBuf + prevEnd, 0xCC, rawSize - prevEnd);
    }

    parallelForEach(sec->chunks, [&](Chunk *c) {
      uint8_t *buf = secBuf + c->getRVA() - sec->getRVA();
      c->writeTo(buf);

      // Write the offset to EC entry thunk preceding section contents. The low
      // bit is always set, so it's effectively an offset from the last byte of
      // the offset.
      if (Defined *entryThunk = c->getEntryThunk())
        write32le(buf - sizeof(uint32_t),
                  entryThunk->getRVA() - c->getRVA() + 1);
    });
  }

  padWeakInterposition(buf);
  unfuseImportCalls(buf);
  disposeOfDeadBytes(buf);
}

void Writer::writeBuildId() {
  llvm::TimeTraceScope timeScope("Write build ID");

  // There are two important parts to the build ID.
  // 1) If building with debug info, the COFF debug directory contains a
  //    timestamp as well as a Guid and Age of the PDB.
  // 2) In all cases, the PE COFF file header also contains a timestamp.
  // For reproducibility, instead of a timestamp we want to use a hash of the
  // PE contents.
  Configuration *config = &ctx.config;
  bool generateSyntheticBuildId = config->buildIDHash == BuildIDHash::Binary;
  if (generateSyntheticBuildId) {
    assert(buildId && "BuildId is not set!");
    // BuildId->BuildId was filled in when the PDB was written.
  }

  // At this point the only fields in the COFF file which remain unset are the
  // "timestamp" in the COFF file header, and the ones in the coff debug
  // directory.  Now we can hash the file and write that hash to the various
  // timestamp fields in the file.
  StringRef outputFileData(
      reinterpret_cast<const char *>(buffer->getBufferStart()),
      buffer->getBufferSize());

  uint32_t timestamp = config->timestamp;
  uint64_t hash = 0;

  if (config->repro || generateSyntheticBuildId)
    hash = xxh3_64bits(outputFileData);

  if (config->repro)
    timestamp = static_cast<uint32_t>(hash);

  if (generateSyntheticBuildId) {
    buildId->buildId->PDB70.CVSignature = OMF::Signature::PDB70;
    buildId->buildId->PDB70.Age = 1;
    memcpy(buildId->buildId->PDB70.Signature, &hash, 8);
    // xxhash only gives us 8 bytes, so put some fixed data in the other half.
    memcpy(&buildId->buildId->PDB70.Signature[8], "LLD PDB.", 8);
  }

  if (debugDirectory)
    debugDirectory->setTimeDateStamp(timestamp);

  uint8_t *buf = buffer->getBufferStart();
  buf += dosStubSize + sizeof(PEMagic);
  object::coff_file_header *coffHeader =
      reinterpret_cast<coff_file_header *>(buf);
  coffHeader->TimeDateStamp = timestamp;
}

// Sort .pdata section contents according to PE/COFF spec 5.5.
template <typename T>
void Writer::sortExceptionTable(ChunkRange &exceptionTable) {
  if (!exceptionTable.first)
    return;

  // We assume .pdata contains function table entries only.
  auto bufAddr = [&](Chunk *c) {
    OutputSection *os = ctx.getOutputSection(c);
    return buffer->getBufferStart() + os->getFileOff() + c->getRVA() -
           os->getRVA();
  };
  uint8_t *begin = bufAddr(exceptionTable.first);
  uint8_t *end = bufAddr(exceptionTable.last) + exceptionTable.last->getSize();
  if ((end - begin) % sizeof(T) != 0) {
    Fatal(ctx) << "unexpected .pdata size: " << (end - begin)
               << " is not a multiple of " << sizeof(T);
  }

  parallelSort(MutableArrayRef<T>(reinterpret_cast<T *>(begin),
                                  reinterpret_cast<T *>(end)),
               [](const T &a, const T &b) { return a.begin < b.begin; });
}

// Sort .pdata section contents according to PE/COFF spec 5.5.
void Writer::sortExceptionTables() {
  llvm::TimeTraceScope timeScope("Sort exception table");

  struct EntryX64 {
    ulittle32_t begin, end, unwind;
  };
  struct EntryArm {
    ulittle32_t begin, unwind;
  };

  switch (ctx.config.machine) {
  case AMD64:
    sortExceptionTable<EntryX64>(pdata);
    break;
  case ARM64EC:
  case ARM64X:
    sortExceptionTable<EntryX64>(hybridPdata);
    [[fallthrough]];
  case ARMNT:
  case ARM64:
    sortExceptionTable<EntryArm>(pdata);
    break;
  default:
    if (pdata.first)
      ctx.e.errs() << "warning: don't know how to handle .pdata\n";
    break;
  }
}

// The CRT section contains, among other things, the array of function
// pointers that initialize every global variable that is not trivially
// constructed. The CRT calls them one after the other prior to invoking
// main().
//
// As per C++ spec, 3.6.2/2.3,
// "Variables with ordered initialization defined within a single
// translation unit shall be initialized in the order of their definitions
// in the translation unit"
//
// It is therefore critical to sort the chunks containing the function
// pointers in the order that they are listed in the object file (top to
// bottom), otherwise global objects might not be initialized in the
// correct order.
void Writer::sortCRTSectionChunks(std::vector<Chunk *> &chunks) {
  auto sectionChunkOrder = [](const Chunk *a, const Chunk *b) {
    auto sa = dyn_cast<SectionChunk>(a);
    auto sb = dyn_cast<SectionChunk>(b);
    assert(sa && sb && "Non-section chunks in CRT section!");

    StringRef sAObj = sa->file->mb.getBufferIdentifier();
    StringRef sBObj = sb->file->mb.getBufferIdentifier();

    return sAObj == sBObj && sa->getSectionNumber() < sb->getSectionNumber();
  };
  llvm::stable_sort(chunks, sectionChunkOrder);

  if (ctx.config.verbose) {
    for (auto &c : chunks) {
      auto sc = dyn_cast<SectionChunk>(c);
      Log(ctx) << "  " << sc->file->mb.getBufferIdentifier().str()
               << ", SectionID: " << sc->getSectionNumber();
    }
  }
}

OutputSection *Writer::findSection(StringRef name) {
  for (OutputSection *sec : ctx.outputSections)
    if (sec->name == name)
      return sec;
  return nullptr;
}

uint32_t Writer::getSizeOfInitializedData() {
  uint32_t res = 0;
  for (OutputSection *s : ctx.outputSections)
    if (s->header.Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)
      res += s->getRawSize();
  return res;
}

// Add base relocations to .reloc section.
void Writer::addBaserels() {
  if (!ctx.config.relocatable)
    return;
  std::vector<Baserel> v;
  for (OutputSection *sec : ctx.outputSections) {
    if (sec->header.Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
      continue;
    llvm::TimeTraceScope timeScope("Base relocations: ", sec->name);
    // Collect all locations for base relocations.
    for (Chunk *c : sec->chunks)
      c->getBaserels(&v);
    // Add the addresses to .reloc section.
    if (!v.empty())
      addBaserelBlocks(v);
    v.clear();
  }
}

// Add addresses to .reloc section. Note that addresses are grouped by page.
void Writer::addBaserelBlocks(std::vector<Baserel> &v) {
  const uint32_t mask = ~uint32_t(pageSize - 1);
  uint32_t page = v[0].rva & mask;
  size_t i = 0, j = 1;
  llvm::sort(v,
             [](const Baserel &x, const Baserel &y) { return x.rva < y.rva; });
  for (size_t e = v.size(); j < e; ++j) {
    uint32_t p = v[j].rva & mask;
    if (p == page)
      continue;
    relocSec->addChunk(make<BaserelChunk>(page, &v[i], &v[0] + j));
    i = j;
    page = p;
  }
  if (i == j)
    return;
  relocSec->addChunk(make<BaserelChunk>(page, &v[i], &v[0] + j));
}

void Writer::createDynamicRelocs() {
  if (!ctx.dynamicRelocs)
    return;

  // Adjust the Machine field in the COFF header to AMD64.
  ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint16_t),
                         coffHeaderOffset + offsetof(coff_file_header, Machine),
                         AMD64);

  if (ctx.getOutputSymtab().entry != ctx.hybridSymtab->entry ||
      pdata.first != hybridPdata.first) {
    chpeSym = cast_or_null<DefinedRegular>(
        ctx.getOutputSymtab().findUnderscore("__chpe_metadata"));
    if (!chpeSym)
      Warn(ctx) << "'__chpe_metadata' is missing for ARM64X target";
  }

  if (ctx.getOutputSymtab().entry != ctx.hybridSymtab->entry) {
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           peHeaderOffset +
                               offsetof(pe32plus_header, AddressOfEntryPoint),
                           cast_or_null<Defined>(ctx.getOutputSymtab().entry));

    // Swap the alternate entry point in the CHPE metadata.
    if (chpeSym)
      ctx.dynamicRelocs->add(
          IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
          Arm64XRelocVal(chpeSym, offsetof(chpe_metadata, AlternateEntryPoint)),
          cast_or_null<Defined>(ctx.hybridSymtab->entry));
  }

  if (ctx.getOutputSymtab().edataStart != ctx.hybridSymtab->edataStart) {
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           dataDirOffset64 +
                               EXPORT_TABLE * sizeof(data_directory) +
                               offsetof(data_directory, RelativeVirtualAddress),
                           ctx.getOutputSymtab().edataStart);
    // The Size value is assigned after addresses are finalized.
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           dataDirOffset64 +
                               EXPORT_TABLE * sizeof(data_directory) +
                               offsetof(data_directory, Size));
  }

  if (pdata.first != hybridPdata.first) {
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           dataDirOffset64 +
                               EXCEPTION_TABLE * sizeof(data_directory) +
                               offsetof(data_directory, RelativeVirtualAddress),
                           hybridPdata.first);
    // The Size value is assigned after addresses are finalized.
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           dataDirOffset64 +
                               EXCEPTION_TABLE * sizeof(data_directory) +
                               offsetof(data_directory, Size));

    // Swap ExtraRFETable in the CHPE metadata.
    if (chpeSym) {
      ctx.dynamicRelocs->add(
          IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
          Arm64XRelocVal(chpeSym, offsetof(chpe_metadata, ExtraRFETable)),
          pdata.first);
      // The Size value is assigned after addresses are finalized.
      ctx.dynamicRelocs->add(
          IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
          Arm64XRelocVal(chpeSym, offsetof(chpe_metadata, ExtraRFETableSize)));
    }
  }

  // Set the hybrid load config to the EC load config.
  ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                         dataDirOffset64 +
                             LOAD_CONFIG_TABLE * sizeof(data_directory) +
                             offsetof(data_directory, RelativeVirtualAddress),
                         ctx.getOutputSymtab().loadConfigSym);
  ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                         dataDirOffset64 +
                             LOAD_CONFIG_TABLE * sizeof(data_directory) +
                             offsetof(data_directory, Size),
                         ctx.getOutputSymtab().loadConfigSize);
}

PartialSection *Writer::createPartialSection(StringRef name,
                                             uint32_t outChars) {
  PartialSection *&pSec = partialSections[{name, outChars}];
  if (pSec)
    return pSec;
  pSec = make<PartialSection>(name, outChars);
  return pSec;
}

PartialSection *Writer::findPartialSection(StringRef name, uint32_t outChars) {
  auto it = partialSections.find({name, outChars});
  if (it != partialSections.end())
    return it->second;
  return nullptr;
}

void Writer::fixTlsAlignment() {
  Defined *tlsSym =
      dyn_cast_or_null<Defined>(ctx.getOutputSymtab().findUnderscore("_tls_used"));
  if (!tlsSym)
    return;

  OutputSection *sec = ctx.getOutputSection(tlsSym->getChunk());
  assert(sec && tlsSym->getRVA() >= sec->getRVA() &&
         "no output section for _tls_used");

  uint8_t *secBuf = buffer->getBufferStart() + sec->getFileOff();
  uint64_t tlsOffset = tlsSym->getRVA() - sec->getRVA();
  uint64_t directorySize = ctx.config.is64()
                               ? sizeof(object::coff_tls_directory64)
                               : sizeof(object::coff_tls_directory32);

  if (tlsOffset + directorySize > sec->getRawSize())
    Fatal(ctx) << "_tls_used sym is malformed";

  if (ctx.config.is64()) {
    object::coff_tls_directory64 *tlsDir =
        reinterpret_cast<object::coff_tls_directory64 *>(&secBuf[tlsOffset]);
    tlsDir->setAlignment(tlsAlignment);
  } else {
    object::coff_tls_directory32 *tlsDir =
        reinterpret_cast<object::coff_tls_directory32 *>(&secBuf[tlsOffset]);
    tlsDir->setAlignment(tlsAlignment);
  }
}

void Writer::prepareLoadConfig() {
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    if (!symtab.loadConfigSym)
      return;

    OutputSection *sec = ctx.getOutputSection(symtab.loadConfigSym->getChunk());
    uint8_t *secBuf = buffer->getBufferStart() + sec->getFileOff();
    uint8_t *symBuf = secBuf + (symtab.loadConfigSym->getRVA() - sec->getRVA());

    if (ctx.config.is64())
      prepareLoadConfig(symtab,
                        reinterpret_cast<coff_load_configuration64 *>(symBuf));
    else
      prepareLoadConfig(symtab,
                        reinterpret_cast<coff_load_configuration32 *>(symBuf));
  });
}

template <typename T>
void Writer::prepareLoadConfig(SymbolTable &symtab, T *loadConfig) {
  size_t loadConfigSize = loadConfig->Size;

#define RETURN_IF_NOT_CONTAINS(field)                                          \
  if (loadConfigSize < offsetof(T, field) + sizeof(T::field)) {                \
    Warn(ctx) << "'_load_config_used' structure too small to include " #field; \
    return;                                                                    \
  }

#define IF_CONTAINS(field)                                                     \
  if (loadConfigSize >= offsetof(T, field) + sizeof(T::field))

#define CHECK_VA(field, sym)                                                   \
  if (auto *s = dyn_cast<DefinedSynthetic>(symtab.findUnderscore(sym)))        \
    if (loadConfig->field != ctx.config.imageBase + s->getRVA())               \
      Warn(ctx) << #field " not set correctly in '_load_config_used'";

#define CHECK_ABSOLUTE(field, sym)                                             \
  if (auto *s = dyn_cast<DefinedAbsolute>(symtab.findUnderscore(sym)))         \
    if (loadConfig->field != s->getVA())                                       \
      Warn(ctx) << #field " not set correctly in '_load_config_used'";

  if (ctx.config.dependentLoadFlags) {
    RETURN_IF_NOT_CONTAINS(DependentLoadFlags)
    loadConfig->DependentLoadFlags = ctx.config.dependentLoadFlags;
  }

  if (ctx.dynamicRelocs) {
    IF_CONTAINS(DynamicValueRelocTableSection) {
      loadConfig->DynamicValueRelocTableSection = relocSec->sectionIndex;
      loadConfig->DynamicValueRelocTableOffset =
          ctx.dynamicRelocs->getRVA() - relocSec->getRVA();
    }
    else {
      Warn(ctx) << "'_load_config_used' structure too small to include dynamic "
                   "relocations";
    }
  }

  IF_CONTAINS(CHPEMetadataPointer) {
    // On ARM64X, only the EC version of the load config contains
    // CHPEMetadataPointer. Copy its value to the native load config.
    if (ctx.config.machine == ARM64X && !symtab.isEC() &&
        ctx.getOutputSymtab().loadConfigSize >=
            offsetof(T, CHPEMetadataPointer) + sizeof(T::CHPEMetadataPointer)) {
      OutputSection *sec =
          ctx.getOutputSection(ctx.getOutputSymtab().loadConfigSym->getChunk());
      uint8_t *secBuf = buffer->getBufferStart() + sec->getFileOff();
      auto hybridLoadConfig =
          reinterpret_cast<const coff_load_configuration64 *>(
              secBuf + (ctx.getOutputSymtab().loadConfigSym->getRVA() - sec->getRVA()));
      loadConfig->CHPEMetadataPointer = hybridLoadConfig->CHPEMetadataPointer;
    }
  }

  if (ctx.config.guardCF == GuardCFLevel::Off)
    return;
  RETURN_IF_NOT_CONTAINS(GuardFlags)
  CHECK_VA(GuardCFFunctionTable, "__guard_fids_table")
  CHECK_ABSOLUTE(GuardCFFunctionCount, "__guard_fids_count")
  CHECK_ABSOLUTE(GuardFlags, "__guard_flags")
  IF_CONTAINS(GuardAddressTakenIatEntryCount) {
    CHECK_VA(GuardAddressTakenIatEntryTable, "__guard_iat_table")
    CHECK_ABSOLUTE(GuardAddressTakenIatEntryCount, "__guard_iat_count")
  }

  if (!(ctx.config.guardCF & GuardCFLevel::LongJmp))
    return;
  RETURN_IF_NOT_CONTAINS(GuardLongJumpTargetCount)
  CHECK_VA(GuardLongJumpTargetTable, "__guard_longjmp_table")
  CHECK_ABSOLUTE(GuardLongJumpTargetCount, "__guard_longjmp_count")

  if (!(ctx.config.guardCF & GuardCFLevel::EHCont))
    return;
  RETURN_IF_NOT_CONTAINS(GuardEHContinuationCount)
  CHECK_VA(GuardEHContinuationTable, "__guard_eh_cont_table")
  CHECK_ABSOLUTE(GuardEHContinuationCount, "__guard_eh_cont_count")

#undef RETURN_IF_NOT_CONTAINS
#undef IF_CONTAINS
#undef CHECK_VA
#undef CHECK_ABSOLUTE
}

void Writer::printSummary() {
  if (!ctx.config.showSummary)
    return;

  SmallString<256> buffer;
  raw_svector_ostream stream(buffer);

  stream << center_justify("Summary", 80) << '\n'
         << std::string(80, '-') << '\n';

  auto print = [&](uint64_t v, StringRef s) {
    stream << formatv("{0}",
                      fmt_align(formatv("{0:N}", v), AlignStyle::Right, 20))
           << " " << s << '\n';
  };

  bool hasStats = ctx.pdbStats.has_value();

  print(ctx.objFileInstances.size(),
        "Input OBJ files (expanded from all cmd-line inputs)");
  print(ctx.consumedInputsSize,
        "Size of all consumed OBJ files (non-lazy), in bytes");
  print(ctx.typeServerSourceMappings.size(), "PDB type server dependencies");
  print(ctx.precompSourceMappings.size(), "Precomp OBJ dependencies");
  print(hasStats ? ctx.pdbStats->nbTypeRecords : 0, "Input debug type records");
  print(hasStats ? ctx.pdbStats->nbTypeRecordsBytes : 0,
        "Size of all input debug type records, in bytes");
  print(hasStats ? ctx.pdbStats->nbTPIrecords : 0, "Merged TPI records");
  print(hasStats ? ctx.pdbStats->nbIPIrecords : 0, "Merged IPI records");
  print(hasStats ? ctx.pdbStats->strTabSize : 0, "Output PDB strings");
  print(hasStats ? ctx.pdbStats->globalSymbols : 0, "Global symbol records");
  print(hasStats ? ctx.pdbStats->moduleSymbols : 0, "Module symbol records");
  print(hasStats ? ctx.pdbStats->publicSymbols : 0, "Public symbol records");

  if (hasStats)
    stream << ctx.pdbStats->largeInputTypeRecs;

  Msg(ctx) << buffer;
}
