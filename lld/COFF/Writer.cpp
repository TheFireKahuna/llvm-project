//===- Writer.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
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
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/Win64EH.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <set>
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

// A relocation of clang's KCFI thunk: its offset, type and target, which is
// the mismatch routine, the start or end of the code range, or the guard
// function pointer.
struct KCFIThunkReloc {
  uint32_t offset;
  uint16_t type;
  uint8_t target;
};

// Clang's current KCFI thunks: the range test, the type check and a direct
// jump or return for a target inside the range, then the page test, the type
// check and the guard function for one outside it. On x86-64, -1 marks a
// byte that varies with the type and the options; relocated fields are zero.
static const int16_t kcfiDispatchX64[] = {
    0x4C, 0x8D, 0x15, 0,  0,  0,  0, // lea r10, [rip + __llvm_code_start]
    0x4C, 0x39, 0xD0,                // cmp rax, r10
    0x72, 0x22,                      // jb 1f
    0x4C, 0x8D, 0x15, 0,  0,  0,  0, // lea r10, [rip + __llvm_code_end]
    0x4C, 0x39, 0xD0,                // cmp rax, r10
    0x73, 0x16,                      // jae 1f
    0x49, 0xBB, -1,   -1, -1, -1, -1, -1, -1, -1, // mov r11, expected
    0x4C, 0x39, 0x58, -1,                         // cmp [rax - 8], r11
    0x0F, 0x85, 0,    0,  0,  0,                  // jne mismatch
    0xFF, 0xE0,                                   // jmp rax
    0xA9, -1,   -1,   -1, -1,                     // 1: test eax, mask
    0x0F, 0x84, 0,    0,  0,  0,                  // je mismatch
    0x49, 0xBB, -1,   -1, -1, -1, -1, -1, -1, -1, // mov r11, expected
    0x4C, 0x39, 0x58, -1,                         // cmp [rax - 8], r11
    0x0F, 0x85, 0,    0,  0,  0,                  // jne mismatch
    0xFF, 0x25, 0,    0,  0,  0,                  // jmp [rip + guard]
};
static const int16_t kcfiCheckX64[] = {
    0x4C, 0x8D, 0x15, 0,  0,  0,  0, // lea r10, [rip + __llvm_code_start]
    0x4C, 0x39, 0xD1,                // cmp rcx, r10
    0x72, 0x21,                      // jb 1f
    0x4C, 0x8D, 0x15, 0,  0,  0,  0, // lea r10, [rip + __llvm_code_end]
    0x4C, 0x39, 0xD1,                // cmp rcx, r10
    0x73, 0x15,                      // jae 1f
    0x49, 0xBB, -1,   -1, -1, -1, -1, -1, -1, -1, // mov r11, expected
    0x4C, 0x39, 0x59, -1,                         // cmp [rcx - 8], r11
    0x0F, 0x85, 0,    0,  0,  0,                  // jne mismatch
    0xC3,                                         // ret
    0xF7, 0xC1, -1,   -1, -1, -1,                 // 1: test ecx, mask
    0x0F, 0x84, 0,    0,  0,  0,                  // je mismatch
    0x49, 0xBB, -1,   -1, -1, -1, -1, -1, -1, -1, // mov r11, expected
    0x4C, 0x39, 0x59, -1,                         // cmp [rcx - 8], r11
    0x0F, 0x85, 0,    0,  0,  0,                  // jne mismatch
    0xFF, 0x25, 0,    0,  0,  0,                  // jmp [rip + guard]
};
// On ARM64, each instruction with the bits that vary with the type and the
// options.
static const struct {
  uint32_t insn;
  uint32_t varying;
} kcfiCheckARM64[] = {
    {0x90000010, 0x00000000}, // adrp x16, __llvm_code_start
    {0x91000210, 0x00000000}, // add x16, x16, :lo12:__llvm_code_start
    {0xEB1001FF, 0x00000000}, // cmp x15, x16
    {0x540001A3, 0x00000000}, // b.lo 1f
    {0x90000010, 0x00000000}, // adrp x16, __llvm_code_end
    {0x91000210, 0x00000000}, // add x16, x16, :lo12:__llvm_code_end
    {0xEB1001FF, 0x00000000}, // cmp x15, x16
    {0x54000122, 0x00000000}, // b.hs 1f
    {0xF84001F0, 0x001FF000}, // ldur x16, [x15, #-8]
    {0xD2800011, 0x001FFFE0}, // mov x17, #expected
    {0xF2A00011, 0x001FFFE0}, // movk x17, #expected, lsl #16
    {0xF2C00011, 0x001FFFE0}, // movk x17, #expected, lsl #32
    {0xF2E00011, 0x001FFFE0}, // movk x17, #expected, lsl #48
    {0xEB11021F, 0x00000000}, // cmp x16, x17
    {0x54000001, 0x00000000}, // b.ne mismatch
    {0xD65F03C0, 0x00000000}, // ret
    {0xF20001FF, 0x007FFC00}, // 1: tst x15, #mask
    {0x54000000, 0x00000000}, // b.eq mismatch
    {0xF84001F0, 0x001FF000}, // ldur x16, [x15, #-8]
    {0xD2800011, 0x001FFFE0}, // mov x17, #expected
    {0xF2A00011, 0x001FFFE0}, // movk x17, #expected, lsl #16
    {0xF2C00011, 0x001FFFE0}, // movk x17, #expected, lsl #32
    {0xF2E00011, 0x001FFFE0}, // movk x17, #expected, lsl #48
    {0xEB11021F, 0x00000000}, // cmp x16, x17
    {0x54000001, 0x00000000}, // b.ne mismatch
    {0x90000010, 0x00000000}, // adrp x16, guard
    {0xF9400210, 0x00000000}, // ldr x16, [x16, :lo12:guard]
    {0xD61F0200, 0x00000000}, // br x16
};
static const KCFIThunkReloc kcfiRelocsX64[] = {
    {3, IMAGE_REL_AMD64_REL32, 1},  {15, IMAGE_REL_AMD64_REL32, 2},
    {40, IMAGE_REL_AMD64_REL32, 0}, {53, IMAGE_REL_AMD64_REL32, 0},
    {73, IMAGE_REL_AMD64_REL32, 0}, {79, IMAGE_REL_AMD64_REL32, 3}};
static const KCFIThunkReloc kcfiCheckRelocsARM64[] = {
    {0, IMAGE_REL_ARM64_PAGEBASE_REL21, 1},
    {4, IMAGE_REL_ARM64_PAGEOFFSET_12A, 1},
    {16, IMAGE_REL_ARM64_PAGEBASE_REL21, 2},
    {20, IMAGE_REL_ARM64_PAGEOFFSET_12A, 2},
    {56, IMAGE_REL_ARM64_BRANCH19, 0},
    {68, IMAGE_REL_ARM64_BRANCH19, 0},
    {96, IMAGE_REL_ARM64_BRANCH19, 0},
    {100, IMAGE_REL_ARM64_PAGEBASE_REL21, 3},
    {104, IMAGE_REL_ARM64_PAGEOFFSET_12L, 3}};

// The KCFI prefix, with a marker, of a function the link keeps. From its
// __cfi_ symbol it holds an optional second type word, then 0F 1F 80, the
// marker and B8, then the type, before any patchable prefix and the entry.
struct KCFIPrefix {
  SectionChunk *chunk;
  // The offset in chunk of the __cfi_ symbol, and the size of the type words
  // and the marker from there, 12 or 16 bytes.
  uint32_t offset;
  uint32_t size;
  // The offset in chunk of the function's entry.
  uint32_t entry;
  // Whether the guard function table omits the function, whose type words
  // are then overwritten.
  bool sealed = false;
};

// The writer writes a SymbolTable result to a file.
class Writer {
public:
  Writer(COFFLinkerContext &c)
      : buffer(c.e.outputBuffer), strtab(StringTableBuilder::WinCOFF),
        delayIdata(c), ctx(c) {}
  void run();

private:
  void calculateStubDependentSizes();
  void createSections();
  void createMiscChunks();
  void createImportTables();
  void bindImportSlots();
  void placeImportSlotSections();
  bool iatStartsRdata() const;
  void packPlacedChunks();
  bool validateImportSlots(SectionChunk *sc, std::vector<ImportSlot> &slots);
  void appendImportThunks();
  void locateImportTables();
  uint64_t getIATSize() const;
  void checkImportSlots();
  void createExportTable();
  StringRef getMergeDestination(StringRef fromSection, StringRef toSection);
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
  void createECChunks();
  void insertCtorDtorSymbols();
  void insertBssDataStartEndSymbols();
  void markSymbolsWithRelocations(ObjFile *file, SymbolRVASet &usedSymbols,
                                  SymbolRVASet &usedImports);
  void markDescribedAddressTakes(ObjFile *file, SymbolRVASet &usedSymbols,
                                 SymbolRVASet &usedImports);
  void createGuardCFTables();
  void findKCFIPrefixes();
  void sealKCFIPrefixes();
  void placeLinkerDefinedSymbols();
  void defineKCFICodeRange();
  void rewriteKCFIThunks();
  void boundKCFIMismatches();
  SymbolRVASet getEHContTargets();
  bool protectDelayIat();
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
  void fixTlsAlignment(SymbolTable &symtab);
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
  uint64_t iatSize = 0;
  // The last chunk the import address table directory covers, when read-only
  // in-place import slots extend it past the address tables.
  Chunk *iatEnd = nullptr;
  // The read-only chunks holding in-place import slots that are laid out with
  // the import address tables, in their order there.
  std::vector<SectionChunk *> slotChunks;
  DenseSet<const SectionChunk *> heldBackChunks;
  // The read-only output sections other than .rdata that hold in-place import
  // slots, which are laid out before .rdata, and whether a $-group of .rdata
  // holds one; either places the import address tables at the start of
  // .rdata, so that the directory covers them all.
  SetVector<StringRef> slotSections;
  bool slotRdataGroups = false;
  // Whether packPlacedChunks lays out the pinned and 64-byte-aligned chunks of
  // .rdata.
  bool packRdata = false;
  DelayLoadContents delayIdata;
  bool setNoSEHCharacteristic = false;
  uint32_t tlsAlignment = 0;

  DebugDirectoryChunk *debugDirectory = nullptr;
  std::vector<std::pair<COFF::DebugType, Chunk *>> debugRecords;
  CVDebugRecordChunk *buildId = nullptr;
  ArrayRef<uint8_t> sectionTable;

  // List of Arm64EC export thunks.
  std::vector<std::pair<Chunk *, Defined *>> exportThunks;

  // The KCFI prefixes with a marker, found under -import-slots; by chunk, the
  // entries they precede, each with the bytes at a page's start it must stay
  // out of; whether the image is sealed; and the output section that holds
  // them all, if there is one.
  std::vector<KCFIPrefix> kcfiPrefixes;
  // The objects that define code and no KCFI prefix with a marker, which are
  // foreign.
  DenseSet<ObjFile *> kcfiForeignFiles;
  DenseMap<const Chunk *, SmallVector<std::pair<uint32_t, uint32_t>, 1>>
      kcfiEntries;
  bool kcfiSealed = false;
  bool kcfiUnprefixedTargets = false;
  OutputSection *kcfiCodeSec = nullptr;

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
};
} // anonymous namespace

void lld::coff::writeResult(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Write output(s)");
  Writer(ctx).run();
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
        thunkSymbolIndex = file->addRangeThunkSymbol(thunk);
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
  if (!ctx.symtab.isEC())
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

  Symbol *tableCountSym = ctx.symtab.findUnderscore("__hybrid_code_map_count");
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
    bindImportSlots();
    createSections();
    appendImportThunks();
    // Import thunks must be added before the Control Flow Guard tables are
    // added.
    createMiscChunks();
    createExportTable();
    mergeSections();
    sortECChunks();
    appendECImportTables();
    placeImportSlotSections();
    packPlacedChunks();
    createDynamicRelocs();
    removeUnusedSections();
    layoutSections();
    finalizeAddresses();
    removeEmptySections();
    assignOutputSectionIndices();
    placeLinkerDefinedSymbols();
    defineKCFICodeRange();
    setSectionPermissions();
    checkImportSlots();
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
    sealKCFIPrefixes();
    rewriteKCFIThunks();
    prepareLoadConfig();
    sortExceptionTables();

    // Fix up the alignment in the TLS Directory's characteristic field,
    // if a specific alignment value is needed
    if (tlsAlignment)
      ctx.forEachSymtab([&](SymbolTable &symtab) { fixTlsAlignment(symtab); });
  }

  if (!ctx.config.pdbPath.empty() && ctx.config.debug) {
    assert(buildId);
    createPDB(ctx, sectionTable, buildId->buildId);
  }
  writeBuildId();

  writeLLDMapFile(ctx);
  writeMapFile(ctx);

  writePEChecksum();

  printSummary();

  if (errorCount())
    return;

  llvm::TimeTraceScope timeScope("Commit PE to disk");
  ScopedTimer t2(ctx.outputCommitTimer);
  if (auto e = buffer->commit())
    Fatal(ctx) << "failed to write output '" << buffer->getPath()
               << "': " << toString(std::move(e));
}

