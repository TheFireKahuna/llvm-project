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
class DefinedImportData;
class DefinedLocalImport;
class DefinedRegular;
class OfferTableChunk;
class OverrideAnchorChunk;
class OverrideThunk;
struct FunctionOverride;
class ImportThunkChunk;
class LazyArchive;
class LocalImportChunk;
class TlsPlaceChunk;
class TlsRecordChunk;
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

  // Once liveness is known, binds .refptr cells as local import pointers
  // where a reference can be rewritten, and notes the pointers that roots and
  // other names of a cell keep. The relocation scan finds the rest.
  void bindLocalImports();

  // Reports LNK4217 for a reference from where to li, which binds a
  // dllimport of a symbol the image defines.
  void reportLocalImport(StringRef where, DefinedLocalImport *li);

  // The .refptr cells bound as local import pointers, by section.
  llvm::DenseMap<SectionChunk *, LocalImportChunk *> cells;

  // Defines the referenced, undefined __start_X and __stop_X of each run of
  // sections named X or X$*, where X is a C identifier, and _etext, _edata and
  // _end, unprefixed too. The writer places them once the layout is known.
  void addStartStopSymbols();

  // Defines the bounds of the loader's thread-local template where no
  // foreign object does, and reports what crosses the two templates by
  // definition: a bound an object of the model defines, and a COMDAT
  // thread-local with copies of both models.
  void checkTlsTemplates();

  // Keeps wincrt's thread-end slot in an executable that has TLS callbacks
  // of its own or registers a thread-local destructor.
  void keepThreadEndSlot();

  // Load lazy objects that are needed for MinGW automatic import and for
  // doing stdcall fixups, and those that a reference to X or __imp_X from an
  // object for Windows Itanium or NT-POSIX asks for. Returns whether it loaded
  // any.
  bool loadMinGWSymbols();
  bool handleMinGWAutomaticImport(Symbol *sym, StringRef name);

  // Returns a symbol for a given name. Returns a nullptr if not found.
  Symbol *find(StringRef name);
  Symbol *findUnderscore(StringRef name);
  // Returns the import named name, which the image comes to need for the
  // reason why, loading the member of an import library that offers it: such
  // a member defines nothing else, so it is still read once the image's
  // sections are known.
  DefinedImportData *findImport(const Twine &name, const Twine &why);
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
  // Defines var's record, var's name followed by $tls, for the fact of file
  // that states it, with the guard that an object names.
  void addTlsRecord(Symbol *var, Symbol *guard, InputFile *file);

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
  // offers it, and an image that imports from that DLL binds the DLL's. A
  // thread-local variable is one by its record, which is what images share
  // of it.
  struct VagueEntity {
    VagueEntity()
        : init(0), descriptor(false), function(false), overridable(false) {}
    // The import of the address of a function with Symbol::addressImported.
    DefinedImportData *owner = nullptr;
    // The greatest llvm::COFF::LinkFactInitKind that a copy of a variable
    // states.
    uint8_t init : 3;
    // True if the entity is a type_info object.
    uint8_t descriptor : 1;
    // True if the entity is a function, whose address some object takes.
    uint8_t function : 1;
    // True if the entity is a function that an object defines to replace an
    // overridable default: a strong definition, not a default or an entity
    // that the copies of one name make.
    uint8_t overridable : 1;
  };
  llvm::MapVector<Symbol *, VagueEntity> vagueEntities;

  // The overridable defaults that have a slot in the override table, with
  // the key of their type prefixes. A link fact states one that an object
  // defines, whose bit ownOverrides has, and an import library's link
  // records those that its DLL and the images loaded with it define.
  llvm::object::COFFLinkOverrides overrides;
  uint32_t ownOverrides = 0;
  // Enters d, which from states with a type prefix of key, as the default of
  // slot. A name has one slot, one forwarding and one type prefix, a slot
  // has one name, and the prefixes are of one key.
  void addOverrideDefault(unsigned slot,
                          const llvm::object::COFFLinkOverrides::Default &d,
                          StringRef key, const Twine &from);
  // The slot of the overridable default named name, or -1.
  int findOverrideSlot(StringRef name) const;
  // Whether an executable's definition replaces an overridable default for
  // every image, and so is the address of its name in every image.
  bool replaces(const VagueEntity &e) const;
  // The anchor of the override table of an executable that replaces an
  // overridable default.
  OverrideAnchorChunk *overrideAnchor = nullptr;
  // The thunks of defaults that forward across a dropped argument.
  std::vector<OverrideThunk *> overrideThunks;
  // An overridable default that the image defines: its slot; its check
  // word, which its own calls read too; the word of its address, which is
  // made for the address-takes; and the entry that the image's own calls and
  // jumps reach, which checks the check word and transfers through it. A
  // DLL's vague entity that it keeps preemptible has only the word, its cell,
  // through which its accesses and address-takes go, and VagueSlot.
  enum : uint8_t { VagueSlot = 0xff };
  struct OwnDefault {
    uint8_t slot;
    DefinedLocalImport *check = nullptr, *address = nullptr;
    Defined *entry = nullptr;
  };
  // By where each is, which each of its names shares but a local symbol.
  llvm::MapVector<std::pair<Chunk *, uint64_t>, OwnDefault> ownDefaults;
  OwnDefault *findOwnDefault(Symbol *s);
  // Whether a DLL's offer, which the loader binds, replaces o, a default
  // that the executable defines.
  bool replacedByOffer(const OwnDefault &o) const;
  // Lists the words of the image that hold the address of an overridable
  // default, once the relocations are scanned: those of its own defaults,
  // each word that holds an import of one, and the call words of the
  // second imports of those that forward to another.
  void bindOverrideCells();
  // Returns the type ID that the prefix before the entry of sym, which name
  // is, holds where a link fact says it has one, of the layout that key
  // states.
  std::optional<uint32_t> getTypeId(DefinedRegular *sym, Symbol *name,
                                    StringRef key);

  // Enters the vague entities that the objects of other producers define,
  // which only take part in an image that also has an object for Windows
  // Itanium or NT-POSIX; foreignRead counts the objects looked at. Each
  // input that is read asks, so that an import library's pointer read after
  // a definition finds the definition's entity.
  void addForeignEntities();
  void addForeignEntities(ObjFile *file);
  size_t foreignRead = 0;
  Symbol *findOwnGuard(DefinedRegular *var);
  // The initialization function of the thread-local variable whose record
  // is the vague entity sym, where a thread initializes it: a DLL offers it
  // with the record, for a unit that only declares the variable, as on ELF.
  Symbol *findOfferedInit(Symbol *sym, const VagueEntity &entity);
  // The definition of the vague entity sym, if the link has one: sym, or the
  // variable of a record.
  DefinedRegular *getEntityDefinition(Symbol *sym);

  // Whether another image's instance may stand for the image's definition of
  // sym, a vague entity that is a function or a variable.
  bool isPreemptible(const Symbol *sym, bool function);

  // Whether the image takes the instance of a vague entity that the DLL of
  // file offers for its own. Every binding of an offer asks here, at the
  // import that carries it, so an offer the image does not take leaves no
  // import of its name.
  bool honoursOffer(const ImportFile *file);

  // The import library's pointer for name, a name of a vague entity, where
  // the image takes the offer that it carries.
  DefinedImportData *findTakenOffer(StringRef name);
  // Whether another image's instance stands for the image's definition of
  // sym, as findTakenOffer finds before the link has decided liveness.
  bool takesOffer(Symbol *sym, bool function);

  // Gives each vague entity that the link defines and a DLL offers, with
  // every name of its COMDAT group, the instance of the offer's DLL, and
  // leaves the group's sections out of the image. A function keeps its
  // definition for its calls, and only its address becomes the DLL's. It runs
  // once before LTO, which then compiles no copy of a variable but a
  // constant's, and again once every input is loaded, before liveness is
  // decided.
  void preemptVagueEntities();
  // The vague entities whose protected copy in a DLL the executable takes,
  // each name of the group from that DLL; and makes name an import from dll,
  // of a function's address where code.
  llvm::DenseSet<Symbol *> protectedOwners;
  void takeImport(StringRef name, StringRef dll, bool code);

  // The sections that preemptVagueEntities left out, which no later
  // reference brings back.
  std::vector<SectionChunk *> preempted;

  // By the import that took its name, a pointer to each name of a preempted
  // group that lies in one read-only section. The group is the image's copy
  // of constants that the import's DLL holds too.
  llvm::DenseMap<Symbol *, DefinedLocalImport *> valueCopies;

  // Makes each site that its object states to behave the same with any
  // definition of a constant refer to the pointer to the image's copy, where
  // the copy holds what the DLL's does in every word, so that the scan binds
  // it as a site of the image's own data. Every other site keeps the import,
  // and with it the one address of the constant. Liveness, which is decided
  // next, keeps a copy that a site reads.
  void bindValueOnlySites();

  // The record of each thread-local variable that a link fact states one
  // for, by the variable.
  llvm::MapVector<Symbol *, TlsRecordChunk *> tlsRecords;
  // In the owned model, the image's place word, which the records name, once
  // code names it or the scan finds that the image needs it.
  TlsPlaceChunk *placeWord = nullptr;
  // The place word and its key word, which a relaxed record chain reads.
  Defined *placeSym = nullptr, *keySym = nullptr;
  // Each thread-local that a GOTTPOFF relocation names, and the word of its
  // offset from the thread pointer that the link makes, ELF's initial-exec
  // slot; none where the link knows the offset.
  llvm::MapVector<Symbol *, Symbol *> tpoffWords;
  TlsPlaceChunk *getPlaceWord();
  void selectPlaceWord();
  void exportTlsUnit();

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
  // The runtime's function that an ensure stub calls with its DLL's list.
  Symbol *delayResolve = nullptr;
  // The runtime's function that writes the words of COFFLinkerContext::fill.
  Symbol *fill = nullptr;
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
  // The entity that the Images record and the offer table state for sym, of
  // which d is the copy: once, under the name of its definition.
  std::optional<llvm::object::COFFLinkImage::Entity>
  getImageEntity(Symbol *sym, DefinedRegular *d, const VagueEntity &entity);

  // Makes a DLL export each vague entity it keeps, and forward each that it
  // imports to the DLL that keeps it. Liveness is decided before, so an offer
  // keeps nothing. Lists each name that the image keeps in its offer table,
  // and gives a DLL a cell for each that it keeps preemptible.
  void addOffers();
  OfferTableChunk *offerTable = nullptr;
  // A DLL's baseline: the names that it exports, of which an entity that the
  // DLL keeps is no addition, and its epoch; and the DLL's own epoch, which
  // follows the baseline's while the DLL keeps nothing that it did not.
  std::optional<llvm::StringSet<>> baseline;
  uint64_t baselineEpoch = 0, offersEpoch = 0;
  // How many exports addOffers made.
  size_t offerExports = 0;
  // Whether a DLL offers sym, a vague entity that it keeps, since its
  // baseline, which exports no such name.
  bool isAddition(Symbol *sym) const;

  // Points an export at the DLL that exports what it names, as a PE
  // forwarder: to owner's own export, or, where not own, to the export of
  // e's name there. Fails, with an error, where the loader could not read the
  // forwarder.
  bool forwardExport(Export &e, Symbol &sym, DefinedImportData *owner,
                     StringRef what, bool own = true);

  // Makes an export that a fork object's weak external binds to another DLL's
  // function, a destructor forwarder over an imported base, a forwarder to
  // that DLL's export, before liveness, so that the image holds no thunk for
  // it. A foreign object's export keeps link.exe's thunk.
  void forwardImportedExports();

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
  // A thread-local COMDAT's leader and the file of each of its other copies.
  std::vector<std::pair<DefinedRegular *, InputFile *>> tlsCopies;

  DefinedRegular *loadConfigSym = nullptr;
  uint32_t loadConfigSize = 0;
  void initializeLoadConfig();

  // The X_$fo_bdd$ and X_$fo_rvas$ sections of the objects read, under
  // /funcoverride or in an object that LLVM made for Windows Itanium or
  // NT-POSIX, and the function overrides they make.
  std::vector<SectionChunk *> functionOverrideSections;
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

// The guard variable's name of the vague variable named var, an Itanium
// special name, where its initialization kind, a LinkFactInitKind, has one.
std::optional<std::string> getGuardName(StringRef var, uint64_t init);

// An entity's shape, in the words of a diagnostic.
std::string describeShape(const llvm::object::COFFLinkImage::Entity &e);

} // namespace lld::coff

#endif
