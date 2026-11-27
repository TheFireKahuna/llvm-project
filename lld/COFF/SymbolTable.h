//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOL_TABLE_H
#define LLD_COFF_SYMBOL_TABLE_H

#include "InputFiles.h"
#include "LTO.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
struct LTOCodeGenerator;
}

namespace lld::coff {

class Chunk;
class CommonChunk;
class COFFLinkerContext;
class Defined;
class DefinedAbsolute;
class DefinedRegular;
struct FunctionOverride;
class ImportThunkChunk;
class LazyArchive;
class SameAddressThunkARM64EC;
class SectionChunk;
class Symbol;

// This data structure is instantiated for each -wrap option.
struct WrappedSymbol {
  Symbol *sym;
  // __real_X and its import pointer, each once something refers to it.
  Symbol *real;
  Symbol *realImp;
  Symbol *wrap;
  StringRef name;
  // Whether an object's .drectve asked for the wrap, which then yields to a
  // definition of X among the inputs.
  bool fromDirective;
};

struct UndefinedDiag;

// SymbolTable is a bucket of all known symbols, including defined,
// undefined, or lazy symbols (the last one is symbols in archive
// files whose archive members are not yet loaded).
//
// We put all symbols of all files to a SymbolTable, and the
// SymbolTable selects the "best" symbols if there are name
// conflicts. For example, obviously, a defined symbol is better than
// an undefined symbol. Or, if there's a conflict between a lazy and a
// undefined, it'll read an archive member to read a real definition
// to replace the lazy symbol. The logic is implemented in the
// add*() functions, which are called by input files as they are parsed.
// There is one add* function per symbol type.
class SymbolTable {
public:
  SymbolTable(COFFLinkerContext &c,
              llvm::COFF::MachineTypes machine = IMAGE_FILE_MACHINE_UNKNOWN)
      : ctx(c), machine(machine) {}

  // Emit errors for symbols that cannot be resolved.
  void reportUnresolvable();

  // Try to resolve any undefined symbols and update the symbol table
  // accordingly, then print an error message for any remaining undefined
  // symbols and warn about imported local symbols.
  void resolveRemainingUndefines(std::vector<Undefined *> &aliases);

  // Try to resolve undefined symbols with alternate names.
  void resolveAlternateNames();

  // On ARM64, once liveness is known, binds .refptr cells as local import
  // pointers, and when every instruction that refers to a pointer loads it,
  // makes the loads reach its symbol directly, keeping the pointer only for a
  // root or data that refers to it.
  void bindLocalImports();

  // Defines the referenced, undefined __start_X and __stop_X of each run of
  // sections named X or X$*, where X is a C identifier, and _etext, _edata and
  // _end, unprefixed too. The writer places them once the layout is known.
  void addStartStopSymbols();

  // Load lazy objects that are needed for MinGW automatic import and for
  // doing stdcall fixups, and those that a reference to X or __imp_X from an
  // object for Windows Itanium or NT-POSIX asks for. Returns whether it loaded
  // any.
  bool loadMinGWSymbols();
  bool handleMinGWAutomaticImport(Symbol *sym, StringRef name);

  // Returns a symbol for a given name. Returns a nullptr if not found.
  Symbol *find(StringRef name);
  Symbol *findUnderscore(StringRef name);
  // Returns the symbols whose names match, in the order they entered the table.
  std::vector<Symbol *> findAll(llvm::function_ref<bool(StringRef)> match);

  void addUndefinedGlob(StringRef arg);

  // Occasionally we have to resolve an undefined symbol to its
  // mangled symbol. This function tries to find a mangled name
  // for U from the symbol table, and if found, set the symbol as
  // a weak alias for U.
  Symbol *findMangle(StringRef name);
  StringRef mangleMaybe(Symbol *s);

  // Symbol names are mangled by prepending "_" on x86.
  StringRef mangle(StringRef sym);

  // Windows specific -- "main" is not the only main function in Windows.
  // You can choose one from these four -- {w,}{WinMain,main}.
  // There are four different entry point functions for them,
  // {w,}{WinMain,main}CRTStartup, respectively. The linker needs to
  // choose the right one depending on which "main" function is defined.
  // This function looks up the symbol table and resolve corresponding
  // entry point name.
  StringRef findDefaultEntry();
  WindowsSubsystem inferSubsystem();

  // Build a set of COFF objects representing the combined contents of
  // BitcodeFiles and add them to the symbol table. Called after all files are
  // added and before the writer writes results to a file.
  void compileBitcodeFiles();

  void waitForLTOCleanup();

  // Creates an Undefined symbol and marks it as live.
  Symbol *addGCRoot(StringRef sym, bool aliasEC = false);

