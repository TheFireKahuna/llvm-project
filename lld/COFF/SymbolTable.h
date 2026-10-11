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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
struct LTOCodeGenerator;
namespace RTLIB {
struct RuntimeLibcallsInfo;
}
}

namespace lld::coff {

class Chunk;
class CommonChunk;
class COFFLinkerContext;
class Defined;
class DefinedAbsolute;
class DefinedRegular;
class ImportThunkChunk;
class LazyArchive;
class SameAddressThunkARM64EC;
class SectionChunk;
class Symbol;

// This data structure is instantiated for each -wrap option.
struct WrappedSymbol {
  Symbol *sym;
  Symbol *real;
  Symbol *wrap;
};

struct UndefinedDiag;

// Whether a definition in a COMDAT named name may be shared along import paths
// under -import-slots, so that the instance of a DLL the image imports it from
// is the image's.
bool isShareableVagueName(StringRef name);

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

  // Under -import-slots, load the archive member behind each undefined __imp_X
  // that no input defines under that name and whose X is lazy, and reference
  // an X that /alternatename defines, as a direct reference to X would; and
  // load the import that an import library offers for each weak reference.
  // Returns whether any member was loaded or X referenced.
  bool loadLocalImportMembers();

  // Under -import-slots, loads the import of each name that a DLL exports for
  // an address inside its data which a word of static data holds, and
  // NtProtectVirtualMemory's where the residual fill, which writes a word for
  // which no name is exported, would make a read-only word read-only again.
  // Returns whether it loaded any, which may change what a word needs.
  bool loadInteriorImports();

  // Under -import-slots, the import of the name that the DLL of imp exports
  // for the address offset bytes into imp's data, or null if it exports none.
  DefinedImportData *findInteriorImport(DefinedImportData *imp, int64_t offset);

  // Under -import-slots, exports the names of the addresses inside each
  // exported definition along with it, whatever exported the definition.
  void exportInteriorNames();

  // The .refptr.X symbols that objects define where bindPointerCells may
  // bind them, which it visits instead of the whole table.
  std::vector<Symbol *> pointerCells;

  // Under -import-slots, the weak externals of extern_weak declarations, whose
  // default is absolute zero: an import library's offer satisfies them, as a
  // shared object satisfies an ELF weak reference, though no archive member
  // does.
  std::vector<Symbol *> weakRefs;

  // Under -import-slots, the leaders of the COMDAT groups of data definitions
  // that the instance of a DLL the image imports from may preempt, which
  // loadLocalImportMembers and preemptWeakData visit instead of the whole
  // table.
  std::vector<Symbol *> weakData;

  // Under -import-slots, makes each COMDAT data definition that an import
  // library offers the DLL's instance, as an ELF shared object's definition
  // preempts another copy of vague-linkage data: every external symbol of its
  // group, and its guard, resolve to the DLL's same-named imports, and the
  // group's sections are discarded. Runs once liveness is decided.
  void preemptWeakData();

  // Makes sym, a definition of data, the instance that imp imports.
  void preemptSymbol(Symbol *sym, DefinedImportData *imp);

  // Under -import-slots, binds each import library's __imp_X to the image's
  // own definition of X, which beats it, as a definition in an ELF executable
  // beats a shared object's: the import-form references reach the definition
  // locally. What the image takes from a DLL that offers it keeps the import;
  // an offer of a delay-loaded or ignored DLL is not taken. Runs before
  // liveness is decided, which keeps the definition.
  void bindImportsToDefinitions();

  // The forwarders that objects' link records list, each a function that
  // only jumps through an import pointer, with that pointer.
  struct Forwarder {
    ObjFile *file;
    Symbol *forwarder;
    Symbol *pointer;
  };
  std::vector<Forwarder> forwarders;

  // Replaces each forwarder whose import pointer reaches a definition in the
  // image by that definition, so that every reference to the forwarder
  // reaches the definition directly, and liveness discards the forwarder; a
  // forwarder to an import stays. Runs before liveness is decided.
  void bindForwarders();

