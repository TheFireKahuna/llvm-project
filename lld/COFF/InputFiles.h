//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INPUT_FILES_H
#define LLD_COFF_INPUT_FILES_H

#include "Config.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/StringSaver.h"
#include <memory>
#include <set>
#include <vector>

namespace llvm {
struct DILineInfo;
struct NewArchiveMember;
namespace pdb {
class DbiModuleDescriptorBuilder;
class NativeSession;
}
namespace lto {
class InputFile;
}
}

namespace lld {
class DWARFCache;

namespace coff {
class COFFLinkerContext;

const COFFSyncStream &operator<<(const COFFSyncStream &, const InputFile *);

std::vector<MemoryBufferRef> getArchiveMembers(COFFLinkerContext &,
                                               llvm::object::Archive *file);

using llvm::COFF::IMAGE_FILE_MACHINE_UNKNOWN;
using llvm::COFF::MachineTypes;
using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_import_header;
using llvm::object::coff_relocation;
using llvm::object::coff_section;

class Chunk;
class Defined;
class DefinedImportData;
class DefinedImportThunk;
class DefinedRegular;
class ImportThunkChunk;
class ImportThunkChunkARM64EC;
class SectionChunk;
class Symbol;
class SymbolTable;
class Undefined;
class TpiSource;

// The root class of input files.
class InputFile {
public:
  enum Kind {
    ArchiveKind,
    ObjectKind,
    PDBKind,
    ImportKind,
    BitcodeKind,
    DLLKind
  };
  Kind kind() const { return fileKind; }
  virtual ~InputFile() {}

  // Returns the filename.
  StringRef getName() const { return mb.getBufferIdentifier(); }

  // Reads a file (the constructor doesn't do that).
  virtual void parse() = 0;

  // Returns the CPU type this file was compiled to.
  virtual MachineTypes getMachineType() const {
    return IMAGE_FILE_MACHINE_UNKNOWN;
  }

  MemoryBufferRef mb;

  // An archive file name if this file is created from an archive.
  StringRef parentName;

  // Returns .drectve section contents if exist.
  StringRef getDirectives() { return directives; }

  SymbolTable &symtab;

protected:
  InputFile(SymbolTable &s, Kind k, MemoryBufferRef m, bool lazy = false)
      : mb(m), symtab(s), fileKind(k), lazy(lazy) {}

  StringRef directives;

private:
  const Kind fileKind;

public:
  // True if this is a lazy ObjFile or BitcodeFile.
  bool lazy = false;
};

// An archive's symbol table indexed by name: the first symbol with each name,
// in a hash table by its position. The archive's reader thread builds it.
class ArchiveIndex {
public:
  // Creates the archive in mb and indexes it, or leaves the index unbuilt if
  // mb is not a valid archive.
  void build(MemoryBufferRef mb);
  void build(const Archive &a);
  bool isBuilt() const { return archive; }
  // Leaves the first symbol named as the one at position i out.
  void remove(uint32_t i);

  std::optional<uint32_t> find(llvm::CachedHashStringRef name) const;
  Archive::Symbol getSymbol(uint32_t i) const;
  StringRef getSymbolName(uint32_t i) const;
  std::vector<uint32_t> getIndexedSymbols() const;
  std::vector<StringRef> getSymbolNames(StringRef prefix) const;
  size_t getNumIndexed() const { return numIndexed; }

  // The archive that build(mb) created, until its reader takes it.
  std::unique_ptr<Archive> file;
  // The positions of the symbols that could be an imported DllMain, which
  // the archive's reader checks once the machine is known.
  std::vector<uint32_t> dllMainCandidates;

private:
  const Archive *archive = nullptr;
  // Each symbol's name, as an offset into the archive's symbol table, and the
  // hash table. An entry is zero, or holds one more than a symbol's position
  // in its low positionBits bits and the high bits of the name's hash above.
  std::vector<uint32_t> nameOffsets;
  std::vector<uint32_t> buckets;
  unsigned positionBits = 0;
  size_t numIndexed = 0;
  // Whether the names are in ascending order, as in a COFF archive, so that
  // those with a given prefix are adjacent.
  bool sorted = true;
  // The positions that remove() left out.
  std::vector<uint32_t> removed;
};

// .lib or .a file.
class ArchiveFile : public InputFile {
public:
  explicit ArchiveFile(COFFLinkerContext &ctx, MemoryBufferRef mb,
                       std::unique_ptr<Archive> &f, ArchiveIndex index = {});
  static bool classof(const InputFile *f) { return f->kind() == ArchiveKind; }
  void parse() override;

