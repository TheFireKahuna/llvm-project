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
#include "Relocations.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/StringSaver.h"
#include <atomic>
#include <memory>
#include <set>
#include <vector>

namespace llvm {
struct DILineInfo;
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

using llvm::COFF::IMAGE_FILE_MACHINE_UNKNOWN;
using llvm::COFF::MachineTypes;
using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_import_header;
using llvm::object::coff_section;

class Chunk;
class Defined;
class DefinedImportData;
class DefinedImportThunk;
class DefinedLocalImport;
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

  // True if LLVM made this file for Windows Itanium or NT-POSIX: an object by
  // its @comp.id symbol, bitcode by its triple.
  bool forkObject = false;

  // True if the file states the owned thread-local model in its _TLS_MODEL
  // key: its thread-locals are the image's template, not the loader's.
  bool tlsModel = false;

  // True if the assembler of such an object checked its fused relocation
  // types against its instructions. The link binds only checked fused sites.
  bool fusedChecked = false;

protected:
  // Applies the link facts of an object's Symbols group, or of bitcode, to
  // the symbols that they name by index, where a null symbol takes none. A
  // fact that is not valid is the error, and then no fact is applied.
  // defines says whether the file's definition of the symbol with an index
  // is one that no other definition of the name replaces.
  llvm::Error readLinkFacts(StringRef facts, ArrayRef<Symbol *> symbols,
                            llvm::function_ref<bool(uint64_t)> defines);
};

// .lib or .a file.
class ArchiveFile : public InputFile {
public:
  explicit ArchiveFile(COFFLinkerContext &ctx, MemoryBufferRef mb,
                       std::unique_ptr<Archive> &f);
  static bool classof(const InputFile *f) { return f->kind() == ArchiveKind; }
  void parse() override;

  // Enqueues an archive member load for the given symbol. If we've already
  // enqueued a load for the same archive member, this function does nothing,
  // which ensures that we don't load the same member more than once. Returns
  // whether the member loads, which one the link keeps out does not.
  bool addMember(const Archive::Symbol &sym);
  // Keeps the member that defines sym out of the link.
  void dropMember(const Archive::Symbol &sym);
  // Adds every member not enqueued or added yet, lazily if lazy.
  void addAllMembers(bool lazy);
  // Returns the member that defines the symbol named name, which no link
  // loads, or nothing.
  std::optional<MemoryBufferRef> findMember(StringRef name);

private:
  std::unique_ptr<Archive> file;
  llvm::DenseSet<uint64_t> seen;
  llvm::DenseSet<uint64_t> dropped;
};

// A dispatch through a jump table that an object states: its branch, the
// table of count entries of an llvm::codeview::JumpTableEntrySize type, the
// label that the entries are relative to, the instruction that loads the
// entry and llvm::COFF::SwitchTableFlags.
struct SwitchTable {
  DefinedRegular *branch, *table, *base, *load;
  uint64_t count;
  uint8_t type, flags;
};

// The spans that the link records state of the fields of one section over
// one code section, llvm::COFF::SpanKind, as the records hold them: each a
// field that holds the distance between two points of the code. They were
// checked when the file was read. jumps says that one is a SpanJump32.
struct SpanList {
  SectionChunk *field, *target;
  StringRef spans;
  bool jumps;
};

// A fact of the site of a relocation of a section that an object states: an
// llvm::COFF::SiteFact and its value.
struct RelocFact {
  const SectionChunk *chunk;
  uint32_t reloc;
  uint32_t fact;
  uint64_t value;
  bool operator==(const RelocFact &o) const {
    return reloc == o.reloc && fact == o.fact && value == o.value;
  }
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

  // The size that the object states for c, a COMDAT section of its own, which
  // an assembler that pads its sections keeps apart from the padded size.
  uint32_t getComdatLength(const SectionChunk *c);

  const coff_section *linkRecordsSec = nullptr;

  // The dispatches through jump tables that the link records state and whose
  // branches \p sc holds. Kept in the order of the sections of their branches.
  ArrayRef<SwitchTable> getSwitchTables(const SectionChunk *sc) const;
  std::vector<SwitchTable> switchTables;

  // The lists of spans that the link records state for the fields of \p sc.
  // Kept in the order of the sections of their fields, the first list of
  // each section by its number in spanStarts.
  ArrayRef<SpanList> getSpans(const SectionChunk *sc) const;
  std::vector<SpanList> spanLists;
  std::vector<uint32_t> spanStarts;

  // The facts that the link records state of the sites of \p sc, in the order
  // of its relocations, and the value of one fact of one relocation, if the
  // records state it. Kept in the order of the sections, and only where the
  // assembler checked the object's fused sites.
  ArrayRef<RelocFact> getSiteFacts(const SectionChunk *sc) const;
  std::optional<uint64_t> getSiteFact(const SectionChunk *sc, size_t reloc,
                                      uint32_t fact) const;
  std::vector<RelocFact> siteFacts;