static StringRef getOutputSectionName(StringRef name, bool isMinGW) {
  StringRef s = name.split('$').first;
  if (!isMinGW)
    return s;

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
    iatSize = alignTo(iatSize, 0x1000);

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
    if (!importAddresses->chunks.empty())
      iatStart = importAddresses->chunks.front();
    for (Chunk *c : importAddresses->chunks)
      iatSize += c->getSize();
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
  // The pieces of a KCFI type's list are kept in order by their suffix.
  if (name.starts_with(".rdata$llvm_kcfi_"))
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
  rsrcSec = createSection(".rsrc", data | r);
  relocSec = createSection(".reloc", data | discardable | r);
  ctorsSec = createSection(".ctors", data | r | w);
  dtorsSec = createSection(".dtors", data | r | w);

  // Pinned and 64-byte-aligned read-only chunks are packed under
  // -import-slots, the drivers' sign of a target that places vtables, where
  // the layout's page residues are those of the sections' offsets.
  const Configuration &config = ctx.config;
  packRdata =
      config.importSlots && config.order.empty() && config.align % 4096 == 0 &&
      !ctx.hybridSymtab && !isArm64EC(config.machine) &&
      llvm::any_of(ctx.driver.getChunks(), [&](Chunk *c) {
        auto *sc = dyn_cast<SectionChunk>(c);
        if (!sc || !sc->live || sc->getOutputCharacteristics() != (data | r) ||
            !(ctx.chunkPins.contains(sc) || sc->getAlignment() >= 64))
          return false;
        StringRef name = sc->getSectionName();
        if (shouldStripSectionSuffix(sc, name, config.mingw))
          name = name.split('$').first;
        return name == ".rdata" || heldBackChunks.contains(sc);
      });

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
    if (sc && sc->hasImportSlots && heldBackChunks.contains(sc))
      continue;
    StringRef name = c->getSectionName();
    if (shouldStripSectionSuffix(sc, name, ctx.config.mingw))
      name = name.split('$').first;

    if (name.starts_with(".tls"))
      tlsAlignment = std::max(tlsAlignment, c->getAlignment());

    PartialSection *pSec = createPartialSection(name,
                                                c->getOutputCharacteristics());
    pSec->chunks.push_back(c);
  }
  for (Chunk *c : ctx.symtab.kcfiChunks)
    createPartialSection(c->getSectionName(), c->getOutputCharacteristics())
        ->chunks.push_back(c);

  fixPartialSectionChars(".rsrc", data | r);
  fixPartialSectionChars(".edata", data | r);
  // Even in non MinGW cases, we might need to link against GNU import
  // libraries.
  bool hasIdata = fixGnuImportChunks();
  if (!idata.empty())
    hasIdata = true;

  if (hasIdata)
    addSyntheticIdata();

  sortSections();

  // The read-only chunks holding in-place import slots follow the import
  // address tables, inside the range the loader makes writable while it
  // binds imports. Section ordering has run, so it cannot separate them.
  if (!slotChunks.empty()) {
    PartialSection *pSec = findPartialSection(
        ".idata$5", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    pSec->chunks.insert(pSec->chunks.end(), slotChunks.begin(),
                        slotChunks.end());
    iatEnd = slotChunks.back();
  }

  if (hasIdata)
    locateImportTables();

  for (auto thunk : ctx.symtab.sameAddressThunks)
    wowthkSec->addChunk(thunk);

  // Then create an OutputSection for each section.
  // '$' and all following characters in input section names are
  // discarded when determining output section. So, .text$foo
  // contributes to .text, for example. See PE/COFF spec 3.2.
  for (auto it : partialSections) {
    PartialSection *pSec = it.second;
    StringRef name = getOutputSectionName(pSec->name, ctx.config.mingw);
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

    // Delay the chunks that placeImportSlotSections puts at the start of
    // .rdata.
    if (iatStartsRdata() &&
        (pSec->name == ".idata$5" || (slotRdataGroups && name == ".rdata" &&
                                      pSec->name.starts_with(".rdata$"))))
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

  if (config->importSlots)
    findKCFIPrefixes();

  // Create /guard:cf tables if requested.
  createGuardCFTables();
  boundKCFIMismatches();

  createECChunks();

  if (config->autoImport)
    createRuntimePseudoRelocs();

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

// Whether a section is unwind or exception-handling data, which the PE format
// defines as RVAs of the image's own code and handlers.
static bool isExceptionData(SectionChunk *sc) {
  StringRef name = sc->getSectionName();
  return name == ".xdata" || name == ".pdata" || name.starts_with(".xdata$") ||
         name.starts_with(".pdata$");
}

// Under -import-slots, a word of static data that holds the address of an
// import is an in-place import slot: the loader writes the address there
// through an import descriptor whose address table is the run of such words
// the word belongs to, so the word needs no base relocation, and a function's
// address is the function's, not its import thunk's. Code that takes the
// address of an imported function in an instruction its object describes, or
// in an ARM64 adrp and add pair, is rewritten to load it from the import
// address table, so that code and static data agree. An object that may take
// it in an instruction it does not describe makes the image use the import
// thunk as the function's address everywhere, with a warning. A delay-loaded
// function keeps its thunk, and code that loads its address from the import
// address table is rewritten to take the thunk's. A reference to data that
// resolved to its import other than by such a word cannot reach the data, and
// is an error.
void Writer::bindImportSlots() {
  Configuration &config = ctx.config;
  if (!config.importSlots || ctx.hybridSymtab || isArm64EC(config.machine))
    return;
  llvm::TimeTraceScope timeScope("Import slots");

  bool arm64 = config.machine == ARM64;
  auto delayLoaded = [&](DefinedImportData *imp) {
    return config.delayLoads.contains(imp->getDLLName().lower());
  };
  // An import thunk, data that resolved to its import, or the import address
  // table entry of a delay-loaded function.
  auto isImport = [&](Symbol *s) {
    if (isa_and_nonnull<DefinedImportThunk>(s))
      return true;
    auto *imp = dyn_cast_or_null<DefinedImportData>(s);
    return imp && (imp->isRuntimePseudoReloc ||
                   (imp->file->thunkSym && delayLoaded(imp)));
  };

  // An export of data that resolved to its import would publish the address
  // of the import address table entry.
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym);
        imp && imp->isRuntimePseudoReloc)
      Err(ctx) << "cannot export " << ctx.symtab.printSymbol(imp)
               << ": it is imported from " << imp->getDLLName()
               << "; export a forwarder to it instead";

  struct Pending {
    SectionChunk *chunk;
    std::vector<ImportSlot> slots;
    Symbol *firstRef;
  };
  std::vector<Pending> pending;
  struct Site {
    SectionChunk *chunk;
    uint32_t offset;
    DefinedImportData *imp;
  };
  std::vector<Site> addressSites;
  // On ARM64 the loads of a delay-loaded function's entry are rewritten for
  // the symbol as a whole, since an adrp may serve several loads.
  MapVector<DefinedImportData *,
            std::vector<std::pair<SectionChunk *, uint32_t>>>
      delayLoads;
  DenseSet<DefinedImportData *> delayLoadsRead;
  // For each import thunk, the references to it, and those that a slot or a
  // rewritten instruction bypasses.
  MapVector<DefinedImportThunk *, std::pair<size_t, size_t>> thunkRefs;

  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab ||
        llvm::none_of(file->getSymbols(), isImport))
      continue;
    SmallPtrSet<Symbol *, 4> reported;
    auto thunkIsAddress = [&](Symbol *s, DefinedImportData *imp,
                              SectionChunk *data = nullptr) {
      imp->file->thunkIsAddress = true;
      if (!reported.insert(s).second)
        return;
      if (data)
        Warn(ctx) << file << ": " << data->getSectionName()
                  << " holds the address of " << file->symtab.printSymbol(s)
                  << ", imported from " << imp->getDLLName()
                  << ", in a 32-bit field, so the image uses its import thunk "
                     "as its address";
      else
        Warn(ctx) << file << ": may take the address of "
                  << file->symtab.printSymbol(s) << ", imported from "
                  << imp->getDLLName()
                  << ", in an instruction it does not describe, so the image "
                     "uses its import thunk as its address; declare it "
                     "imported, or rebuild the object with clang";
    };

    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      uint32_t chars = sc->header->Characteristics;
      if (chars & IMAGE_SCN_MEM_DISCARDABLE)
        continue;
      bool code = chars & IMAGE_SCN_CNT_CODE;
      ArrayRef<coff_relocation> relocs = sc->getRelocs();
      Pending p{sc, {}, nullptr};
      for (size_t i = 0, e = relocs.size(); i != e; ++i) {
        const coff_relocation &rel = relocs[i];
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        if (!isImport(s))
          continue;
        auto *thunk = dyn_cast<DefinedImportThunk>(s);
        auto *data = dyn_cast<DefinedImportData>(s);
        if (thunk)
          ++thunkRefs[thunk].first;

        // The import address table entry of a delay-loaded function: a load
        // of the address in code takes the thunk's instead.
        if (data && !data->isRuntimePseudoReloc) {
          if (!code)
            delayLoadsRead.insert(data);
          else if (arm64 && sc->isArm64PointerLoad(rel))
            delayLoads[data].push_back({sc, rel.VirtualAddress});
          else if (arm64)
            delayLoadsRead.insert(data);
          else if (sc->isDescribedSite(rel, LinkSiteLoad))
            addressSites.push_back({sc, rel.VirtualAddress, data});
          continue;
        }

        // Data that resolved to its import is a copy of the import's symbol;
        // the import tables know the original.
        DefinedImportData *imp =
            thunk ? thunk->wrappedSym
                  : cast<DefinedImportData>(data->file->impSym);
        // A section-relative offset names a section of this image.
        if (sc->isSectionRelative(rel)) {
          if (reported.insert(s).second)
            Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                     << " is imported from " << imp->getDLLName() << ", but "
                     << sc->getSectionName()
                     << " refers to it with relocation type "
                     << file->getCOFFObj()->getRelocationTypeName(rel.Type)
                     << ", which names a section of this image";
          continue;
        }
        if (code) {
          if (!thunk) {
            if (reported.insert(s).second)
              Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                       << " is imported from " << imp->getDLLName()
                       << ", but the object was compiled as if it were "
                          "local; mark its declaration, or compile the object "
                          "with -fauto-import";
            continue;
          }
          if (sc->isAddressWord(rel)) {
            Err(ctx) << file << ": " << sc->getSectionName()
                     << " is executable and holds the address of "
                     << file->symtab.printSymbol(s) << ", imported from "
                     << imp->getDLLName();
            continue;
          }
          if (delayLoaded(imp))
            continue;
          if (arm64) {
            switch (rel.Type) {
            case IMAGE_REL_ARM64_PAGEBASE_REL21:
              if (i + 1 != e && sc->isArm64AddressPair(rel, relocs[i + 1])) {
                addressSites.push_back({sc, rel.VirtualAddress, imp});
                addressSites.push_back({sc, relocs[i + 1].VirtualAddress, imp});
                ++thunkRefs[thunk].first;
                thunkRefs[thunk].second += 2;
                ++i;
              } else {
                thunkIsAddress(s, imp);
              }
              break;
            case IMAGE_REL_ARM64_PAGEOFFSET_12A:
            case IMAGE_REL_ARM64_REL21:
              thunkIsAddress(s, imp);
              break;
            default:
              break;
            }
            continue;
          }
          switch (rel.Type) {
          case IMAGE_REL_AMD64_REL32: {
            if (!file->describesSites) {
              thunkIsAddress(s, imp);
              break;
            }
            std::optional<LinkSiteForm> form =
                file->getLinkSiteForm(sc, rel.VirtualAddress);
            if (form == LinkSiteAddress) {
              if (sc->isDescribedSite(rel, LinkSiteAddress)) {
                addressSites.push_back({sc, rel.VirtualAddress, imp});
                ++thunkRefs[thunk].second;
              } else {
                Err(ctx) << file << ": the instruction at offset 0x"
                         << Twine::utohexstr(rel.VirtualAddress) << " in "
                         << sc->getSectionName()
                         << " is not the one its link-only record describes";
              }
            } else if (form == LinkSiteOther) {
              thunkIsAddress(s, imp);
            }
            break;
          }
          case IMAGE_REL_AMD64_REL32_1:
          case IMAGE_REL_AMD64_REL32_2:
          case IMAGE_REL_AMD64_REL32_3:
          case IMAGE_REL_AMD64_REL32_4:
          case IMAGE_REL_AMD64_REL32_5:
            Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                     << " is imported from " << imp->getDLLName()
                     << ", but an instruction in " << sc->getSectionName()
                     << " reads its bytes, which its import thunk's are not";
            break;
          default:
            break;
          }
          continue;
        }

        // A 32-bit field cannot hold another image's address. A field
        // through which the function is only called, as a frame's handler is
        // in unwind data, is served by its import thunk; any other function
        // address there makes the thunk the function's address image-wide.
        // An object that lists its call-only fields says which they are;
        // otherwise they are those of exception data, which the PE format
        // defines as RVAs that the system calls.
        if (!sc->isAddressWord(rel)) {
          bool callOnly = file->listsCallOnly
                              ? file->isCallOnlyRef(sc, rel.VirtualAddress)
                              : isExceptionData(sc);
          if (thunk && !callOnly)
            thunkIsAddress(s, imp, sc);
          else if (!thunk && reported.insert(s).second)
            Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                     << " is imported from " << imp->getDLLName() << ", but "
                     << sc->getSectionName()
                     << " refers to it with relocation type "
                     << file->getCOFFObj()->getRelocationTypeName(rel.Type)
                     << ", which cannot reach another image";
          continue;
        }
        if (delayLoaded(imp))
          continue;
        if (uint64_t(rel.VirtualAddress) + config.wordsize > sc->getSize()) {
          Err(ctx) << file << ": the address of " << file->symtab.printSymbol(s)
                   << " at offset 0x" << Twine::utohexstr(rel.VirtualAddress)
                   << " extends past the end of " << sc->getSectionName()
                   << " (size 0x" << Twine::utohexstr(sc->getSize()) << ")";
          continue;
        }
        const uint8_t *word = sc->getContents().data() + rel.VirtualAddress;
        int64_t addend =
            config.is64() ? int64_t(read64le(word)) : int32_t(read32le(word));
        if (addend) {
          Err(ctx) << file << ": " << sc->getSectionName()
                   << " holds the address of " << file->symtab.printSymbol(s)
                   << " plus " << addend << ", imported from "
                   << imp->getDLLName()
                   << ", but the loader writes only the address itself";
          continue;
        }
        p.slots.push_back({sc, rel.VirtualAddress, imp});
        if (thunk)
          ++thunkRefs[thunk].second;
        if (!p.firstRef)
          p.firstRef = s;
      }
      if (!p.slots.empty())
        pending.push_back(std::move(p));
    }
  }

  // A function that an object may take the address of in an instruction it
  // does not describe keeps its thunk as its address: its words in static
  // data name the thunk, and the instructions that take its address are left
  // as they are.
  // The thunk whose address a rewritten load takes is kept in the image.
  auto keepThunk = [](DefinedImportData *imp) {
    cast<DefinedImportThunk>(imp->file->thunkSym)->getChunk()->live = true;
  };
  for (const Site &site : addressSites) {
    if (site.imp->file->thunkIsAddress)
      continue;
    ctx.importSites.insert({site.chunk, site.offset});
    if (delayLoaded(site.imp))
      keepThunk(site.imp);
  }
  for (auto &[imp, sites] : delayLoads) {
    if (delayLoadsRead.contains(imp))
      continue;
    for (auto [sc, offset] : sites)
      ctx.importSites.insert({sc, offset});
    keepThunk(imp);
  }

  // An import thunk that every reference bypasses is left out of the image,
  // and so out of the Control Flow Guard tables, unless something other than
  // a relocation names it.
  DenseSet<Symbol *> named(config.gcroot.begin(), config.gcroot.end());
  for (Export &e : ctx.symtab.exports)
    named.insert(e.sym);
  for (auto &[thunk, refs] : thunkRefs)
    if (refs.first == refs.second && !thunk->wrappedSym->file->thunkIsAddress &&
        !delayLoaded(thunk->wrappedSym) && !named.contains(thunk))
      thunk->getChunk()->live = false;

  std::vector<SectionChunk *> writable;
  for (Pending &p : pending) {
    SectionChunk *sc = p.chunk;
    llvm::erase_if(p.slots, [](const ImportSlot &s) {
      return s.sym->file->thunkIsAddress;
    });
    if (p.slots.empty())
      continue;
    llvm::stable_sort(p.slots, [](const ImportSlot &a, const ImportSlot &b) {
      return a.offset < b.offset;
    });
    if (!validateImportSlots(sc, p.slots))
      continue;

    // A read-only slot can be written only inside the range the loader
    // makes writable while it binds imports. A chunk of .rdata is moved
    // there. A read-only section whose order or bounds its program may rely
    // on keeps its place: another section is laid out beside that range, and
    // the $-groups of .rdata, together and in their order, at its start.
    // Exception data never holds an import's address.
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
    StringRef name = sc->getSectionName();
    if (shouldStripSectionSuffix(sc, name, config.mingw))
      name = name.split('$').first;
    StringRef outName = getOutputSectionName(name, config.mingw);
    if (readOnly && outName == ".xdata") {
      Err(ctx) << sc->file << ": " << sc->getSectionName()
               << " is read-only and holds the address of "
               << sc->file->symtab.printSymbol(p.firstRef) << ", imported from "
               << p.slots[0].sym->getDLLName()
               << ", but cannot be laid out with the import address table";
      continue;
    }
    sc->hasImportSlots = true;
    ctx.importSlots[sc] = std::move(p.slots);
    if (readOnly && name == ".rdata") {
      slotChunks.push_back(sc);
      continue;
    }
    if (readOnly && outName == ".rdata")
      slotRdataGroups = true;
    else if (readOnly)
      slotSections.insert(outName);
    writable.push_back(sc);
  }
  if (ctx.importSlots.empty())
    return;

  // Read-only slot chunks are ordered by the DLL of their first slot, so that
  // single-pointer chunks of one DLL form one run.
  auto dllIndex = [&](SectionChunk *sc) {
    StringRef dll = ctx.importSlots[sc].front().sym->getDLLName();
    return config.dllOrder[dll.lower()];
  };
  llvm::stable_sort(slotChunks, [&](SectionChunk *a, SectionChunk *b) {
    return dllIndex(a) < dllIndex(b);
  });
  heldBackChunks.insert(slotChunks.begin(), slotChunks.end());

  // Slots of one DLL one word apart form a run: within a chunk, and across
  // two read-only chunks laid out with no padding between them.
  std::vector<ImportSlot *> *run = nullptr;
  ImportSlot *prev = nullptr;
  auto follows = [&](ImportSlot &s) {
    if (!prev ||
        !prev->sym->getDLLName().equals_insensitive(s.sym->getDLLName()))
      return false;
    if (prev->chunk == s.chunk)
      return s.offset == prev->offset + config.wordsize;
    SectionChunk *a = prev->chunk;
    return heldBackChunks.contains(a) && heldBackChunks.contains(s.chunk) &&
           !ctx.chunkPins.contains(s.chunk) &&
           s.offset == 0 && prev->offset + config.wordsize == a->getSize() &&
           s.chunk->getAlignment() <= a->getAlignment() &&
           a->getSize() % s.chunk->getAlignment() == 0;
  };
  auto addRuns = [&](SectionChunk *sc) {
    for (ImportSlot &s : ctx.importSlots[sc]) {
      if (!follows(s)) {
        idata.slotRuns.emplace_back();
        run = &idata.slotRuns.back();
      }
      run->push_back(&s);
      prev = &s;
    }
  };
  for (SectionChunk *sc : writable) {
    prev = nullptr;
    addRuns(sc);
  }
  prev = nullptr;
  for (SectionChunk *sc : slotChunks)
    addRuns(sc);
}