  // Enqueues an archive member load for the given symbol. If we've already
  // enqueued a load for the same archive member, this function does nothing,
  // which ensures that we don't load the same member more than once.
  void addMember(const Archive::Symbol &sym);

  // An archive that the symbol table looks up instead of holding its lazy
  // symbols is indexed.
  std::optional<uint32_t> findSymbol(llvm::CachedHashStringRef name) const {
    return index.find(name);
  }
  Archive::Symbol getSymbol(uint32_t i) const { return index.getSymbol(i); }
  StringRef getSymbolName(uint32_t i) const { return index.getSymbolName(i); }
  // The positions of the indexed symbols, in hash table order.
  std::vector<uint32_t> getIndexedSymbols() const {
    return index.getIndexedSymbols();
  }
  // The names of the archive's symbols that start with prefix.
  std::vector<StringRef> getSymbolNames(StringRef prefix) const {
    return index.getSymbolNames(prefix);
  }
  size_t getNumIndexed() const { return index.getNumIndexed(); }

private:
  std::unique_ptr<Archive> file;
  llvm::DenseSet<uint64_t> seen;
  ArchiveIndex index;
};

// .obj or .o file. This may be a member of an archive file.
class ObjFile : public InputFile {
public:
  static ObjFile *create(COFFLinkerContext &ctx, COFFObjectFile *coffObj,
                         bool lazy = false);
  static ObjFile *create(COFFLinkerContext &ctx, MemoryBufferRef mb,
                         bool lazy = false) {
    return ObjFile::create(ctx, ObjFile::createCOFFObject(ctx, mb).release(),
                           lazy);
  }
  explicit ObjFile(SymbolTable &symtab, COFFObjectFile *coffObj, bool lazy);

  static std::unique_ptr<COFFObjectFile>
  createCOFFObject(COFFLinkerContext &ctx, MemoryBufferRef mb);

  static bool classof(const InputFile *f) { return f->kind() == ObjectKind; }
  void parse() override;
  void parseLazy();
  MachineTypes getMachineType() const override;
  ArrayRef<Chunk *> getChunks() { return chunks; }
  // The chunk of the section numbered sectionNumber, or null if the object has
  // no such section or did not keep it. Only an object with link-only records
  // keeps the index past initialization, since only its records number
  // sections.
  SectionChunk *getSectionChunk(uint32_t sectionNumber) const;
  ArrayRef<SectionChunk *> getDebugChunks() { return debugChunks; }
  ArrayRef<SectionChunk *> getSXDataChunks() { return sxDataChunks; }
  ArrayRef<SectionChunk *> getGuardFidChunks() { return guardFidChunks; }
  ArrayRef<SectionChunk *> getGuardIATChunks() { return guardIATChunks; }
  ArrayRef<SectionChunk *> getGuardLJmpChunks() { return guardLJmpChunks; }
  ArrayRef<SectionChunk *> getGuardEHContChunks() { return guardEHContChunks; }
  ArrayRef<Symbol *> getSymbols() { return symbols; }

  MutableArrayRef<Symbol *> getMutableSymbols() { return symbols; }

  ArrayRef<uint8_t> getDebugSection(StringRef secName);

  // Returns a Symbol object for the symbolIndex'th symbol in the
  // underlying object file.
  Symbol *getSymbol(uint32_t symbolIndex) {
    return symbols[symbolIndex];
  }

  // The indices of the external symbols that the object's symbol table
  // defines in the section numbered section, in order.
  ArrayRef<uint32_t> getExternalDefinitions(uint32_t section);

  // Returns the underlying COFF file.
  COFFObjectFile *getCOFFObj() { return coffObj.get(); }

  // Add a symbol for a range extension thunk. Return the new symbol table
  // index. This index can be used to modify a relocation.
  uint32_t addRangeThunkSymbol(Symbol *thunk) {
    symbols.push_back(thunk);
    return symbols.size() - 1;
  }

