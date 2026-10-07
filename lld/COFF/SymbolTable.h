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
class ImportThunkChunk;
class KCFIOpenChunk;
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

  // Decides, after mark-live, which local import pointers a reference still
  // reads.
  void bindLocalImports();

  // Try to resolve undefined symbols with alternate names.
  void resolveAlternateNames();

  // Under -import-slots, load the archive member behind each undefined __imp_X
  // that no input defines under that name and whose X is lazy, and reference
  // an X that /alternatename defines, as a direct reference to X would; and
  // load the import that an import library offers for each weak reference.
  // Returns whether any member was loaded or X referenced.
  bool loadLocalImportMembers();

  // The import named name, __imp_X, loading the import library member that
  // offers it if it is not loaded yet, which sets loaded; or null.
  DefinedImportData *findImport(StringRef name, bool &loaded);

  // Under -import-slots, the import of the name that the DLL of imp exports
  // for the address offset bytes into imp's data, or null if it exports none,
  // as findImport finds it.
  DefinedImportData *findInteriorImport(DefinedImportData *imp, int64_t offset,
                                        bool &loaded);

  // Under -import-slots, exports the names of the addresses inside each
  // exported definition along with it, whatever exported the definition.
  void exportInteriorNames();

  // Under -import-slots, makes each .refptr.X pointer the pointer to X that
  // the link already has or can make: X's import pointer when X is imported,
  // and otherwise a local import pointer, whose described references
  // bindLocalImports rewrites to reach X, or zero, directly.
  void bindPointerCells();

  // Under -import-slots, the weak externals of extern_weak declarations, whose
  // default is absolute zero: an import library's offer satisfies them, as a
  // shared object satisfies an ELF weak reference, though no archive member
  // does.
  std::vector<Symbol *> weakRefs;

  // Under -import-slots, opens the KCFI types through which code without a
  // KCFI prefix, which the link brings in, can be reached.
  void openKCFITypes();

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
  Symbol *find(StringRef name) const;
  Symbol *findUnderscore(StringRef name) const;

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
                                   ImportLocation &location);
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

  // The routines and list words of the KCFI types that openKCFITypes opened,
  // each to be placed in its section.
  std::vector<Chunk *> kcfiChunks;
  // The imports whose address those lists name by its import address table
  // entry, which the open routines read, each with its list's section.
  std::vector<std::pair<DefinedImportData *, StringRef>> kcfiListedImports;
  // The routines that openKCFITypes made dynamic only because foreign code in
  // the image reaches them, each with the static scanner of its kind.
  std::vector<std::pair<KCFIOpenChunk *, Defined *>> kcfiLocalRoutines;

  // The input sections named X or X$*, which __start_X and __stop_X bound.
  struct SectionRun {
    StringRef name;
    Symbol *start = nullptr;
    Symbol *stop = nullptr;
    std::vector<SectionChunk *> chunks;
  };
  std::vector<SectionRun> sectionRuns;
  // The placeholders addBoundarySymbols defined.
  std::vector<Symbol *> boundarySymbols;

  // A list of DLL exports.
  std::vector<Export> exports;
  llvm::DenseSet<StringRef> directivesExports;
  bool hadExplicitExports;

  Chunk *edataStart = nullptr;
  Chunk *edataEnd = nullptr;

  Symbol *delayLoadHelper = nullptr;
  Chunk *tailMergeUnwindInfoChunk = nullptr;

  // A list of wrapped symbols.
  std::vector<WrappedSymbol> wrapped;

  // Used for /alternatename.
  std::map<StringRef, StringRef> alternateNames;

  // Used for /aligncomm.
  std::map<std::string, int> alignComm;

  void fixupExports();
  void assignExportOrdinals();
  void parseModuleDefs(StringRef path);
  void parseAlternateName(StringRef);
  void parseAligncomm(StringRef);

  // Iterates symbols in non-determinstic hash table order.
  template <typename T> void forEachSymbol(T callback) {
    for (auto &pair : symMap)
      callback(pair.second);
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
  /// Inserts symbol if not already present.
  std::pair<Symbol *, bool> insert(StringRef name);
  /// Same as insert(Name), but also sets isUsedInRegularObj.
  std::pair<Symbol *, bool> insert(StringRef name, InputFile *f);

  bool findUnderscoreMangle(StringRef sym);
  std::vector<Symbol *> getSymsWithPrefix(StringRef prefix);

  llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> symMap;
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

// The size of the type words and the marker of the KCFI prefix that a static
// __cfi_ label at off in sc begins, 12 or 16 bytes, or 0 without the marker.
uint32_t getKCFIPrefixSize(SectionChunk *sc, uint32_t off);

// Whether file was built by a compiler other than clang: it defines code and
// holds no KCFI prefix with the marker in any section, including those the
// link drops, where clang's objects keep one for every external function.
bool isKCFIForeignFile(ObjFile *file);

StringRef ltrim1(StringRef s, const char *chars);

} // namespace lld::coff

#endif