// Every word that an in-place import slot covers has that slot as its only
// writer: a relocation overlapping it, which would also be a second writer of
// a base relocation, is an error. An identical duplicate is one binding.
bool Writer::validateImportSlots(SectionChunk *sc,
                                 std::vector<ImportSlot> &slots) {
  struct Span {
    uint32_t offset;
    uint32_t size;
    const coff_relocation *rel;
    bool slot;
  };
  DenseSet<uint32_t> slotOffsets;
  for (const ImportSlot &s : slots)
    slotOffsets.insert(s.offset);
  std::vector<Span> spans;
  for (const coff_relocation &rel : sc->getRelocs()) {
    if (rel.Type == 0) // IMAGE_REL_*_ABSOLUTE writes nothing.
      continue;
    bool word = sc->isAddressWord(rel);
    spans.push_back({rel.VirtualAddress,
                     word ? uint32_t(ctx.config.wordsize) : 4u, &rel,
                     word && slotOffsets.contains(rel.VirtualAddress)});
  }
  llvm::stable_sort(
      spans, [](const Span &a, const Span &b) { return a.offset < b.offset; });

  bool ok = true;
  const Span *last = nullptr;
  for (const Span &s : spans) {
    if (last && uint64_t(last->offset) + last->size > s.offset &&
        (last->slot || s.slot)) {
      bool identical = last->offset == s.offset && last->size == s.size &&
                       last->rel->Type == s.rel->Type &&
                       last->rel->SymbolTableIndex == s.rel->SymbolTableIndex;
      if (!identical) {
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(last->slot ? last->offset : s.offset)
                 << " in " << sc->getSectionName()
                 << " overlaps the relocation at offset 0x"
                 << Twine::utohexstr(last->slot ? s.offset : last->offset);
        ok = false;
      }
    }
    if (!last ||
        uint64_t(s.offset) + s.size > uint64_t(last->offset) + last->size)
      last = &s;
  }
  if (!ok)
    return false;
  slots.erase(llvm::unique(slots,
                           [](const ImportSlot &a, const ImportSlot &b) {
                             return a.offset == b.offset;
                           }),
              slots.end());
  return true;
}

// Whether the import address tables, and the read-only chunks holding
// in-place import slots after them, start .rdata: when other sections holding
// slots must lie next to them, and when they are packed before the rest of
// .rdata.
bool Writer::iatStartsRdata() const {
  return slotRdataGroups || !slotSections.empty() ||
         (packRdata && !slotChunks.empty());
}

// A read-only section other than .rdata that holds in-place import slots is
// laid out before .rdata, whose start holds the import address tables, the
// read-only chunks laid out with them, and then, when one of them holds a
// slot, the $-groups of .rdata in their order, so that the import address
// table directory covers every read-only slot with no other data between.
void Writer::placeImportSlotSections() {
  if (!iatStartsRdata())
    return;
  const uint32_t rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  std::vector<Chunk *> start;
  std::vector<PartialSection *> contribs;
  for (auto &[key, pSec] : partialSections) {
    if (key.characteristics != rdata ||
        (key.name != ".idata$5" &&
         !(slotRdataGroups && key.name.starts_with(".rdata$"))))
      continue;
    // .idata$5 sorts before .rdata$, so the import address tables come first.
    start.insert(start.end(), pSec->chunks.begin(), pSec->chunks.end());
    contribs.push_back(pSec);
  }
  rdataSec->chunks.insert(rdataSec->chunks.begin(), start.begin(), start.end());
  rdataSec->contribSections.insert(rdataSec->contribSections.begin(),
                                   contribs.begin(), contribs.end());
  iatEnd = start.back();

  std::vector<OutputSection *> before;
  // A section merged into another is left empty and is not laid out.
  for (StringRef name : slotSections)
    if (OutputSection *sec = findSection(name);
        sec && sec != rdataSec && !sec->chunks.empty())
      before.push_back(sec);
  if (before.empty())
    return;
  llvm::erase_if(ctx.outputSections,
                 [&](OutputSection *sec) { return is_contained(before, sec); });
  ctx.outputSections.insert(llvm::find(ctx.outputSections, rdataSec),
                            before.begin(), before.end());
  iatStart = before.front()->chunks.front();
}

namespace {
// What packPlacedChunks places: a chunk, or chunks that a run of in-place
// import slots crossing from one to the next binds together with no padding
// between them. Its alignment and pin are its first chunk's.
struct PackItem {
  SmallVector<Chunk *, 1> chunks;
  uint64_t size = 0;
  uint32_t align = 1;
  const ChunkPin *pin = nullptr;
  bool isPlaced() const { return pin || align >= 64; }
  // The first offset at or after pos that the item may start at.
  uint64_t startAt(uint64_t pos) const {
    pos = alignTo(pos, align);
    if (pin)
      pos += (pin->residue - pos) & maskTrailingOnes<uint64_t>(pin->log2);
    return pos;
  }
};
} // namespace