  void includeResourceChunks();

  bool isResourceObjFile() const { return !resourceChunks.empty(); }

  // Flags in the absolute @feat.00 symbol if it is present. These usually
  // indicate if an object was compiled with certain security features enabled
  // like stack guard, safeseh, /guard:cf, or other things.
  uint32_t feat00Flags = 0;

  // True if this object file is compatible with SEH.  COFF-specific and
  // x86-only. COFF spec 5.10.1. The .sxdata section.
  bool hasSafeSEH() { return feat00Flags & 0x1; }

  // True if this file was compiled with /guard:cf.
  bool hasGuardCF() { return feat00Flags & 0x800; }

  // True if this file was compiled with /guard:ehcont.
  bool hasGuardEHCont() { return feat00Flags & 0x4000; }

  // Pointer to the PDB module descriptor builder. Various debug info records
  // will reference object files by "module index", which is here. Things like
  // source files and section contributions are also recorded here. Will be null
  // if we are not producing a PDB.
  llvm::pdb::DbiModuleDescriptorBuilder *moduleDBI = nullptr;

  const coff_section *addrsigSec = nullptr;

  const coff_section *callgraphSec = nullptr;

  // Facts the object's link-only records describe about one relocation, joined
  // to it at read time so consumers need no search: its site's form, whether
  // its symbol is only called through it, and any interior place it refers to.
  struct RelocFact {
    uint32_t interior = 0;
    llvm::COFF::LinkSiteForm form = llvm::COFF::LinkSiteOther;
    bool hasSite = false;
    bool hasInterior = false;
    bool callOnly = false;
  };

  // A site the records describe as a load of an import pointer whose register
  // only memory reads read, with its form and (on ARM64) the distance back to
  // the adrp giving the pointer's page; or, of form LinkSiteOther, an access
  // through such a load's register that only reads.
  struct LinkValueOnly {
    uint32_t section;
    uint32_t offset;
    uint16_t page;
    llvm::COFF::LinkSiteForm form;
  };

  // A load of the import pointer of data, at offset in section, whose pointer
  // register reg holds the pointer of other data that the load at load gave
  // it, on every path to it.
  struct LinkAnchor {
    uint32_t section;
    uint32_t offset;
    uint32_t load;
    uint8_t reg;
  };

  // A site the records describe as a call or jump through a register a load
  // filled with a pointer (padded to the memory transfer's length), as such a
  // load only those calls read, or as an access or add through that register;
  // or as an index, adding an offset an access of a thread-local variable's
  // record loaded. An access of a record may itself be a load. Once the linker
  // knows where each pointer leads it decides which calls stay, go direct or
  // read the pointer, which accesses and adds go direct, and which loads are
  // dead.
  struct LinkFusion {
    enum CallForm : uint8_t { ThroughRegister, Direct, ThroughPointer, Record };
    enum UseKind : uint8_t { None, Call, Access, Add, Index };

    uint32_t section;
    uint32_t offset;
    // For a call, access or add, the offset of the load's site; for a call,
    // the register the load fills, and whether the call is a jump; for an
    // add, the register it writes.
    uint32_t load = 0;
    uint8_t reg = 0;
    bool isJump = false;
    UseKind use = None;
    // For an access, the offsets in it of its ModRM byte and of the prefix
    // that holds its base register's high bit (15 for none), that prefix's
    // kind (a LinkAccessPrefix), its length, the size of its displacement (0,
    // 1 for 8 bits, 2 for 32), whether a SIB byte follows ModRM, and the log2
    // of the scale of an 8-bit displacement. An index has a SIB byte, and
    // base says whether the register is its base rather than its index; or,
    // where add is set, it is add r64, r64, and base says whether the
    // register is its destination rather than its source.
    uint8_t modrm = 0;
    uint8_t prefix = 15;
    uint8_t prefixKind = 0;
    uint8_t length = 0;
    uint8_t disp = 0;
    uint8_t scale = 0;
    bool sib = false;
    bool base = false;
    bool add = false;
    // For an add, the constant it adds.
    int64_t addend = 0;
    // For an access, whether it only reads memory (not volatile or atomic),
    // so it may read the image's own copy of immutable data.
    bool valueRead = false;
    // For a load, the length of the instruction that directly follows it, or
    // zero where none is known, and whether that one transfers control or may
    // fuse with a branch.
    uint8_t nextSize = 0;
    bool isLoad = false;
    bool nextTransfers = false;
    // The form the linker writes a call in, and whether nothing reads the
    // load. On ARM64 an access of a thread-local variable's record, or a load
    // of a record all of whose accesses are such, that takes what the record
    // holds wherever the code lies, is of form Record.
    CallForm form = ThroughRegister;
    bool dead = false;
  };