  // Creates an Undefined symbol for a given name.
  Symbol *addUndefined(StringRef name);

  Symbol *addSynthetic(StringRef n, Chunk *c);
  Symbol *addAbsolute(StringRef n, uint64_t va);

  Symbol *addUndefined(StringRef name, InputFile *f, bool overrideLazy);
  void addStrongReference(Symbol *s);
  bool loadsLazy(Symbol *s, InputFile *f);
  void warnWeakDefinition(Symbol *s, InputFile *lazy, InputFile *weak);
  void addLazyArchive(ArchiveFile *f, const Archive::Symbol &sym);
  void addLazyObject(InputFile *f, StringRef n);
  void addLazyDLLSymbol(DLLFile *f, DLLFile::Symbol *sym, StringRef n);
  Symbol *addAbsolute(StringRef n, COFFSymbolRef s);
  Symbol *addRegular(InputFile *f, StringRef n,
                     const llvm::object::coff_symbol_generic *s = nullptr,
                     SectionChunk *c = nullptr, uint32_t sectionOffset = 0,
                     bool isWeak = false);
  std::pair<DefinedRegular *, bool>
  addComdat(InputFile *f, StringRef n,
            const llvm::object::coff_symbol_generic *s = nullptr);
  Symbol *addCommon(InputFile *f, StringRef n, uint64_t size,
                    const llvm::object::coff_symbol_generic *s = nullptr,
                    CommonChunk *c = nullptr);
  DefinedImportData *addImportData(StringRef n, ImportFile *f,
                                   Chunk *&location);
  Defined *addImportThunk(StringRef name, DefinedImportData *s,
                          ImportThunkChunk *chunk);
  void addLibcall(StringRef name);
  void addEntryThunk(Symbol *from, Symbol *to);
  void addExitThunk(Symbol *from, Symbol *to);
  void initializeECThunks();
  void initializeSameAddressThunks();

  void reportDuplicate(Symbol *existing, InputFile *newFile,
                       SectionChunk *newSc = nullptr,
                       uint32_t newSectionOffset = 0);

  COFFLinkerContext &ctx;
  llvm::COFF::MachineTypes machine;

  bool isEC() const { return machine == ARM64EC; }

  // An entry point symbol.
  Symbol *entry = nullptr;

  // A list of chunks which to be added to .rdata.
  std::vector<Chunk *> localImportChunks;

  // A list of EC EXP+ symbols.
  std::vector<Symbol *> expSymbols;

  std::vector<SameAddressThunkARM64EC *> sameAddressThunks;

  // The runs of sections that addStartStopSymbols bounds, and the _etext,
  // _edata and _end it defines, a weak one as its zero default.
  struct SectionRun {
    StringRef name;
    Symbol *start = nullptr;
    Symbol *stop = nullptr;
    std::vector<SectionChunk *> chunks;
  };
  std::vector<SectionRun> sectionRuns;
  std::vector<Symbol *> boundarySymbols;

  // The vague entities that the link facts state, by the symbol that every
  // copy resolves to, in the order the link met them. A DLL that keeps one
  // offers it, and an image that imports from that DLL binds the DLL's.
  struct VagueEntity {
    VagueEntity() : init(0), descriptor(false), function(false) {}
    // The greatest llvm::COFF::LinkFactInitKind that a copy of a variable
    // states.
    uint8_t init : 2;
    // True if the entity is a type_info object.
    uint8_t descriptor : 1;
    // True if the entity is a function, whose address some object takes.
    uint8_t function : 1;
  };
  llvm::MapVector<Symbol *, VagueEntity> vagueEntities;

  // Whether the image takes the instance of a vague entity that the DLL of
  // file offers for its own. Every binding of an offer asks here, at the
  // import that carries it, so an offer the image does not take leaves no
  // import of its name.
  bool honoursOffer(const ImportFile *file);

  // Gives each vague entity that the link defines and owner names an import
  // for, with every name of its COMDAT group, the instance of the import's
  // DLL, and leaves the group's sections out of the image. It runs once
  // before LTO, which then compiles no copy, and again once every input is
  // loaded, before liveness is decided.
  void preemptVagueEntities(
      llvm::function_ref<DefinedImportData *(StringRef)> owner);

  // The sections that preemptVagueEntities left out, which no later
  // reference brings back.
  std::vector<SectionChunk *> preempted;

  // A list of DLL exports.
  std::vector<Export> exports;
  // Exports that repeat one of those names through EXPORTAS: the export table
  // has the name once, and the import library has their symbols too.
  std::vector<Export> exportAsAliases;
  llvm::DenseSet<StringRef> directivesExports;
  bool hadExplicitExports;
  // Definitions whose names state an export, until addNamedExports reads them.
  std::vector<Symbol *> namedExports;
  // Each default version, f@@V, with f, until f is seen to have a definition
  // of its own.
  std::vector<std::pair<StringRef, DefinedRegular *>> defaultVersions;