// Lays out the pinned and 64-byte-aligned chunks of .rdata, vtables in
// practice, so that the padding before each holds other read-only data
// rather than zeros. Each in turn is the one that needs the least padding from
// where the layout has reached, and its padding is filled with the largest
// unplaced chunks that fit. The chunks held back into the import address
// table directory are packed first, at the start of .rdata; chunks of plain
// .rdata may fill their gaps, and move into the directory to do so. Nothing
// else moves. assignAddresses honours the pins whatever the plan.
void Writer::packPlacedChunks() {
  if (!packRdata)
    return;
  const uint32_t rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  PartialSection *plain = findPartialSection(".rdata", rdata);
  PartialSection *iat = findPartialSection(".idata$5", rdata);
  if (iat && !is_contained(rdataSec->contribSections, iat))
    iat = nullptr;
  if (plain && !is_contained(rdataSec->contribSections, plain))
    plain = nullptr;

  // The items, held-back ones first, in their current order.
  DenseMap<Chunk *, Chunk *> continuedBy;
  for (std::vector<ImportSlot *> &run : idata.slotRuns)
    for (size_t i = 1; i < run.size(); ++i)
      if (run[i]->chunk != run[i - 1]->chunk)
        continuedBy[run[i - 1]->chunk] = run[i]->chunk;
  std::vector<PackItem> items;
  auto addItem = [&](Chunk *c) {
    PackItem &item = items.emplace_back();
    item.chunks.push_back(c);
    item.size = c->getSize();
    item.align = c->getAlignment();
    if (auto it = ctx.chunkPins.find(c); it != ctx.chunkPins.end())
      item.pin = &it->second;
  };
  size_t numHeldBack = 0;
  if (iat) {
    for (Chunk *c : iat->chunks) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !heldBackChunks.contains(sc))
        continue;
      if (numHeldBack && continuedBy.lookup(items.back().chunks.back()) == c) {
        items.back().chunks.push_back(c);
        items.back().size += c->getSize();
        continue;
      }
      addItem(c);
      numHeldBack = items.size();
    }
  }
  if (plain)
    for (Chunk *c : plain->chunks)
      addItem(c);
  if (llvm::none_of(items, [](const PackItem &item) { return item.isPlaced(); }))
    return;

  // The offset in .rdata of the end of the chunks before target.
  auto offsetOf = [&](Chunk *target) {
    uint64_t pos = 0;
    for (Chunk *c : rdataSec->chunks) {
      if (c == target)
        break;
      pos = alignTo(pos, c->getAlignment());
      if (auto it = ctx.chunkPins.find(c); it != ctx.chunkPins.end())
        pos += (it->second.residue - pos) &
               maskTrailingOnes<uint64_t>(it->second.log2);
      pos += c->getSize();
    }
    return pos;
  };

  // The unplaced items that may fill a gap, by size and then by reverse
  // order, so that the search down from the largest that fits meets equal
  // sizes in input order.
  std::set<std::pair<uint64_t, size_t>> fillers;
  auto fillerKey = [&](size_t i) {
    return std::make_pair(items[i].size, SIZE_MAX - i);
  };
  auto fill = [&](uint64_t &pos, uint64_t end, std::vector<size_t> &out) {
    while (pos < end) {
      auto it = fillers.upper_bound({end - pos, SIZE_MAX});
      bool placed = false;
      // An item's alignment can keep it from a gap its size fits; a few
      // smaller ones are tried before the gap is left as it is.
      for (int tries = 0; it != fillers.begin() && tries != 4; ++tries) {
        --it;
        size_t i = SIZE_MAX - it->second;
        uint64_t start = alignTo(pos, items[i].align);
        if (start + items[i].size > end)
          continue;
        out.push_back(i);
        pos = start + items[i].size;
        fillers.erase(it);
        placed = true;
        break;
      }
      if (!placed)
        return;
    }
  };

  // Packs the placed items of [begin, end) from offset pos, returning them and
  // the fillers that went into their gaps in layout order.
  auto pack = [&](size_t begin, size_t end, uint64_t pos) {
    std::vector<size_t> out, pinned;
    std::map<uint32_t, std::deque<size_t>> aligned;
    for (size_t i = begin; i != end; ++i) {
      if (items[i].pin)
        pinned.push_back(i);
      else if (items[i].isPlaced())
        aligned[items[i].align].push_back(i);
    }
    for (;;) {
      size_t best = SIZE_MAX;
      uint64_t bestStart = 0;
      auto consider = [&](size_t i) {
        uint64_t start = items[i].startAt(pos);
        if (best == SIZE_MAX || start < bestStart ||
            (start == bestStart && i < best)) {
          best = i;
          bestStart = start;
        }
      };
      for (size_t i : pinned)
        consider(i);
      // An aligned item goes first only if it ends before the next pinned
      // one would start, so that it never pushes a pin a page further on.
      uint64_t pinStart = best == SIZE_MAX ? UINT64_MAX : bestStart;
      for (auto &[align, queue] : aligned)
        if (!queue.empty() &&
            (pinStart == UINT64_MAX ||
             items[queue.front()].startAt(pos) + items[queue.front()].size <=
                 pinStart))
          consider(queue.front());
      if (best == SIZE_MAX)
        return out;
      fill(pos, bestStart, out);
      out.push_back(best);
      pos = bestStart + items[best].size;
      if (items[best].pin)
        llvm::erase(pinned, best);
      else
        aligned[items[best].align].pop_front();
    }
  };

  // Lays out the chunks of the items that order lists where the chunks of
  // pSec that isReplaced selects were, in pSec and in .rdata; any other chunk
  // of those items leaves the place it had.
  auto relayout = [&](PartialSection *pSec, ArrayRef<size_t> order,
                      function_ref<bool(Chunk *)> isReplaced) {
    std::vector<Chunk *> chunks;
    for (size_t i : order)
      llvm::append_range(chunks, items[i].chunks);
    DenseSet<Chunk *> moved(chunks.begin(), chunks.end());
    auto rebuild = [&](std::vector<Chunk *> &v) {
      std::vector<Chunk *> out;
      bool inserted = false;
      for (Chunk *c : v) {
        if (isReplaced(c)) {
          if (!inserted)
            llvm::append_range(out, chunks);
          inserted = true;
        } else if (!moved.contains(c)) {
          out.push_back(c);
        }
      }
      v = std::move(out);
    };
    rebuild(pSec->chunks);
    rebuild(rdataSec->chunks);
  };

  for (size_t i = numHeldBack; i != items.size(); ++i)
    if (!items[i].isPlaced())
      fillers.insert(fillerKey(i));

  // The held-back items stay in the directory, after the address tables. The
  // fillers of their gaps join them, and their unplaced items follow.
  if (numHeldBack) {
    for (size_t i = 0; i != numHeldBack; ++i)
      if (!items[i].isPlaced())
        fillers.insert(fillerKey(i));
    std::vector<size_t> order =
        pack(0, numHeldBack, offsetOf(items[0].chunks.front()));
    for (size_t i = 0; i != numHeldBack; ++i)
      if (!items[i].isPlaced() && fillers.erase(fillerKey(i)))
        order.push_back(i);
    DenseSet<Chunk *> heldBack, movedIn;
    for (size_t i : order)
      (i < numHeldBack ? heldBack : movedIn)
          .insert(items[i].chunks.begin(), items[i].chunks.end());
    relayout(iat, order, [&](Chunk *c) { return heldBack.contains(c); });
    if (plain) {
      llvm::erase_if(plain->chunks,
                     [&](Chunk *c) { return movedIn.contains(c); });
      // An emptied partial section contributes nothing.
      if (plain->chunks.empty())
        llvm::erase(rdataSec->contribSections, plain);
    }
    if (iatEnd && heldBack.contains(iatEnd))
      iatEnd = iat->chunks.back();
  }

  // Then plain .rdata, from where its remaining chunks start.
  if (plain && !plain->chunks.empty()) {
    std::vector<size_t> order =
        pack(numHeldBack, items.size(), offsetOf(plain->chunks.front()));
    for (size_t i = numHeldBack; i != items.size(); ++i)
      if (!items[i].isPlaced() && fillers.erase(fillerKey(i)))
        order.push_back(i);
    DenseSet<Chunk *> remaining(plain->chunks.begin(), plain->chunks.end());
    relayout(plain, order, [&](Chunk *c) { return remaining.contains(c); });
  }
}

// The size of the import address table directory: the address tables, and
// the read-only in-place import slots laid out after them.
uint64_t Writer::getIATSize() const {
  if (!iatEnd)
    return iatSize;
  return iatEnd->getRVA() + iatEnd->getSize() - iatStart->getRVA();
}