  // Whether the COMDAT definition d has default visibility: listed with that
  // role in the object's link records, or any such definition when none list.
  bool hasDefaultVisibility(DefinedRegular *d) const;
  // Whether the object's link records list its definitions with default
  // visibility, as a compiler that shares them along import paths emits.
  bool listsBindings() const {
    return linkRecords && linkRecords->listsBindings;
  }

  bool hasLinkRecords() const { return linkRecords; }

  // True if the object describes every x86-64 instruction site its records
  // must, so any other code REL32 to a preemptible symbol is a branch.
  bool describesSites() const {
    return linkRecords && linkRecords->describesSites;
  }
  // Whether the object describes its sites that refer to an external function
  // it defines, so that a REL32 relocation to one without a site is a branch.
  bool describesFunctionSites() const {
    return linkRecords && linkRecords->describesFunctionSites;
  }

  // True if the object's KCFI lists name each imported function by its IAT
  // entry alone, so the linker lists the import thunk where data holds it.
  bool listsKCFIImports() const {
    return linkRecords && linkRecords->listsKCFIImports;
  }

  // True if the object's delay-load helper writes a delay-load import address
  // table only while the table is writable.
  bool protectsDelayIat() const {
    return linkRecords && linkRecords->protectsDelayIat;
  }

  // The facts the records join to rel, a relocation of the section of sc, or
  // null when the object joins none to that section's relocations.
  RelocFact *relocFact(const SectionChunk *sc, const coff_relocation &rel) const;

  // Whether the object joins facts to the relocations of the section of sc.
  bool hasRelocFacts(const SectionChunk *sc) const;

  // The fused calls and loads of the section of sc, by offset.
  MutableArrayRef<LinkFusion> getLinkFusions(const SectionChunk *sc);

  // The fused call or load at offset in the section of sc, if any.
  const LinkFusion *getLinkFusion(const SectionChunk *sc,
                                  uint32_t offset) const;

  // Whether the section of sc ends in a jump that also ends the function it
  // holds.
  bool endsInTailJump(const SectionChunk *sc) const;

  bool hasLinkFusions() const {
    return linkRecords && !linkRecords->fusions.empty();
  }

  // The loads the object describes as only read through, and the accesses it
  // describes as only reading.
  ArrayRef<LinkValueOnly> getValueOnlyLoads() const {
    return linkRecords ? ArrayRef(linkRecords->valueOnlyLoads)
                       : ArrayRef<LinkValueOnly>();
  }

  // The loads the object describes as able to take their pointers from the
  // registers of other loads, by section and offset.
  ArrayRef<LinkAnchor> getLinkAnchors() const {
    return linkRecords ? ArrayRef(linkRecords->anchors) : ArrayRef<LinkAnchor>();
  }

  // A value or alignment the records say depends on where a covered section's
  // code lies: at offset in section field, a value of section target's code.
  // For an alignment, a is the padding length, b its max, kind the log2
  // alignment and size the longest no-op to fill it; for an interior entry, a
  // is the place in target the relocation at offset refers to.
  struct LinkCoverage {
    uint32_t field;
    uint32_t target;
    uint32_t offset;
    uint32_t a;
    uint32_t b;
    int64_t constant;
    llvm::COFF::LinkCoverageEntry type;
    uint8_t kind;
    uint8_t size;
  };

  // Whether the object describes every layout relationship of the code of
  // the section of sc, so that the linker may resize it.
  bool isCovered(const SectionChunk *sc) const;
  bool hasLinkCoverage() const {
    return linkRecords && !linkRecords->coveredSections.empty();
  }
  // The values and alignments in the section of sc, by offset.
  ArrayRef<LinkCoverage> getLinkCoverage(const SectionChunk *sc) const;
  // Every value and alignment of the object.
  ArrayRef<LinkCoverage> getAllLinkCoverage() const {
    return linkRecords ? ArrayRef<LinkCoverage>(linkRecords->coverage)
                       : ArrayRef<LinkCoverage>();
  }