  // Whether the image does not take what imp's DLL offers: data is never
  // delay-bound, a delay-load thunk is not a function's address, and
  // -ignore-offers names DLLs whose offers the image keeps its copies of.
  bool ignoresOffersOf(DefinedImportData *imp) const;

  // The symbols that preemptWeakData resolved to imports, whose addresses that
  // code takes in a described instruction become loads of their imports.
  llvm::DenseSet<Symbol *> preemptedData;

  // The imports that the sections preemptWeakData discarded refer to, which
  // need no entry if nothing else uses them.
  llvm::DenseSet<ImportFile *> preemptedGroupImports;

  // Under -import-slots, the leaders of the COMDAT groups of functions with
  // default visibility whose addresses objects take, which a DLL the image
  // imports from may offer, and the number of objects whose
  // address-significance tables have been read for them.
  std::vector<Symbol *> weakFunctions;
  size_t weakFunctionObjects = 0;

  // Under -import-slots, the functions whose address is that of the instance
  // a DLL the image imports from offers, as an ELF shared object's definition
  // preempts another copy of an inline function: every address the image
  // takes is the DLL's, while calls keep the image's copy. Runs once liveness
  // is decided.
  void bindOfferedFunctions();

  // Under -import-slots, in a link that writes an import library, exports
  // each definition with default visibility in a COMDAT that the image keeps
  // and another image may observe, as an ELF shared object's dynamic symbol
  // table gives it: data, and functions whose addresses are significant. What
  // the image takes from a DLL that offers it is exported as a forwarder to
  // that DLL.
  void addOffers();

  // The name of the epoch export of the entities the image keeps, if any.
  StringRef offersEpoch;
  llvm::MapVector<Symbol *, DefinedImportData *> offeredFunctions;

  // The exports that forward to the instance of a DLL the image imports, by
  // the import of that instance, which the image's import library names in
  // their place so that an importer binds to the DLL that holds it.
  llvm::DenseMap<StringRef, DefinedImportData *> forwardedImports;

  // Define placeholders for the referenced, undefined __start_X and __stop_X
  // of each section run X, and for _etext, _edata and _end. The writer places
  // them once the output sections are laid out.
  void addStartStopSymbols();
  void addBoundarySymbols();

  // Load lazy objects that are needed for MinGW automatic import and for
  // doing stdcall fixups.
  void loadMinGWSymbols();
  bool handleMinGWAutomaticImport(Symbol *sym, StringRef name);

  // Returns a symbol for a given name. Returns a nullptr if not found.
  Symbol *find(StringRef name);
  Symbol *findUnderscore(StringRef name);

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
  void addLazyArchive(ArchiveFile *f, const Archive::Symbol &sym);
  // Adds an archive whose symbols are looked up in its index when needed,
  // which loads the members that define the symbols waiting for them now.
  void addIndexedArchive(ArchiveFile *f);
  // Enters the lazy symbols of indexed archives whose names start with
  // prefix, so that iterating the table sees them.
  void addIndexedLazies(StringRef prefix);
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

  // The input sections named X or X$*, which __start_X and __stop_X bound.
  struct SectionRun {
    StringRef name;
    Symbol *start = nullptr;
    Symbol *stop = nullptr;
    std::vector<SectionChunk *> chunks;
  };
  std::vector<SectionRun> sectionRuns;
  // The placeholders addBoundarySymbols defined, each with whether its
  // reference was weak.
  std::vector<std::pair<Symbol *, bool>> boundarySymbols;

  // A list of DLL exports.
  std::vector<Export> exports;
  // Exports that repeat the name of one of those through EXPORTAS. The export
  // table has the name once, and the import library has their symbols too.
  std::vector<Export> exportAsAliases;
  llvm::DenseSet<StringRef> directivesExports;
  bool hadExplicitExports;

  Chunk *edataStart = nullptr;
  Chunk *edataEnd = nullptr;

  Symbol *delayLoadHelper = nullptr;
  Chunk *tailMergeUnwindInfoChunk = nullptr;

  // A list of wrapped symbols.
  std::vector<WrappedSymbol> wrapped;