// The loader writes an in-place import slot with an absolute address, makes
// only the import address table directory writable while it binds imports, and
// restores one protection over all of it. Each slot's word must therefore be
// in a section that is not executable, a read-only one inside the directory,
// whose page-rounded range has one protection; each run must still be one word
// a slot.
void Writer::checkImportSlots() {
  if (ctx.importSlots.empty())
    return;
  const uint32_t perms =
      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;
  uint64_t iatBegin = iatStart->getRVA();
  uint64_t iatLimit = iatBegin + getIATSize();
  for (auto &[sc, slots] : ctx.importSlots) {
    OutputSection *os = ctx.getOutputSection(sc);
    uint32_t chars = os->header.Characteristics;
    for (const ImportSlot &s : slots) {
      uint64_t rva = sc->getRVA() + s.offset;
      if (chars & IMAGE_SCN_MEM_EXECUTE)
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(s.offset) << " in " << sc->getSectionName()
                 << " is in executable section " << os->name;
      else if (!(chars & IMAGE_SCN_MEM_WRITE) &&
               (rva < iatBegin || rva + ctx.config.wordsize > iatLimit))
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(s.offset) << " in " << sc->getSectionName()
                 << " is in read-only section " << os->name
                 << " outside the import address table";
    }
  }
  uint64_t pageBegin = alignDown(iatBegin, ctx.config.align);
  uint64_t pageLimit = alignTo(iatLimit, ctx.config.align);
  for (OutputSection *os : ctx.outputSections) {
    if (os->getRVA() >= pageLimit ||
        os->getRVA() + os->getVirtualSize() <= pageBegin)
      continue;
    if ((os->header.Characteristics & perms) != IMAGE_SCN_MEM_READ)
      Err(ctx) << "section " << os->name
               << " shares a page with the import address table, which holds "
                  "in-place import slots, but is not read-only data";
  }
  for (const std::vector<ImportSlot *> &run : idata.slotRuns)
    for (size_t i = 1; i < run.size(); ++i)
      if (run[i]->chunk->getRVA() + run[i]->offset !=
          run[i - 1]->chunk->getRVA() + run[i - 1]->offset +
              ctx.config.wordsize)
        Err(ctx) << run[i]->chunk->file
                 << ": a run of in-place import slots in "
                 << run[i]->chunk->getSectionName()
                 << " is not contiguous in the image";
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
    // A protected delay-load import address table is alone in .didat, as
    // link.exe lays it out: the loader keeps the section read-only and makes
    // it writable only while it resolves an import. The descriptors and the
    // name table are never written and go with the other read-only data.
    OutputSection *tableSec = didatSec;
    OutputSection *iatSec = dataSec;
    if (protectDelayIat()) {
      if (!didatSec->chunks.empty())
        Err(ctx) << "input sections named .didat cannot share the protected "
                    "delay-load import address table's section";
      // .didat=.rdata is the default merge rule.
      auto it = ctx.config.merge.find(".didat");
      if (it != ctx.config.merge.end()) {
        if (it->second != ".rdata")
          Err(ctx) << "/merge:.didat=" << it->second
                   << ": .didat holds the protected delay-load import address "
                      "table and cannot be merged";
        ctx.config.merge.erase(it);
      }
      for (auto &p : ctx.config.merge)
        if (getMergeDestination(p.first, p.second) == ".didat")
          Err(ctx) << "/merge:" << p.first << "=" << p.second
                   << ": .didat holds the protected delay-load import address "
                      "table and cannot be merged into";
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
      ctx.symtab.edataStart = edataSec->chunks.front();
      ctx.symtab.edataEnd = edataSec->chunks.back();
    } else {
      // On hybrid target, split EC and native chunks.
      llvm::stable_sort(edataSec->chunks, [=](const Chunk *a, const Chunk *b) {
        return (a->getMachine() != ARM64) < (b->getMachine() != ARM64);
      });

      for (auto chunk : edataSec->chunks) {
        if (chunk->getMachine() != ARM64) {
          ctx.symtab.edataStart = chunk;
          ctx.symtab.edataEnd = edataSec->chunks.back();
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

StringRef Writer::getMergeDestination(StringRef fromSection,
                                      StringRef toSection) {
  StringSet<> names;
  while (true) {
    if (!names.insert(toSection).second)
      Fatal(ctx) << "/merge: cycle found for section '" << fromSection << "'";
    auto i = ctx.config.merge.find(toSection);
    if (i == ctx.config.merge.end())
      break;
    toSection = i->second;
  }
  return toSection;
}

void Writer::mergeSection(const std::map<StringRef, StringRef>::value_type &p) {
  if (p.first == p.second)
    return;

  StringRef toSection = getMergeDestination(p.first, p.second);

  OutputSection *from = findSection(p.first);
  OutputSection *to = findSection(toSection);
  if (!from)
    return;
  if (!to) {
    from->name = toSection;
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
  if (it != ctx.config.merge.end()) {
    // Resolve the final merge target name following the chain.
    StringRef toSection = getMergeDestination(it->first, it->second);
    // Don't merge .bss into a shared section. MSVC link.exe keeps .bss
    // separate when the target has IMAGE_SCN_MEM_SHARED, preventing unexpected
    // sharing across processes.
    auto secIt = ctx.config.section.find(toSection);
    if (secIt == ctx.config.section.end() ||
        !(secIt->second & IMAGE_SCN_MEM_SHARED))
      mergeSection({it->first, toSection});
  }
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
      // A KCFI check outside the code range reads no prefix before a target
      // in a page's first bytes, which may follow an unmapped page, and treats
      // it as foreign, so no entry of a prefixed function goes there. Padding
      // by less than a page tries every place the alignment allows.
      if (auto it = kcfiEntries.find(c); it != kcfiEntries.end()) {
        auto inPageStart = [&] {
          return llvm::any_of(it->second, [&](auto entry) {
            return (rva + virtualSize + entry.first) % 4096 < entry.second;
          });
        };
        for (uint32_t pad = c->getAlignment(); pad < 4096 && inPageStart();
             pad += c->getAlignment())
          virtualSize += c->getAlignment();
      }
      // A pinned chunk starts at its residue, whatever comes before it.
      if (auto it = ctx.chunkPins.find(c); it != ctx.chunkPins.end())
        virtualSize += (it->second.residue - rva - virtualSize) &
                       maskTrailingOnes<uint64_t>(it->second.log2);
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
  SymbolTable &symtab =
      ctx.config.machine == ARM64X ? *ctx.hybridSymtab : ctx.symtab;
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
  if (config->guardCF & GuardCFLevel::CF)
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
      FileOutputBuffer::create(path, fileSize, FileOutputBuffer::F_executable),
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
      handlers.empty() || !ctx.symtab.findUnderscore("_load_config_used");

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
static bool isCallOrJump(LinkSiteForm form) {
  return form == LinkSiteCall || form == LinkSiteJump ||
         form == LinkSiteJumpOnePrefix;
}

// Whether the target of rel, as its object sees it, may resolve to something
// other than a definition the object fixes: a symbol it leaves undefined or
// weak, or a symbol in a COMDAT other than the section of rel. An object that
// describes its instruction sites gives every such REL32 in code that is not a
// branch a site, so such a REL32 without one is a branch.
static bool hasPreemptibleTarget(ObjFile *file, const SectionChunk *sc,
                                 const coff_relocation &rel) {
  COFFObjectFile *obj = file->getCOFFObj();
  Expected<COFFSymbolRef> sym = obj->getSymbol(rel.SymbolTableIndex);
  if (!sym) {
    consumeError(sym.takeError());
    return false;
  }
  int32_t secNum = sym->getSectionNumber();
  if (secNum == IMAGE_SYM_UNDEFINED)
    return true;
  if (secNum <= 0 || uint32_t(secNum) == sc->getSectionNumber())
    return false;
  Expected<const coff_section *> sec = obj->getSection(secNum);
  if (!sec) {
    consumeError(sec.takeError());
    return false;
  }
  return (*sec)->Characteristics & IMAGE_SCN_LNK_COMDAT;
}

// Adds the address that a reference other than a call takes: a function's, the
// symbol's that a local import pointer holds, or an import address table
// entry's, with the delay-load thunk the entry holds until resolved.
static void markAddressTake(Symbol *ref, SymbolRVASet &usedSymbols,
                            SymbolRVASet &usedImports) {
  if (auto *li = dyn_cast_or_null<DefinedLocalImport>(ref)) {
    maybeAddAddressTakenFunction(usedSymbols, li->getTarget());
    return;
  }
  if (auto *imp = dyn_cast_or_null<DefinedImportData>(ref)) {
    addSymbolToRVASet(usedImports, imp);
    if (imp->loadThunkSym)
      addSymbolToRVASet(usedSymbols, imp->loadThunkSym);
  }
  maybeAddAddressTakenFunction(usedSymbols, ref);
}

// The compiler lists the addresses an object takes, but not those taken by
// instructions it did not generate, such as inline assembly, nor the one that
// a load through a local import pointer takes once rewritten to compute the
// address. An object that describes its instruction sites says which
// instructions take one: every site that is not a call or jump.
void Writer::markDescribedAddressTakes(ObjFile *file,
                                       SymbolRVASet &usedSymbols,
                                       SymbolRVASet &usedImports) {
  for (Chunk *c : file->getChunks()) {
    SectionChunk *sc = dyn_cast<SectionChunk>(c);
    if (!sc || !sc->live || file->getLinkSites(sc).empty())
      continue;
    for (const coff_relocation &reloc : sc->getRelocs()) {
      if (reloc.Type != IMAGE_REL_AMD64_REL32)
        continue;
      std::optional<LinkSiteForm> form =
          file->getLinkSiteForm(sc, reloc.VirtualAddress);
      if (!form || isCallOrJump(*form))
        continue;
      // A site rewritten for an import takes the address of what it now
      // reaches.
      Symbol *ref = sc->getImportSiteTarget(reloc);
      markAddressTake(ref ? ref : file->getSymbol(reloc.SymbolTableIndex),
                      usedSymbols, usedImports);
    }
  }
}

void Writer::markSymbolsWithRelocations(ObjFile *file,
                                        SymbolRVASet &usedSymbols,
                                        SymbolRVASet &usedImports) {
  for (Chunk *c : file->getChunks()) {
    // We only care about live section chunks. Common chunks and other chunks
    // don't generally contain relocations.
    SectionChunk *sc = dyn_cast<SectionChunk>(c);
    if (!sc || !sc->live)
      continue;
    bool described = file->describesSites &&
                     (sc->getOutputCharacteristics() & IMAGE_SCN_CNT_CODE);

    for (const coff_relocation &reloc : sc->getRelocs()) {
      if (ctx.config.machine == I386 &&
          reloc.Type == COFF::IMAGE_REL_I386_REL32)
        // Ignore relative relocations on x86. On x86_64 they can't be ignored
        // since they're also used to compute absolute addresses.
        continue;

      // Where the object describes its sites, a branch takes no address, and
      // neither does a call or jump through a pointer, which passes the
      // pointer's value nowhere.
      if (described && reloc.Type == IMAGE_REL_AMD64_REL32) {
        std::optional<LinkSiteForm> form =
            file->getLinkSiteForm(sc, reloc.VirtualAddress);
        if (form ? isCallOrJump(*form) : hasPreemptibleTarget(file, sc, reloc))
          continue;
      }

      // An in-place import slot reads no import address table entry, and is
      // listed itself.
      if (sc->getImportSlot(reloc))
        continue;

      Symbol *ref = sc->getImportSiteTarget(reloc);
      if (!ref)
        ref = sc->file->getSymbol(reloc.SymbolTableIndex);
      // An object without guard metadata does not say which import address
      // table entries it passes the value of, so every entry it references is
      // listed.
      markAddressTake(ref, usedSymbols, usedImports);
    }
  }
}

// Whether the delay-load import address table gets a section of its own,
// which the loader keeps read-only. A call through the table is not checked by
// Control Flow Guard, so it must not stay writable. A writable table is a
// target for overwriting whether or not other calls are checked, so
// -import-slots asks for it with or without /guard:cf. mingw-w64's delay-load
// helper stores to the table directly, so MinGW images keep the old layout.
bool Writer::protectDelayIat() {
  return ((ctx.config.guardCF & GuardCFLevel::CF) || ctx.config.importSlots) &&
         !ctx.config.mingw && !delayIdata.empty();
}

// Returns the offset in data of the language handler RVA of the unwind record
// at off, if the record names a handler. A chained record is not followed.
static std::optional<uint32_t>
getUnwindHandlerOffset(bool isArm64, ArrayRef<uint8_t> data, uint32_t off) {
  if (off + 4 > data.size())
    return std::nullopt;
  if (!isArm64) {
    uint8_t flags = data[off] >> 3;
    if (!(flags &
          (Win64EH::UNW_ExceptionHandler | Win64EH::UNW_TerminateHandler)))
      return std::nullopt;
    return off + 4 + alignTo(data[off + 2], 2) * 2;
  }
  uint32_t header = read32le(&data[off]);
  if (!(header & (1u << 20)))
    return std::nullopt;
  uint32_t size = 4;
  uint32_t epilogCount = (header >> 22) & 0x1f;
  uint32_t codeWords = header >> 27;
  if (epilogCount == 0 && codeWords == 0) {
    if (off + 8 > data.size())
      return std::nullopt;
    uint32_t ext = read32le(&data[off + 4]);
    epilogCount = ext & 0xffff;
    codeWords = (ext >> 16) & 0xff;
    size = 8;
  }
  // With the E bit, the only epilog scope is packed into the header.
  if (header & (1u << 21))
    epilogCount = 0;
  return off + size + epilogCount * 4 + codeWords * 4;
}

// Reports an object without EH continuation metadata that has continuation
// targets no table would list, where link.exe fails with LNK2046 or LNK2047:
// an unwind record naming a language handler other than __GSHandlerCheck,
// which only checks the stack cookie, or a reference to _local_unwind.
static void checkEHContMetadata(COFFLinkerContext &ctx, ObjFile *file) {
  MachineTypes machine = file->getMachineType();
  bool isArm64 = isAnyArm64(machine);
  if (machine != AMD64 && !isArm64)
    return;
  // Name the option that asked for the table.
  StringRef option = (ctx.config.importSlots && ctx.config.cetCompat)
                         ? "-cetcompat"
                         : "/guard:ehcont";
  for (Chunk *c : file->getChunks()) {
    auto *sc = dyn_cast<SectionChunk>(c);
    if (!sc || !sc->live)
      continue;
    for (const coff_relocation &rel : sc->getRelocs()) {
      Symbol *sym = file->getSymbol(rel.SymbolTableIndex);
      if (sym && sym->getName() == "_local_unwind") {
        Err(ctx) << option << ": " << file
                 << " has no EH continuation metadata but references "
                    "_local_unwind";
        return;
      }
    }
    if (sc->getSectionName() != ".pdata")
      continue;
    // The object carries no records, so its unwind data is read as link.exe
    // reads it: through the unwind data field of each function table entry.
    uint32_t entrySize = isArm64 ? 8 : 12;
    ArrayRef<uint8_t> pdata = sc->getContents();
    for (const coff_relocation &rel : sc->getRelocs()) {
      if (rel.VirtualAddress % entrySize != entrySize - 4 ||
          rel.VirtualAddress + 4 > pdata.size())
        continue;
      auto *unwind = dyn_cast_or_null<DefinedRegular>(
          file->getSymbol(rel.SymbolTableIndex));
      if (!unwind || !unwind->getChunk())
        continue;
      SectionChunk *xdata = unwind->getChunk();
      std::optional<uint32_t> handlerOff = getUnwindHandlerOffset(
          isArm64, xdata->getContents(),
          unwind->getValue() + read32le(&pdata[rel.VirtualAddress]));
      if (!handlerOff)
        continue;
      for (const coff_relocation &hrel : xdata->getRelocs()) {
        if (hrel.VirtualAddress != *handlerOff)
          continue;
        Symbol *handler = file->getSymbol(hrel.SymbolTableIndex);
        if (handler && handler->getName() != "__GSHandlerCheck") {
          Err(ctx) << option << ": " << file
                   << " has no EH continuation metadata but its unwind data "
                      "names exception handler "
                   << handler->getName();
          return;
        }
      }
    }
  }
}

// Returns the EH continuation targets, which objects compiled with
// /guard:ehcont list in .gehcont$y sections, and reports each object without
// that metadata whose continuation targets would be missing.
SymbolRVASet Writer::getEHContTargets() {
  SymbolRVASet ehContTargets;
  for (ObjFile *file : ctx.objFileInstances) {
    if (file->hasGuardEHCont())
      markSymbolsForRVATable(file, file->getGuardEHContChunks(), ehContTargets);
    else
      checkEHContMetadata(ctx, file);
  }
  return ehContTargets;
}

// Create the guard function id table. This is a table of RVAs of all
// address-taken functions. It is sorted and uniqued, just like the safe SEH
// table.
void Writer::createGuardCFTables() {
  Configuration *config = &ctx.config;

  if (!(config->guardCF & GuardCFLevel::CF)) {
    // MSVC marks the entire image as instrumented if any input object was built
    // with /guard:cf.
    uint32_t guardFlags = 0;
    if (llvm::any_of(ctx.objFileInstances,
                     [](ObjFile *file) { return file->hasGuardCF(); }))
      guardFlags |= uint32_t(GuardFlags::CF_INSTRUMENTED);
    // The EH continuation table can be asked for without /guard:cf.
    if (config->guardCF & GuardCFLevel::EHCont) {
      maybeAddRVATable(getEHContTargets(), "__guard_eh_cont_table",
                       "__guard_eh_cont_count");
      guardFlags |= uint32_t(GuardFlags::EH_CONTINUATION_TABLE_PRESENT);
    }
    if (protectDelayIat())
      guardFlags |= uint32_t(GuardFlags::PROTECT_DELAYLOAD_IAT) |
                    uint32_t(GuardFlags::DELAYLOAD_IAT_IN_ITS_OWN_SECTION);
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
  // What foreign objects list is collected apart first, as foreign code can
  // hand ours any function it lists.
  SymbolRVASet foreignTakenSyms;
  for (ObjFile *file : ctx.objFileInstances) {
    SymbolRVASet &takenSyms =
        kcfiForeignFiles.contains(file) ? foreignTakenSyms : addressTakenSyms;
    // If the object was compiled with /guard:cf, the address taken symbols
    // are in .gfids$y sections, and the longjmp targets are in .gljmp$y
    // sections. If the object was not compiled with /guard:cf, we assume there
    // were no setjmp targets, and that all code symbols with relocations are
    // possibly address-taken.
    if (file->hasGuardCF()) {
      markSymbolsForRVATable(file, file->getGuardFidChunks(), takenSyms);
      std::vector<Symbol *> giats;
      getSymbolsFromSections(file, file->getGuardIATChunks(), giats);
      for (Symbol *s : giats) {
        // The pointer the linker makes for a symbol in the image is no import
        // address table entry; the address it holds is taken.
        if (auto *li = dyn_cast<DefinedLocalImport>(s)) {
          maybeAddAddressTakenFunction(takenSyms, li->getTarget());
          continue;
        }
        addSymbolToRVASet(giatsRVASet, cast<Defined>(s));
        giatsSymbols.push_back(s);
      }
      markSymbolsForRVATable(file, file->getGuardLJmpChunks(), longJmpTargets);
      if (file->describesSites)
        markDescribedAddressTakes(file, takenSyms, giatsRVASet);
    } else {
      markSymbolsWithRelocations(file, takenSyms, giatsRVASet);
    }
  }
  addressTakenSyms.insert(foreignTakenSyms.begin(), foreignTakenSyms.end());

  // An in-place import slot that holds a function's address is an entry the
  // loader fills with an address the image passes on.
  for (auto &kv : ctx.importSlots)
    for (const ImportSlot &s : kv.second)
      if (s.sym->file->thunkSym)
        giatsRVASet.insert({s.chunk, s.offset});

  // Mark the image entry as address-taken.
  SymbolRVASet exportedSyms;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    if (symtab.entry)
      maybeAddAddressTakenFunction(addressTakenSyms, symtab.entry);

    // Mark exported symbols in executable sections as address-taken.
    for (Export &e : symtab.exports)
      maybeAddAddressTakenFunction(exportedSyms, e.sym);
  });

  // For each entry in the .giats table, check if it has a corresponding load
  // thunk (e.g. because the DLL that defines it will be delay-loaded) and, if
  // so, add the load thunk to the address taken (.gfids) table.
  for (Symbol *s : giatsSymbols) {
    if (auto *di = dyn_cast<DefinedImportData>(s)) {
      if (di->loadThunkSym)
        addSymbolToRVASet(addressTakenSyms, di->loadThunkSym);
    }
  }

  // An exported function that is a target for no other reason is marked
  // export-suppressed: in a process that suppresses exports, it becomes a valid
  // target only once GetProcAddress returns it. The loader can record that
  // only for a 16-byte aligned entry.
  SymbolRVASet exportSuppressed;
  for (const ChunkAndOffset &c : exportedSyms)
    if (addressTakenSyms.insert(c).second && c.offset % 16 == 0)
      exportSuppressed.insert(c);

  // Under -import-slots, the image is sealed: a function the table omits has
  // its KCFI type overwritten, so that a KCFI check never accepts a function
  // that Control Flow Guard would reject. Identical code folding can make one
  // chunk the definition of several functions, and the table and the prefixes
  // are both keyed by the chunk that remains, so a folded function stays
  // unsealed if any function folded into it is listed.
  //
  // A function without a prefix that a foreign object lists, such as one of
  // its own or an import thunk, is one that foreign code can hand to ours.
  if (config->importSlots) {
    for (KCFIPrefix &p : kcfiPrefixes)
      p.sealed = !addressTakenSyms.contains({p.chunk, p.entry});
    kcfiSealed = true;
    kcfiUnprefixedTargets =
        llvm::any_of(foreignTakenSyms, [&](const ChunkAndOffset &c) {
          auto it = kcfiEntries.find(c.inputChunk);
          return it == kcfiEntries.end() ||
                 llvm::none_of(it->second,
                               [&](auto &e) { return e.first == c.offset; });
        });
  }

  // Ensure sections referenced in the gfid table are 16-byte aligned.
  for (const ChunkAndOffset &c : addressTakenSyms)
    if (c.inputChunk->getAlignment() < 16)
      c.inputChunk->setAlignment(16);

  // Every table has a flag byte after each RVA if an entry of any of them has
  // a flag, as link.exe writes them, and none otherwise.
  bool hasFlag = !exportSuppressed.empty();

  maybeAddRVATable(std::move(addressTakenSyms), "__guard_fids_table",
                   "__guard_fids_count", hasFlag, std::move(exportSuppressed));

  // Add the Guard Address Taken IAT Entry Table (.giats).
  maybeAddRVATable(std::move(giatsRVASet), "__guard_iat_table",
                   "__guard_iat_count", hasFlag);

  // Add the longjmp target table unless the user told us not to.
  if (config->guardCF & GuardCFLevel::LongJmp)
    maybeAddRVATable(std::move(longJmpTargets), "__guard_longjmp_table",
                     "__guard_longjmp_count", hasFlag);

  // Add the ehcont target table unless the user told us not to.
  if (config->guardCF & GuardCFLevel::EHCont)
    maybeAddRVATable(getEHContTargets(), "__guard_eh_cont_table",
                     "__guard_eh_cont_count", hasFlag);

  // Set __guard_flags, which will be used in the load config to indicate that
  // /guard:cf was enabled.
  uint32_t guardFlags = uint32_t(GuardFlags::CF_INSTRUMENTED) |
                        uint32_t(GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  if (hasFlag)
    guardFlags |= uint32_t(GuardFlags::CF_FUNCTION_TABLE_SIZE_5BYTES);
  if (config->guardCF & GuardCFLevel::LongJmp)
    guardFlags |= uint32_t(GuardFlags::CF_LONGJUMP_TABLE_PRESENT);
  if (config->guardCF & GuardCFLevel::EHCont)
    guardFlags |= uint32_t(GuardFlags::EH_CONTINUATION_TABLE_PRESENT);
  if (config->guardCF & GuardCFLevel::ExportSuppress)
    guardFlags |= uint32_t(GuardFlags::CF_ENABLE_EXPORT_SUPPRESSION);
  // The loader resolves a protected table's imports in a buffer and copies
  // them in under one reprotection, restoring read-only whether or not the
  // section was protected at load, so the two flags go together.
  if (protectDelayIat())
    guardFlags |= uint32_t(GuardFlags::PROTECT_DELAYLOAD_IAT) |
                  uint32_t(GuardFlags::DELAYLOAD_IAT_IN_ITS_OWN_SECTION);
  // The export-suppressed marks and the address-taken IAT table are complete,
  // but the loader can rely on that only where the load configuration reaches
  // the table.
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

// Finds the KCFI prefix with a marker of every function the link keeps, which
// clang labels with a static __cfi_ symbol, keyed by the chunk that remains
// after identical code folding, and the foreign objects, which have none.
void Writer::findKCFIPrefixes() {
  DenseSet<std::pair<SectionChunk *, uint32_t>> seen;
  for (ObjFile *file : ctx.objFileInstances) {
    SmallVector<DefinedRegular *, 0> labels, entries;
    for (Symbol *s : file->getSymbols()) {
      auto *d = dyn_cast_or_null<DefinedRegular>(s);
      if (!d || d->file != file)
        continue;
      SectionChunk *sc = d->getChunk();
      if (!sc || !sc->live ||
          !(sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
        continue;
      if (d->getCOFFSymbol().getComplexType() == IMAGE_SYM_DTYPE_FUNCTION)
        entries.push_back(d);
      else if (!d->getCOFFSymbol().isExternal() &&
               d->getName().starts_with("__cfi_"))
        labels.push_back(d);
    }
    // Only an object without a live prefix with the marker can be foreign.
    bool hasPrefix = false;
    if (labels.empty()) {
      if (isKCFIForeignFile(file))
        kcfiForeignFiles.insert(file);
      continue;
    }

    // The function a prefix belongs to is the first that follows it in its
    // chunk.
    auto byLocation = [](DefinedRegular *a, DefinedRegular *b) {
      return std::make_pair(a->getChunk(), a->getValue()) <
             std::make_pair(b->getChunk(), b->getValue());
    };
    llvm::sort(entries, byLocation);
    for (DefinedRegular *label : labels) {
      SectionChunk *sc = label->getChunk();
      uint32_t off = label->getValue();
      auto it = llvm::upper_bound(entries, label, byLocation);
      // A prefix without the marker, such as upstream KCFI's, can never pass
      // the thunks' check, so there is nothing to seal. One with the marker
      // that no function follows is malformed.
      uint32_t size = getKCFIPrefixSize(sc, off);
      if (size == 0)
        continue;
      hasPrefix = true;
      if (it == entries.end() || (*it)->getChunk() != sc ||
          (*it)->getValue() < off + size) {
        Err(ctx) << file << ": no function follows the KCFI prefix at "
                 << label->getName();
        continue;
      }
      if (!seen.insert({sc, off}).second)
        continue;
      // A check outside the code range reads up to 12 bytes before the end of
      // the marker pattern, or 16 with a second type word, which a patchable
      // prefix moves further from the entry, and reads nothing before a
      // target in the page's first PowerOf2Ceil of that many bytes.
      uint32_t entry = (*it)->getValue();
      uint32_t patchable = entry - off - size;
      kcfiPrefixes.push_back({sc, off, size, entry});
      kcfiEntries[sc].push_back({entry, PowerOf2Ceil(patchable + size)});
    }
    if (!hasPrefix && isKCFIForeignFile(file))
      kcfiForeignFiles.insert(file);
  }
}

// Overwrites the type words of each sealed KCFI prefix with the type that no
// call expects.
void Writer::sealKCFIPrefixes() {
  uint8_t *buf = buffer->getBufferStart();
  for (const KCFIPrefix &p : kcfiPrefixes) {
    if (!p.sealed)
      continue;
    OutputSection *sec = ctx.getOutputSection(p.chunk);
    uint8_t *loc =
        buf + sec->getFileOff() + p.chunk->getRVA() - sec->getRVA() + p.offset;
    if (p.size == 16)
      write32le(loc, COFF::KCFISealedType);
    write32le(loc + p.size - 4, COFF::KCFISealedType);
  }
}

// Defines __llvm_code_start and __llvm_code_end, which clang's KCFI thunks test
// to take a target inside the image directly, as the bounds of the output
// section that holds the KCFI prefixes of a sealed image. They keep clang's
// weak default, __llvm_code_empty, a byte in a COMDAT, and so an empty range,
// unless every prefix is in one output section.
// Places the symbols that SymbolTable::addStartStopSymbols and
// addBoundarySymbols defined: __start_X at the first section of run X and
// __stop_X at the end of its last, and _etext, _edata and _end at the ends of
// .text, of .data's initialized data and of .data.
void Writer::placeLinkerDefinedSymbols() {
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    for (SymbolTable::SectionRun &run : symtab.sectionRuns) {
      DenseSet<Chunk *> live;
      OutputSection *sec = nullptr;
      bool split = false;
      for (SectionChunk *c : run.chunks) {
        if (!c->live)
          continue;
        OutputSection *s = ctx.getOutputSection(c);
        split |= sec && s != sec;
        sec = s;
        live.insert(c);
      }
      // Only dead code refers to the bounds of a run with no live section.
      if (live.empty())
        continue;
      if (split) {
        Err(ctx) << "section " << run.name
                 << " is split across output sections and cannot be bounded "
                    "by __start_"
                 << run.name << " and __stop_" << run.name;
        continue;
      }
      // The run's sections make up the output section named X, which merging
      // appends whole, so only range extension thunks can fall between them.
      auto isLive = [&](Chunk *c) { return live.contains(c); };
      Chunk *firstChunk = *llvm::find_if(sec->chunks, isLive);
      Chunk *lastChunk = *llvm::find_if(llvm::reverse(sec->chunks), isLive);
      if (run.start)
        replaceSymbol<DefinedSynthetic>(run.start, run.start->getName(),
                                        firstChunk);
      if (run.stop)
        replaceSymbol<DefinedSynthetic>(run.stop, run.stop->getName(),
                                        lastChunk, lastChunk->getSize());
    }

    for (Symbol *s : symtab.boundarySymbols) {
      StringRef name = s->getName();
      if (ctx.config.machine == I386)
        name = name.drop_front();
      name.consume_front("_");
      StringRef secName = name == "etext" ? ".text" : ".data";
      OutputSection *sec = findSection(secName);
      if (!sec || sec->chunks.empty()) {
        Err(ctx) << s->getName() << " is referenced, but the image has no "
                 << secName << " section";
        continue;
      }
      // _edata ends the initialized data, which precedes the uninitialized.
      Chunk *c = sec->chunks.back();
      if (name == "edata") {
        auto it = llvm::find_if(llvm::reverse(sec->chunks),
                                [](Chunk *c) { return c->hasData; });
        if (it == sec->chunks.rend()) {
          replaceSymbol<DefinedSynthetic>(s, s->getName(), sec->chunks[0]);
          continue;
        }
        c = *it;
      }
      replaceSymbol<DefinedSynthetic>(s, s->getName(), c, c->getSize());
    }
  });
}

void Writer::defineKCFICodeRange() {
  if (!kcfiSealed || kcfiPrefixes.empty())
    return;
  OutputSection *sec = ctx.getOutputSection(kcfiPrefixes.front().chunk);
  for (const KCFIPrefix &p : kcfiPrefixes)
    if (ctx.getOutputSection(p.chunk) != sec)
      return;
  // Each bound is replaced only while it is the weak alias resolved to that
  // default, the leader of its COMDAT.
  auto isEmptyDefault = [](Symbol *s) {
    auto *d = dyn_cast_or_null<DefinedRegular>(s);
    return d && d->getValue() == 0 && d->getChunk()->sym &&
           d->getChunk()->sym->getName() == "__llvm_code_empty";
  };
  Symbol *start = ctx.symtab.find("__llvm_code_start");
  Symbol *end = ctx.symtab.find("__llvm_code_end");
  if (!isEmptyDefault(start) || !isEmptyDefault(end))
    return;
  Chunk *last = sec->chunks.back();
  replaceSymbol<DefinedSynthetic>(start, start->getName(), sec->chunks.front());
  replaceSymbol<DefinedSynthetic>(end, end->getName(), last, last->getSize());
  kcfiCodeSec = sec;
}

// In an image whose guard function table lists a function without a KCFI
// prefix, foreign code linked into the image can hand ours one of its
// functions through a channel that no type rule sees. Where a mismatch would
// fail fast, at the routine of a closed type or at the end of a statically
// open type's list, a target in the image then goes to the dynamic scanner
// instead: the scanner fails fast on a target with our marker, and the guard
// function it continues into accepts only what this image's table lists. A
// target outside the image fails fast as before. A closed type's routine
// points the scanner at an empty list, and the static scanner is entered
// through a routine that gives it only targets outside the image, and the
// dynamic scanner the rest, with the same list. The scanners are clang's, as
// only clang knows the marker and the prefix offset it compares.
void Writer::boundKCFIMismatches() {
  bool isX64 = ctx.config.machine == AMD64;
  if (!kcfiUnprefixedTargets || (!isX64 && ctx.config.machine != ARM64))
    return;
  struct Kind {
    StringRef mismatch, staticScanner, dynamicScanner;
    bool check;
  };
  static const Kind kinds[] = {{"__llvm_kcfi_mismatch_", "__llvm_kcfi_open",
                                "__llvm_kcfi_open_dynamic", false},
                               {"__llvm_kcfi_check_mismatch_",
                                "__llvm_kcfi_check_open",
                                "__llvm_kcfi_check_open_dynamic", true}};
  auto *trap =
      dyn_cast_or_null<DefinedRegular>(ctx.symtab.find("__llvm_kcfi_trap"));
  // Foreign code that references no import can hand ours only functions in
  // the image, which the bound accepts, so a type that only such code opened
  // need not be open dynamically. Foreign code that references one can hand
  // on pointers that it obtained from any DLL at run time.
  bool narrow = !ctx.symtab.kcfiLocalRoutines.empty() &&
                llvm::none_of(kcfiForeignFiles, [](ObjFile *file) {
                  return llvm::any_of(file->getSymbols(), [](Symbol *s) {
                    return isa_and_nonnull<DefinedImportData,
                                           DefinedImportThunk>(s);
                  });
                });
  Defined *empty = nullptr;
  for (const Kind &k : ArrayRef(kinds).drop_front(isX64 ? 0 : 1)) {
    auto *dynamic =
        dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(k.dynamicScanner));
    if (!dynamic)
      continue;
    for (auto [routine, staticScanner] : ctx.symtab.kcfiLocalRoutines) {
      auto *s = dyn_cast<DefinedRegular>(staticScanner);
      if (!narrow || !s || s->getName() != k.staticScanner)
        continue;
      routine->setStatic(s);
      if (!s->getChunk()->live) {
        s->getChunk()->live = true;
        textSec->addChunk(s->getChunk());
      }
    }
    KCFIOpenChunk *closed = nullptr, *open = nullptr;
    // A closed type's routine is the trap, to which the weak default was
    // resolved.
    if (trap && trap->isLive()) {
      ctx.symtab.forEachSymbol([&](Symbol *s) {
        auto *d = dyn_cast<DefinedRegular>(s);
        if (!d || d->getChunk() != trap->getChunk() ||
            d->getValue() != trap->getValue() ||
            !d->getName().starts_with(k.mismatch))
          return;
        if (!closed) {
          // The head, then the odd word that ends the list.
          if (!empty) {
            auto *head = make<KCFIListChunk>(ctx, ".rdata", nullptr);
            rdataSec->addChunk(head);
            rdataSec->addChunk(make<KCFIListChunk>(ctx, ".rdata", nullptr, 1));
            empty = make<DefinedSynthetic>("__llvm_kcfi_list_empty", head);
          }
          closed =
              make<KCFIOpenChunk>(ctx, empty, dynamic, false, trap, k.check);
          textSec->addChunk(closed);
        }
        replaceSymbol<DefinedSynthetic>(s, s->getName(), closed);
      });
    }
    // Every routine that jumps to the static scanner does so through its
    // symbol.
    auto *scanner =
        dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(k.staticScanner));
    if (scanner && scanner->isLive()) {
      auto *outside = make<DefinedSynthetic>(
          scanner->getName(), scanner->getChunk(), scanner->getValue());
      open =
          make<KCFIOpenChunk>(ctx, nullptr, dynamic, false, outside, k.check);
      textSec->addChunk(open);
      replaceSymbol<DefinedSynthetic>(scanner, scanner->getName(), open);
    }
    // The garbage collector left out the dynamic scanner if nothing referred
    // to it before these routines. It refers only to the guard function,
    // which every KCFI thunk refers to.
    SectionChunk *sc = dynamic->getChunk();
    if ((closed || open) && !sc->live) {
      sc->live = true;
      textSec->addChunk(sc);
    }
  }
}

// Rewrites, in place, the range test at the start of each of clang's KCFI
// thunks the image keeps, __llvm_kcfi_dispatch_<type> with the target in RAX
// and __llvm_kcfi_check_<type> with it in RCX or X15, once the code range is
// defined. It becomes one comparison of the target's offset from the start of
// the range with its size, which the linker knows. A thunk for a type that no
// unsealed prefix in the image has cannot match a target in the image, so it
// goes straight to the page test and the guard function. The type checks are
// kept as clang wrote them, since only clang knows the marker, the prefix
// offset and the form of the type they compare, and so are the page test and
// the branches, with their relocations. The new range test is never longer
// than clang's, and the rest is filled with int3 or brk #0xf000.
void Writer::rewriteKCFIThunks() {
  if (!kcfiCodeSec)
    return;
  bool isX64 = ctx.config.machine == AMD64;
  uint32_t codeStart = kcfiCodeSec->chunks.front()->getRVA();
  Chunk *last = kcfiCodeSec->chunks.back();
  uint64_t codeSize = last->getRVA() + last->getSize() - codeStart;
  if ((!isX64 && ctx.config.machine != ARM64) || codeSize > INT32_MAX)
    return;

  DenseSet<uint32_t> types;
  for (const KCFIPrefix &p : kcfiPrefixes)
    if (!p.sealed)
      types.insert(
          read32le(p.chunk->getContents().data() + p.offset + p.size - 4));

  uint8_t *buf = buffer->getBufferStart();
  for (ObjFile *file : ctx.objFileInstances) {
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live || !sc->sym)
        continue;
      StringRef name = sc->sym->getName();
      bool dispatch = name.consume_front("__llvm_kcfi_dispatch_");
      uint32_t type;
      if ((!dispatch && !name.consume_front("__llvm_kcfi_check_")) ||
          name.size() != 8 || name.getAsInteger(16, type))
        continue;

      // Only clang's form is rewritten, which the chunk must match exactly:
      // its size, every relocation with its type and target, every byte but
      // those that vary with the type and the options, and two identical type
      // checks. Anything else, such as a thunk of another compiler or
      // version, is legitimate and left as it is.
      if (dispatch && !isX64)
        continue;
      StringRef mismatchPrefix =
          dispatch ? "__llvm_kcfi_mismatch_" : "__llvm_kcfi_check_mismatch_";
      std::string mismatch = (mismatchPrefix + name).str();
      StringRef guardName =
          dispatch ? "__guard_dispatch_icall_fptr" : "__guard_check_icall_fptr";
      StringRef targets[] = {mismatch, "__llvm_code_start", "__llvm_code_end",
                             guardName};
      ArrayRef<KCFIThunkReloc> expectedRelocs =
          isX64 ? ArrayRef(kcfiRelocsX64) : ArrayRef(kcfiCheckRelocsARM64);
      ArrayRef<uint8_t> contents = sc->getContents();
      ArrayRef<coff_relocation> relocs = sc->getRelocs();
      bool matches = relocs.size() == expectedRelocs.size();
      if (isX64) {
        ArrayRef<int16_t> form =
            dispatch ? ArrayRef(kcfiDispatchX64) : ArrayRef(kcfiCheckX64);
        matches &= contents.size() == form.size() &&
                   contents.slice(24, 14) == contents.slice(57, 14);
        for (size_t i = 0; matches && i != form.size(); ++i)
          matches = form[i] < 0 || contents[i] == form[i];
      } else {
        matches &= contents.size() == std::size(kcfiCheckARM64) * 4 &&
                   contents.slice(32, 24) == contents.slice(72, 24);
        for (size_t i = 0; matches && i != std::size(kcfiCheckARM64); ++i)
          matches = (read32le(contents.data() + i * 4) &
                     ~kcfiCheckARM64[i].varying) == kcfiCheckARM64[i].insn;
      }
      for (size_t i = 0; matches && i != relocs.size(); ++i) {
        const coff_relocation &r = relocs[i];
        Symbol *target = sc->file->getSymbol(r.SymbolTableIndex);
        matches = r.VirtualAddress == expectedRelocs[i].offset &&
                  r.Type == expectedRelocs[i].type && target &&
                  target->getName() == targets[expectedRelocs[i].target];
      }
      if (!matches)
        continue;

      // Only the range test, before the first type check, is replaced. The
      // label 1: of the page test follows the first type check.
      OutputSection *sec = ctx.getOutputSection(sc);
      uint8_t *loc = buf + sec->getFileOff() + sc->getRVA() - sec->getRVA();
      uint64_t rva = sc->getRVA();
      bool testRange = types.contains(type);
      if (isX64) {
        // lea r10, [rip + start]; mov r11, rax (or rcx); sub r11, r10
        // cmp r11, size; jae 1f; nop
        //
        // or, with no unsealed function of the type, jmp 1f.
        uint8_t outside = dispatch ? 46 : 45;
        memset(loc, 0xCC, 24);
        if (testRange) {
          static const uint8_t rangeTest[] = {
              0x4C, 0x8D, 0x15, 0, 0, 0, 0, // lea r10, [rip + start]
              0x49, 0x89, 0xC3,             // mov r11, rax
              0x4D, 0x29, 0xD3,             // sub r11, r10
              0x49, 0x81, 0xFB, 0, 0, 0, 0, // cmp r11, size
              0x73, 0,                      // jae 1f
              0x66, 0x90,                   // nop
          };
          memcpy(loc, rangeTest, sizeof(rangeTest));
          write32le(loc + 3, codeStart - (rva + 7));
          if (!dispatch)
            loc[9] = 0xCB; // mov r11, rcx
          write32le(loc + 16, codeSize);
          loc[21] = outside - 22;
        } else {
          loc[0] = 0xEB; // jmp 1f
          loc[1] = outside - 2;
        }
      } else {
        // adrp x16, start; add x16, x16, :lo12:start; sub x16, x15, x16
        // movz x17, #size; movk x17, #size, lsl #16; cmp x16, x17; b.hs 1f
        // nop
        //
        // or, with no unsealed function of the type, b 1f.
        for (size_t i = 0; i != 32; i += 4)
          write32le(loc + i, 0xD43E0000); // brk #0xf000
        if (testRange) {
          write32le(loc, 0x90000010);     // adrp x16, start
          write32le(loc + 4, 0x91000210); // add x16, x16, :lo12:start
          write32le(loc + 8, 0xCB1001F0); // sub x16, x15, x16
          write32le(loc + 12, 0xD2800011 | (codeSize & 0xFFFF) << 5);
          write32le(loc + 16, 0xF2A00011 | (codeSize >> 16) << 5);
          write32le(loc + 20, 0xEB11021F); // cmp x16, x17
          write32le(loc + 24, 0x54000142); // b.hs 1f
          write32le(loc + 28, 0xD503201F); // nop
          applyArm64Addr(loc, codeStart, rva, 12);
          applyArm64Imm(loc + 4, codeStart & 0xfff, 0);
        } else {
          write32le(loc, 0x14000010); // b 1f
        }
      }
    }
  }
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
      if (Symbol *s = objSymbols[symIndex]) {
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
  if (tableSymbols.empty())
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
  if (!ctx.symtab.isEC())
    return;

  for (Symbol *s : ctx.symtab.expSymbols) {
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
      Symbol *targetSym = ctx.symtab.find((targetName + "$hp_target").str());
      if (!targetSym)
        targetSym = ctx.symtab.find(targetName);
      Defined *t = dyn_cast_or_null<Defined>(targetSym);
      if (t && isArm64EC(t->getChunk()->getMachine()))
        exportThunks.push_back({chunk, t});
    }
  }

  auto codeMapChunk = make<ECCodeMapChunk>(codeMap);
  rdataSec->addChunk(codeMapChunk);
  Symbol *codeMapSym = ctx.symtab.findUnderscore("__hybrid_code_map");
  replaceSymbol<DefinedSynthetic>(codeMapSym, codeMapSym->getName(),
                                  codeMapChunk);

  CHPECodeRangesChunk *ranges = make<CHPECodeRangesChunk>(exportThunks);
  rdataSec->addChunk(ranges);
  Symbol *rangesSym =
      ctx.symtab.findUnderscore("__x64_code_ranges_to_entry_points");
  replaceSymbol<DefinedSynthetic>(rangesSym, rangesSym->getName(), ranges);

  CHPERedirectionChunk *entryPoints = make<CHPERedirectionChunk>(exportThunks);
  a64xrmSec->addChunk(entryPoints);
  Symbol *entryPointsSym =
      ctx.symtab.findUnderscore("__arm64x_redirection_metadata");
  replaceSymbol<DefinedSynthetic>(entryPointsSym, entryPointsSym->getName(),
                                  entryPoints);

  for (auto thunk : ctx.symtab.sameAddressThunks) {
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
      if (!sc || !sc->live || &sc->file->symtab != &symtab)
        continue;
      // Don't create pseudo relocations for sections that won't be
      // mapped at runtime.
      if (sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
        continue;
      sc->getRuntimePseudoRelocs(rels);
    }

    if (!ctx.config.pseudoRelocs) {
      // Not writing any pseudo relocs; if some were needed, error out and
      // indicate what required them.
      for (const RuntimePseudoReloc &rpr : rels)
        Err(ctx) << "automatic dllimport of " << rpr.sym->getName() << " in "
                 << toString(rpr.target->file)
                 << " requires pseudo relocations";
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
    Symbol *dataStartSym = ctx.symtab.find("__data_start__");
    Symbol *dataEndSym = ctx.symtab.find("__data_end__");
    Chunk *endChunk = dataSec->chunks.back();
    replaceSymbol<DefinedSynthetic>(dataStartSym, dataStartSym->getName(),
                                    dataSec->chunks.front());
    replaceSymbol<DefinedSynthetic>(dataEndSym, dataEndSym->getName(), endChunk,
                                    endChunk->getSize());
  }

  if (!bssSec->chunks.empty()) {
    Symbol *bssStartSym = ctx.symtab.find("__bss_start__");
    Symbol *bssEndSym = ctx.symtab.find("__bss_end__");
    Chunk *endChunk = bssSec->chunks.back();
    replaceSymbol<DefinedSynthetic>(bssStartSym, bssStartSym->getName(),
                                    bssSec->chunks.front());
    replaceSymbol<DefinedSynthetic>(bssEndSym, bssEndSym->getName(), endChunk,
                                    endChunk->getSize());
  }
}

// Handles /section options to allow users to overwrite
// section attributes.
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
  if (!ctx.symtab.isEC())
    return;

  llvm::stable_sort(exportThunks, [](const std::pair<Chunk *, Defined *> &a,
                                     const std::pair<Chunk *, Defined *> &b) {
    return a.first->getRVA() < b.first->getRVA();
  });

  ChunkRange &chpePdata = ctx.config.machine == ARM64X ? hybridPdata : pdata;
  Symbol *rfeTableSym = ctx.symtab.findUnderscore("__arm64x_extra_rfe_table");
  replaceSymbol<DefinedSynthetic>(rfeTableSym, "__arm64x_extra_rfe_table",
                                  chpePdata.first);

  if (chpePdata.first) {
    Symbol *rfeSizeSym =
        ctx.symtab.findUnderscore("__arm64x_extra_rfe_table_size");
    cast<DefinedAbsolute>(rfeSizeSym)
        ->setVA(chpePdata.last->getRVA() + chpePdata.last->getSize() -
                chpePdata.first->getRVA());
  }

  Symbol *rangesCountSym =
      ctx.symtab.findUnderscore("__x64_code_ranges_to_entry_points_count");
  cast<DefinedAbsolute>(rangesCountSym)->setVA(exportThunks.size());

  Symbol *entryPointCountSym =
      ctx.symtab.findUnderscore("__arm64x_redirection_metadata_count");
  cast<DefinedAbsolute>(entryPointCountSym)->setVA(exportThunks.size());

  Symbol *iatSym = ctx.symtab.findUnderscore("__hybrid_auxiliary_iat");
  replaceSymbol<DefinedSynthetic>(iatSym, "__hybrid_auxiliary_iat",
                                  idata.auxIat.empty() ? nullptr
                                                       : idata.auxIat.front());

  Symbol *iatCopySym = ctx.symtab.findUnderscore("__hybrid_auxiliary_iat_copy");
  replaceSymbol<DefinedSynthetic>(
      iatCopySym, "__hybrid_auxiliary_iat_copy",
      idata.auxIatCopy.empty() ? nullptr : idata.auxIatCopy.front());

  Symbol *delayIatSym =
      ctx.symtab.findUnderscore("__hybrid_auxiliary_delayload_iat");
  replaceSymbol<DefinedSynthetic>(
      delayIatSym, "__hybrid_auxiliary_delayload_iat",
      delayIdata.getAuxIat().empty() ? nullptr
                                     : delayIdata.getAuxIat().front());

  Symbol *delayIatCopySym =
      ctx.symtab.findUnderscore("__hybrid_auxiliary_delayload_iat_copy");
  replaceSymbol<DefinedSynthetic>(
      delayIatCopySym, "__hybrid_auxiliary_delayload_iat_copy",
      delayIdata.getAuxIatCopy().empty() ? nullptr
                                         : delayIdata.getAuxIatCopy().front());

  if (ctx.config.machine == ARM64X) {
    // For the hybrid image, set the alternate entry point to the EC entry
    // point. In the hybrid view, it is swapped to the native entry point
    // using ARM64X relocations.
    if (auto altEntrySym = cast_or_null<Defined>(ctx.symtab.entry)) {
      // If the entry is an EC export thunk, use its target instead.
      if (auto thunkChunk =
              dyn_cast<ECExportThunkChunk>(altEntrySym->getChunk()))
        altEntrySym = thunkChunk->target;
      ctx.symtab.findUnderscore("__arm64x_native_entrypoint")
          ->replaceKeepingName(altEntrySym, sizeof(SymbolUnion));
    }

    if (ctx.symtab.edataStart)
      ctx.dynamicRelocs->set(
          dataDirOffset64 + EXPORT_TABLE * sizeof(data_directory) +
              offsetof(data_directory, Size),
          ctx.symtab.edataEnd->getRVA() - ctx.symtab.edataStart->getRVA() +
              ctx.symtab.edataEnd->getSize());
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

  for (SameAddressThunkARM64EC *thunk : ctx.symtab.sameAddressThunks)
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

  if (ctx.symtab.entry != ctx.hybridSymtab->entry ||
      pdata.first != hybridPdata.first) {
    chpeSym = cast_or_null<DefinedRegular>(
        ctx.symtab.findUnderscore("__chpe_metadata"));
    if (!chpeSym)
      Warn(ctx) << "'__chpe_metadata' is missing for ARM64X target";
  }

  if (ctx.symtab.entry != ctx.hybridSymtab->entry) {
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           peHeaderOffset +
                               offsetof(pe32plus_header, AddressOfEntryPoint),
                           cast_or_null<Defined>(ctx.symtab.entry));

    // Swap the alternate entry point in the CHPE metadata.
    if (chpeSym)
      ctx.dynamicRelocs->add(
          IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
          Arm64XRelocVal(chpeSym, offsetof(chpe_metadata, AlternateEntryPoint)),
          cast_or_null<Defined>(ctx.hybridSymtab->entry));
  }

  if (ctx.symtab.edataStart != ctx.hybridSymtab->edataStart) {
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           dataDirOffset64 +
                               EXPORT_TABLE * sizeof(data_directory) +
                               offsetof(data_directory, RelativeVirtualAddress),
                           ctx.symtab.edataStart);
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
                         ctx.symtab.loadConfigSym);
  ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                         dataDirOffset64 +
                             LOAD_CONFIG_TABLE * sizeof(data_directory) +
                             offsetof(data_directory, Size),
                         ctx.symtab.loadConfigSize);

  auto nativeTlsUsed =
      dyn_cast_or_null<Defined>(ctx.hybridSymtab->findUnderscore("_tls_used"));
  auto ecTlsUsed =
      dyn_cast_or_null<Defined>(ctx.symtab.findUnderscore("_tls_used"));
  if (nativeTlsUsed || ecTlsUsed) {
    ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
                           dataDirOffset64 +
                               TLS_TABLE * sizeof(data_directory) +
                               offsetof(data_directory, RelativeVirtualAddress),
                           Arm64XRelocVal(ecTlsUsed));
    if (!nativeTlsUsed || !ecTlsUsed)
      ctx.dynamicRelocs->add(
          IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE, sizeof(uint32_t),
          dataDirOffset64 + TLS_TABLE * sizeof(data_directory) +
              offsetof(data_directory, Size),
          ecTlsUsed ? sizeof(coff_tls_directory64) : 0);
  }
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

void Writer::fixTlsAlignment(SymbolTable &symtab) {
  Defined *tlsSym =
      dyn_cast_or_null<Defined>(symtab.findUnderscore("_tls_used"));
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
        ctx.symtab.loadConfigSize >=
            offsetof(T, CHPEMetadataPointer) + sizeof(T::CHPEMetadataPointer)) {
      OutputSection *sec =
          ctx.getOutputSection(ctx.symtab.loadConfigSym->getChunk());
      uint8_t *secBuf = buffer->getBufferStart() + sec->getFileOff();
      auto hybridLoadConfig =
          reinterpret_cast<const coff_load_configuration64 *>(
              secBuf + (ctx.symtab.loadConfigSym->getRVA() - sec->getRVA()));
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