  // True if the object lists every 32-bit field in data through which a
  // symbol it leaves undefined is only called, so that any other such field
  // holds an address.
  bool listsCallOnly() const {
    return linkRecords && linkRecords->listsCallOnly;
  }

  // A pin of the object's link-only records: the address of sym, or of the
  // start of chunk when the pin names a section, must be residue modulo
  // 2^log2. A pin names a symbol, so one that a discarded COMDAT copy carries
  // applies to the copy the link keeps.
  struct LinkPin {
    Symbol *sym;
    SectionChunk *chunk;
    uint64_t residue;
    uint8_t log2;
    bool required;
  };
  ArrayRef<LinkPin> getLinkPins() const;

  // A vtable of the object's link-only records: vtable->getValue() + offset is
  // the primary address point of a vtable of class cls with primary base base
  // (empty if none). onSection is set when the record named a section, not the
  // vtable's symbol. A discarded COMDAT copy's facts describe the kept copy.
  struct LinkVTable {
    DefinedRegular *vtable;
    uint64_t offset;
    std::string cls;
    std::string base;
    bool onSection;
  };
  ArrayRef<LinkVTable> getLinkVTables() const;

  // The number of the section whose symbol is the object's symbol of index,
  // which a group of its link-only records, group, names.
  llvm::Expected<uint32_t> getRecordSection(uint64_t index,
                                            const llvm::Twine &group);

  // When using Microsoft precompiled headers, this is the PCH's key.
  // The same key is used by both the precompiled object, and objects using the
  // precompiled object. Any difference indicates out-of-date objects.
  std::optional<uint32_t> pchSignature;

  // Whether this file was compiled with /hotpatch.
  bool hotPatchable = false;

  // Whether the object was already merged into the final PDB.
  bool mergedIntoPDB = false;

  // True if LTO generated this file.
  bool ltoOutput = false;

  // If the OBJ has a .debug$T stream, this tells how it will be handled.
  TpiSource *debugTypesObj = nullptr;

  // The .debug$P or .debug$T section data if present. Empty otherwise.
  ArrayRef<uint8_t> debugTypes;

  std::optional<std::pair<StringRef, uint32_t>>
  getVariableLocation(StringRef var);

  std::optional<llvm::DILineInfo> getDILineInfo(uint32_t offset,
                                                uint32_t sectionIndex);

private:
  const coff_section* getSection(uint32_t i);
  const coff_section *getSection(COFFSymbolRef sym) {
    return getSection(sym.getSectionNumber());
  }

  void enqueuePdbFile(StringRef path, ObjFile *fromFile);

  void initializeChunks();
  void initializeSymbols();
  void initializeFlags();
  void readLinkRecords();
  llvm::Error readLinkSites(ArrayRef<uint8_t> payload);
  llvm::Error readLinkCoverage(ArrayRef<uint8_t> payload);
  llvm::Error readLinkPins(ArrayRef<uint8_t> payload);
  llvm::Error readLinkVTables(ArrayRef<uint8_t> payload);
  llvm::Error readKCFIThunks(ArrayRef<uint8_t> payload);
  llvm::Error readCallOnlyRefs(ArrayRef<uint8_t> payload);
  llvm::Error readTailJumps(ArrayRef<uint8_t> payload);
  void initializeDependencies();
  void initializeECThunks();

  SectionChunk *
  readSection(uint32_t sectionNumber,
              const llvm::object::coff_aux_section_definition *def,
              StringRef leaderName);

  void readAssociativeDefinition(
      COFFSymbolRef coffSym,
      const llvm::object::coff_aux_section_definition *def);

  void readAssociativeDefinition(
      COFFSymbolRef coffSym,
      const llvm::object::coff_aux_section_definition *def,
      uint32_t parentSection);

  void recordPrevailingSymbolForMingw(
      COFFSymbolRef coffSym,
      llvm::DenseMap<StringRef, uint32_t> &prevailingSectionMap);