  // Each section that the link records list with loops a cache line is to
  // keep, and the loops the link is to bar it by, as the record has them.
  llvm::DenseMap<const SectionChunk *, StringRef> lineLoops;

  // The relocations of each section whose instructions the link rewrites or
  // may resize, as scanRelocations gives them.
  llvm::DenseMap<const SectionChunk *, ScannedSection> relocations;

  // The records of the symbols of this file's resizable sections, which are
  // the symbols' own, until relaxation anchors them, and the offsets of those
  // of type prefixes, __cfi_<f>.
  SmallVector<std::pair<SectionChunk *, llvm::object::coff_symbol_generic *>, 0>
      movingSymbols;
  SmallVector<std::pair<SectionChunk *, uint32_t>, 0> typePrefixes;

  // The relocations, by section and index, whose values depend on the layout
  // of resizable code and might not fit their fields once it is resized.
  SmallVector<std::pair<SectionChunk *, uint32_t>, 0> fitChecks;

  // The indexes of the symbols that GOTTPOFF relocations name.
  SmallVector<uint32_t, 0> gotTpoffIndexes;

  // How this object's live sections use each local import pointer, as the
  // scan finds it, in the order of their first reference.
  std::vector<std::pair<DefinedLocalImport *, uint8_t>> localImportUses;

  const coff_section *callgraphSec = nullptr;

  // When using Microsoft precompiled headers, this is the PCH's key.
  // The same key is used by both the precompiled object, and objects using the
  // precompiled object. Any difference indicates out-of-date objects.
  std::optional<uint32_t> pchSignature;

  // Whether this file was compiled with /hotpatch.
  bool hotPatchable = false;

  // Whether the object was already merged into the final PDB.
  bool mergedIntoPDB = false;

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
  // null pointer.) This vector is only valid during initialization.
  std::vector<SectionChunk *> sparseChunks;

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
  // Whether a definition in the link beats this import for imp, its pointer.
  bool yieldsToDefinition(Symbol *imp) const;
  // An import of name from dll as an import library's short member states
  // it, with exportAs the name it imports by where the name type says so.
  static ImportFile *create(COFFLinkerContext &ctx, MachineTypes machine,
                            StringRef name, StringRef dll,
                            llvm::COFF::ImportType type,
                            llvm::COFF::ImportNameType nameType,
                            StringRef exportAs = "", uint16_t hint = 0);
  // Gives the import of a function a thunk that no symbol of the table
  // names: that of an image which defines the function and yet calls the
  // DLL's.
  void makeUnnamedThunk();

  DefinedImportData *impSym = nullptr;
  Defined *thunkSym = nullptr;
  // What a call or jump through the pointer of an overridable default
  // reaches, while the pointer holds the function's address: in an
  // executable what the default's slot names, and in a DLL the pointer of
  // a second import of a default that forwards to another.
  Defined *replacement = nullptr;
  ImportThunkChunkARM64EC *impchkThunk = nullptr;
  ImportFile *hybridFile = nullptr;
  std::string dllName;
  // Whether the link delay-loads the DLL.
  bool delayLoaded = false;
  // Who binds the import: the loader, or the image's fill by name, for an
  // import from the executable that loads the image, whatever its file name,
  // a weak import that may be absent, or both. The fill writes the pointer
  // of one it binds, a word of .sealed in place of the entry.
  enum Binding : uint8_t { Loader, Host, Weak, HostWeak } binding = Loader;

private:
  void parse() override;
  ImportThunkChunk *makeImportThunk();

public:
  StringRef externalName;
  const coff_import_header *hdr;
  Chunk *location = nullptr;
  // Where the pointer is in location, and whether that is an in-place slot,
  // the import having no entry in an import address table of its DLL.
  uint32_t offset = 0;
  bool slotOnly = false;
  // Whether an object lists the function as one whose address it observes,
  // and once the image is scanned, whether that address is its DLL's own
  // though the DLL is delay-loaded.
  bool observed = false;
  // The hint and name that the import's lookup entries give the loader; none
  // for an import by ordinal.
  Chunk *hintName = nullptr;

  // How roots and live sections use the import. Its thunk: as the address in
  // a word that the loader binds in place, or that code takes and the scan
  // loads from the import's pointer instead, both of which get the function's
  // own; or otherwise, as Thunk. A thunk with no other reference is left
  // out. Its pointer: as a Read of all of it, which any word that holds the
  // address serves, or otherwise, which Keeps its entry.
  enum Use : uint8_t {
    InPlace = 1,
    Loaded = 2,
    Thunk = 4,
    Read = 8,
    Keep = 16
  };
  std::atomic<uint8_t> uses = 0;

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
} // namespace coff

std::string toString(const coff::InputFile *file);
} // namespace lld

#endif