  Chunk *edataStart = nullptr;
  Chunk *edataEnd = nullptr;

  Symbol *delayLoadHelper = nullptr;
  Chunk *tailMergeUnwindInfoChunk = nullptr;

  // A list of wrapped symbols.
  std::vector<WrappedSymbol> wrapped;
  // The names /wrap in .drectve sections asks for. Once the wrapped symbols
  // are created, a directive also wraps at once.
  std::vector<StringRef> directiveWraps;
  bool wrapsAdded = false;

  // Used for /alternatename.
  std::map<StringRef, StringRef> alternateNames;

  // Used for /aligncomm.
  std::map<std::string, int> alignComm;

  // Returns the import whose DLL's instance of the vague entity named name
  // stands for the image's, or null where the image has its own.
  DefinedImportData *getOfferOwner(StringRef name);

  // Returns the definition of sym, a name of a vague entity, where the image
  // keeps the entity: where it is live and no other image's stands for it.
  DefinedRegular *getKeptEntity(Symbol *sym);

  // Makes a DLL export each vague entity it keeps, and forward each that it
  // imports to the DLL that keeps it. Liveness is decided before, so an offer
  // keeps nothing.
  void addOffers();

  void addNamedExports();
  void fixupExports();
  void assignExportOrdinals();
  void parseModuleDefs(StringRef path);
  void parseAlternateName(StringRef);
  void parseAligncomm(StringRef);

  // The symbols in the table when the walk begins, in the order they entered
  // it; the walk may enter more. An archive's symbol enters only once its name
  // is looked up, so findAll is the walk that finds lazy symbols by name.
  auto getSymbols() {
    return llvm::map_range(llvm::seq(size_t(0), symVector.size()),
                           [this](size_t i) { return symVector[i]; });
  }
  template <typename T> void forEachSymbol(T callback) {
    for (Symbol *sym : getSymbols())
      callback(sym);
  }

  std::vector<BitcodeFile *> bitcodeFileInstances;
  // The copies of exact-match COMDATs that met a bitcode leader, compared with
  // the leader once LTO has compiled it.
  std::vector<std::pair<Symbol *, SectionChunk *>> exactMatchCopies;

  DefinedRegular *loadConfigSym = nullptr;
  uint32_t loadConfigSize = 0;
  void initializeLoadConfig();

  // Under /funcoverride, the X_$fo_bdd$ sections of the objects read, and the
  // function overrides they make.
  std::vector<SectionChunk *> functionOverrideDiagrams;
  std::vector<FunctionOverride *> functionOverrides;
  void initializeFunctionOverrides();

  std::string printSymbol(Symbol *sym) const;

private:
  /// Given a name without "__imp_" prefix, returns a defined symbol
  /// with the "__imp_" prefix, if it exists.
  Defined *impSymbol(StringRef name);
  /// Inserts symbol if not already present.
  std::pair<Symbol *, bool> insert(StringRef name);
  /// Same as insert(Name), but also sets isUsedInRegularObj.
  std::pair<Symbol *, bool> insert(StringRef name, InputFile *f);

  bool findUnderscoreMangle(StringRef sym);
  std::vector<Symbol *> getSymsWithPrefix(StringRef prefix);

  // Each name's index in symVector, as in lld/ELF.
  llvm::DenseMap<llvm::CachedHashStringRef, int> symMap;
  SmallVector<Symbol *, 0> symVector;
  // The archive symbols whose names the table lacks, the first of each name.
  // An archive read once the machine is known enters a lazy symbol only when
  // its name is looked up or inserted: most of an import library's are never
  // referenced.
  llvm::DenseMap<llvm::CachedHashStringRef,
                 std::pair<ArchiveFile *, Archive::Symbol>>
      archiveSymbols;
  // The lazy objects that a weak external kept out, by the names they define.
  llvm::DenseMap<llvm::CachedHashStringRef, InputFile *> lazyObjects;
  std::unique_ptr<BitcodeCompiler> lto;
  std::vector<std::pair<Symbol *, Symbol *>> entryThunks;
  llvm::DenseMap<Symbol *, Symbol *> exitThunks;

  void reportProblemSymbols(const llvm::SmallPtrSetImpl<Symbol *> &undefs,
                            bool needBitcodeFiles);
  void reportUndefinedSymbol(const UndefinedDiag &undefDiag);
};

std::vector<std::string> getSymbolLocations(ObjFile *file, uint32_t symIndex);

StringRef ltrim1(StringRef s, const char *chars);

} // namespace lld::coff

#endif