  void maybeAssociateSEHForMingw(
      COFFSymbolRef sym, const llvm::object::coff_aux_section_definition *def,
      const llvm::DenseMap<StringRef, uint32_t> &prevailingSectionMap);

  // Given a new symbol Sym with comdat selection Selection, if the new
  // symbol is not (yet) Prevailing and the existing comdat leader set to
  // Leader, emits a diagnostic if the new symbol and its selection doesn't
  // match the existing symbol and its selection. If either old or new
  // symbol have selection IMAGE_COMDAT_SELECT_LARGEST, Sym might replace
  // the existing leader. In that case, Prevailing is set to true.
  void
  handleComdatSelection(COFFSymbolRef sym, llvm::COFF::COMDATType &selection,
                        bool &prevailing, DefinedRegular *leader,
                        const llvm::object::coff_aux_section_definition *def);

  std::optional<Symbol *>
  createDefined(COFFSymbolRef sym,
                std::vector<const llvm::object::coff_aux_section_definition *>
                    &comdatDefs,
                bool &prevailingComdat);
  Symbol *createRegular(COFFSymbolRef sym);
  Symbol *createUndefined(COFFSymbolRef sym, bool overrideLazy);

  std::unique_ptr<COFFObjectFile> coffObj;

  // List of all chunks defined by this file. This includes both section
  // chunks and non-section chunks for common symbols.
  std::vector<Chunk *> chunks;

  // What the object's link-only records hold, allocated only for an object that
  // has them: their section, what they say of the object, and its described
  // data.
  struct LinkRecords {
    const coff_section *sec;
    bool describesSites = false;
    bool describesFunctionSites = false;
    bool listsKCFIImports = false;
    bool protectsDelayIat = false;
    bool listsCallOnly = false;
    bool listsBindings = false;
    bool checkedFusion = false;
    std::vector<LinkFusion> fusions;
    std::vector<LinkValueOnly> valueOnlyLoads;
    std::vector<LinkAnchor> anchors;
    // Values and alignments depending on where resizable code lies, by section
    // and offset, and the covered sections' numbers, in order.
    std::vector<LinkCoverage> coverage;
    std::vector<uint32_t> coveredSections;
    // The numbers of the sections that end in a jump that also ends the
    // function each holds, in order.
    std::vector<uint32_t> tailJumps;
    std::vector<LinkPin> pins;
    std::vector<LinkVTable> vtables;
    // The indices of the symbols the object gives default visibility.
    llvm::DenseSet<uint32_t> defaultBindings;
    // Facts joined to each relocation, and where each section's begin by
    // section number, filled directly as the records are read.
    std::vector<RelocFact> relocFacts;
    std::vector<uint32_t> relocFactStarts;
  };
  LinkRecords *linkRecords = nullptr;

  std::vector<SectionChunk *> resourceChunks;

  // CodeView debug info sections.
  std::vector<SectionChunk *> debugChunks;

  // Chunks containing symbol table indices of exception handlers. Only used for
  // 32-bit x86.
  std::vector<SectionChunk *> sxDataChunks;

  // Chunks containing symbol table indices of address taken symbols, address
  // taken IAT entries, longjmp and ehcont targets. These are not linked into
  // the final binary when /guard:cf is set.
  std::vector<SectionChunk *> guardFidChunks;
  std::vector<SectionChunk *> guardIATChunks;
  std::vector<SectionChunk *> guardLJmpChunks;
  std::vector<SectionChunk *> guardEHContChunks;

  std::vector<SectionChunk *> hybmpChunks;

  // This vector contains a list of all symbols defined or referenced by this
  // file. They are indexed such that you can get a Symbol by symbol
  // index. Nonexistent indices (which are occupied by auxiliary
  // symbols in the real symbol table) are filled with null pointers.
  std::vector<Symbol *> symbols;

  // This vector contains the same chunks as Chunks, but they are
  // indexed such that you can get a SectionChunk by section index.
  // Nonexistent section indices are filled with null pointers.
  // (Because section number is 1-based, the first slot is always a
  // null pointer.) This vector is only valid during initialization, but for
  // an object with link-only records.
  std::vector<SectionChunk *> sparseChunks;