  // The names that /wrap in .drectve sections wraps, and whether the wrapped
  // symbols have been instantiated, after which a directive wraps at once.
  std::vector<StringRef> directiveWraps;
  bool wrapsAdded = false;
  // The wrapped names that nothing referred to when they were to be wrapped.
  std::vector<StringRef> unreferencedWraps;
  // The wrapped symbols whose __real_ name nothing has referred to yet, by
  // index into wrapped, with that name.
  std::vector<std::pair<size_t, StringRef>> unreferencedReals;
  // The runtime library functions of the bitcode's target, which a wrap
  // counts as referenced, built on first use.
  llvm::RTLIB::RuntimeLibcallsInfo *ltoLibcalls = nullptr;

  // Used for /alternatename.
  std::map<StringRef, StringRef> alternateNames;

  // Used for /aligncomm.
  std::map<std::string, int> alignComm;

  void fixupExports();
  void assignExportOrdinals();
  void parseModuleDefs(StringRef path);
  void parseAlternateName(StringRef);
  void parseAligncomm(StringRef);

  // Iterates symbols in non-determinstic hash table order. The callback may
  // look up a symbol, which can enter an indexed archive's lazy symbol.
  template <typename T> void forEachSymbol(T callback) {
    SmallVector<Symbol *, 0> syms(llvm::make_second_range(symMap));
    for (Symbol *sym : syms)
      callback(sym);
  }

  std::vector<BitcodeFile *> bitcodeFileInstances;

  DefinedRegular *loadConfigSym = nullptr;
  uint32_t loadConfigSize = 0;
  void initializeLoadConfig();

  std::string printSymbol(Symbol *sym) const;

private:
  /// Given a name without "__imp_" prefix, returns a defined symbol
  /// with the "__imp_" prefix, if it exists.
  Defined *impSymbol(StringRef name);
  // Makes each export of a symbol that getInstance gives a DLL's instance for
  // a forwarder to that instance.
  void
  forwardExports(llvm::function_ref<DefinedImportData *(Symbol *)> getInstance);
  void collectWeakDataGroup(DefinedRegular *d,
                            llvm::SmallVectorImpl<SectionChunk *> &chunks,
                            llvm::SmallVectorImpl<DefinedRegular *> &members,
                            bool liveOnly);
  /// Inserts symbol if not already present.
  std::pair<Symbol *, bool> insert(StringRef name);
  /// Same as insert(Name), but also sets isUsedInRegularObj.
  std::pair<Symbol *, bool> insert(StringRef name, InputFile *f);

  bool findUnderscoreMangle(StringRef sym);
  std::vector<Symbol *> getSymsWithPrefix(StringRef prefix);
  // The first indexed archive that defines name, with the position of its
  // symbol, or null.
  std::pair<ArchiveFile *, uint32_t>
  findIndexed(llvm::CachedHashStringRef name);
  // Makes the new symbol s the lazy symbol of the first indexed archive that
  // defines name, if any. Returns whether it did.
  bool addIndexedLazy(Symbol *s, StringRef name);

  llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> symMap;
  // The indexed archives, in the order they were read. A name that the table
  // lacks is that of the lazy symbol of the first of them that defines it, as
  // if each had entered its lazy symbols when it was read.
  std::vector<ArchiveFile *> indexedArchives;
  // Symbols created undefined with the __imp_ prefix under -import-slots,
  // which loadLocalImportMembers visits instead of the whole table.
  std::vector<Symbol *> impUndefs;
  std::unique_ptr<BitcodeCompiler> lto;
  std::vector<std::pair<Symbol *, Symbol *>> entryThunks;
  llvm::DenseMap<Symbol *, Symbol *> exitThunks;

  void
  reportProblemSymbols(const llvm::SmallPtrSetImpl<Symbol *> &undefs,
                       const llvm::DenseMap<Symbol *, Symbol *> *localImports,
                       bool needBitcodeFiles);
  void reportUndefinedSymbol(const UndefinedDiag &undefDiag);
};

std::vector<std::string> getSymbolLocations(ObjFile *file, uint32_t symIndex);

StringRef ltrim1(StringRef s, const char *chars);

} // namespace lld::coff

#endif