  // Indices of the external symbols the object defines, in order, and where
  // each section's begin by section number; getExternalDefinitions fills these.
  std::vector<uint32_t> externalDefinitions;
  std::vector<uint32_t> externalDefinitionStarts;

  DWARFCache *dwarf = nullptr;
};

// This is a PDB type server dependency, that is not a input file per se, but
// needs to be treated like one. Such files are discovered from the debug type
// stream.
class PDBInputFile : public InputFile {
public:
  explicit PDBInputFile(COFFLinkerContext &ctx, MemoryBufferRef m);
  ~PDBInputFile();
  static bool classof(const InputFile *f) { return f->kind() == PDBKind; }
  void parse() override;

  static PDBInputFile *findFromRecordPath(const COFFLinkerContext &ctx,
                                          StringRef path, ObjFile *fromFile);

  // Record possible errors while opening the PDB file
  std::optional<std::string> loadErrorStr;

  // This is the actual interface to the PDB (if it was opened successfully)
  std::unique_ptr<llvm::pdb::NativeSession> session;

  // If the PDB has a .debug$T stream, this tells how it will be handled.
  TpiSource *debugTypesObj = nullptr;
};

// This type represents import library members that contain DLL names
// and symbols exported from the DLLs. See Microsoft PE/COFF spec. 7
// for details about the format.
class ImportFile : public InputFile {
public:
  explicit ImportFile(COFFLinkerContext &ctx, MemoryBufferRef m);

  static bool classof(const InputFile *f) { return f->kind() == ImportKind; }
  MachineTypes getMachineType() const override { return getMachineType(mb); }
  static MachineTypes getMachineType(MemoryBufferRef m);
  bool isSameImport(const ImportFile *other) const;
  bool isEC() const { return impECSym != nullptr; }

  // Whether the link delay-loads the import's DLL, and whether it ignores its
  // offers (as it does a delay-loaded DLL's). A member may be read before
  // /delayload and -ignore-offers are, so each is found when first asked.
  bool isDelayLoaded() const;
  bool ignoresOffers() const;
  ImportThunkChunk *makeImportThunk();

  DefinedImportData *impSym = nullptr;
  Defined *thunkSym = nullptr;
  ImportThunkChunkARM64EC *impchkThunk = nullptr;
  ImportFile *hybridFile = nullptr;
  std::string dllName;

private:
  void parse() override;
  void findDLLOptions() const;

  mutable bool dllOptionsFound = false;
  mutable bool delayLoaded = false;
  mutable bool offersIgnored = false;

public:
  StringRef externalName;
  const coff_import_header *hdr;
  Chunk *location = nullptr;

  // Auxiliary IAT symbols and chunks on ARM64EC.
  DefinedImportData *impECSym = nullptr;
  Chunk *auxLocation = nullptr;
  Defined *auxThunkSym = nullptr;
  DefinedImportData *auxImpCopySym = nullptr;
  Chunk *auxCopyLocation = nullptr;

  // We want to eliminate dllimported symbols if no one actually refers to them.
  // These "Live" bits are used to keep track of which import library members
  // are actually in use.
  //
  // If the Live bit is turned off by MarkLive, Writer will ignore dllimported
  // symbols provided by this import library member.
  bool live;

  // Under -import-slots, whether an object may take the imported function's
  // address in an undescribed instruction, so the thunk is its address
  // everywhere.
  bool thunkIsAddress = false;

  // Under -import-slots, how references other than in-place slots use the
  // import's entry: not at all, only to read the whole address, or otherwise;
  // whether data reads a delay-loaded entry, which keeps ARM64 loads of it;
  // and how many references name the thunk and how many of those bypass it.
  enum EntryUse : uint8_t { Unused, ReadWhole, UsedOtherwise };
  EntryUse entryUse = Unused;
  bool delayLoadRead = false;
  uint32_t thunkRefs = 0;
  uint32_t thunkBypasses = 0;

  // Under -import-slots, whether the import is bound by name at image start,
  // into a cell its fill writes, rather than by the loader: an import from an
  // executable (always the process's own, whatever its file name) and one only
  // weak references name (null where its DLL lacks it, and no dependency on the
  // DLL).
  bool isHostImport = false;
  bool isWeakImport = false;
  bool isBoundAtRunTime() const { return isHostImport || isWeakImport; }

  // The offset of the import's address in location: 0 for an IAT entry, the
  // word's offset for a word of static data the loader writes in place. ARM64EC
  // has no such words, so the auxiliary locations need no offset.
  uint32_t locationOffset = 0;
};

// Used for LTO.
class BitcodeFile : public InputFile {
public:
  explicit BitcodeFile(SymbolTable &symtab, MemoryBufferRef mb,
                       std::unique_ptr<llvm::lto::InputFile> &obj, bool lazy);
  ~BitcodeFile();

  static BitcodeFile *create(COFFLinkerContext &ctx, MemoryBufferRef mb,
                             StringRef archiveName, uint64_t offsetInArchive,
                             bool lazy);
  static bool classof(const InputFile *f) { return f->kind() == BitcodeKind; }
  ArrayRef<Symbol *> getSymbols() { return symbols; }
  MachineTypes getMachineType() const override {
    return getMachineType(obj.get());
  }
  static MachineTypes getMachineType(const llvm::lto::InputFile *obj);
  void parseLazy();
  std::unique_ptr<llvm::lto::InputFile> obj;

private:
  void parse() override;

  std::vector<Symbol *> symbols;
};

// A short import member that imports symbolName from dllName, as an import
// library holds one: by name, or by ordinal with IMPORT_ORDINAL.
MemoryBufferRef makeShortImport(llvm::COFF::MachineTypes machine,
                                StringRef dllName, StringRef symbolName,
                                llvm::COFF::ImportType importType,
                                llvm::COFF::ImportNameType nameType,
                                uint16_t ordinal = 0);

// .dll file. MinGW only.
class DLLFile : public InputFile {
public:
  explicit DLLFile(SymbolTable &symtab, std::unique_ptr<COFFObjectFile> &obj)
      : InputFile(symtab, DLLKind, obj->getMemoryBufferRef()) {
    coffObj.swap(obj);
  }
  static bool classof(const InputFile *f) { return f->kind() == DLLKind; }
  void parse() override;
  MachineTypes getMachineType() const override;

  struct Symbol {
    StringRef dllName;
    StringRef symbolName;
    llvm::COFF::ImportNameType nameType;
    llvm::COFF::ImportType importType;
  };

  void makeImport(Symbol *s);

private:
  std::unique_ptr<COFFObjectFile> coffObj;
  llvm::StringSet<> seen;
};

inline bool isBitcode(MemoryBufferRef mb) {
  return identify_magic(mb.getBuffer()) == llvm::file_magic::bitcode;
}

std::string replaceThinLTOSuffix(StringRef path, StringRef suffix,
                                 StringRef repl);

// The error of malformed link-only records, whose message follows the name of
// their section.
llvm::Error malformedLinkRecords(const llvm::Twine &msg);

// Calls fn with the kind and payload of each group in contents, an
// .llvm_link_records section, returning the first error (message after the
// section name). header, if given, receives the capabilities before any group.
llvm::Error forEachLinkRecordGroup(
    ArrayRef<uint8_t> contents,
    llvm::function_ref<llvm::Error(uint64_t kind, ArrayRef<uint8_t> payload)>
        fn,
    llvm::function_ref<void(uint64_t capabilities)> header = nullptr);

// Calls fn with each group of the link-only records that mb, a member of an
// import library, holds, if it is an object for machine, or for any machine
// when that is unknown. A member it cannot read holds none.
void forEachLinkRecordGroup(
    MemoryBufferRef mb, llvm::COFF::MachineTypes machine,
    llvm::function_ref<void(uint64_t kind, ArrayRef<uint8_t> payload)> fn);

// A member of the import library of the DLL dllName whose link-only records
// are groups, each a kind and its payload, and which defines the absolute
// symbol symbolName, by which an importer's link finds it.
llvm::NewArchiveMember
createLinkRecordMember(COFFLinkerContext &ctx, StringRef dllName,
                       StringRef symbolName,
                       ArrayRef<std::pair<uint64_t, std::string>> groups,
                       std::vector<uint8_t> &buffer);
} // namespace coff

std::string toString(const coff::InputFile *file);
} // namespace lld

#endif
