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
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <optional>
#include <vector>

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
class ThreadLocalRecordChunk;

// This data structure is instantiated for each -wrap option.
struct WrappedSymbol {
  Symbol *sym;
  Symbol *real;
  Symbol *wrap;
};

struct UndefinedDiag;

// Whether a COMDAT definition named name may be shared along import paths under
// -import-slots, so the image uses its importing DLL's instance.
bool isShareableVagueName(StringRef name);

// Whether name is a type_info object: shared along import paths like other
// vague entities, but images no link joins keep their own, compared by name.
bool isTypeInfoName(StringRef name);

// The DLL whose import library offers lazy symbol s as an import of the given
// type, or empty if s is not one.
StringRef getLazyImportDLL(Symbol *s, llvm::COFF::ImportType type);

// The name of the guard variable of the variable named name.
std::string getGuardName(StringRef name);

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

  // Under -import-slots, loads the archive member behind each undefined __imp_X
  // whose lazy X no input defines, references an X that /alternatename defines,
  // and loads the import an import library offers for each weak reference.
  bool loadLocalImportMembers();

  // Under -import-slots, loads each DLL-exported import for an interior address
  // a word of static data holds, and NtProtectVirtualMemory to reseal read-only
  // words. Loading may change what a word needs.
  bool loadInteriorImports();
  // Incremental loadInteriorImports state: objects walked, pending words (by
  // section and relocation), and whether a loaded member defines a referenced
  // symbol.
  size_t interiorImportObjects = 0;
  std::vector<std::pair<SectionChunk *, size_t>> pendingInteriorWords;
  bool interiorImportsRewalk = false;
  bool importUnresolvedFromHost();
  // Redirects accesses to the .refptr.<name> pointer to sym's import imp, which
  // holds the same address, and drops the pointer.
  void replaceRefPtr(Symbol *sym, StringRef name, Defined *imp);

  // Under -import-slots, the import of the name that the DLL of imp exports
  // for the address offset bytes into imp's data, or null if it exports none.
  DefinedImportData *findInteriorImport(DefinedImportData *imp, int64_t offset);

  // Under -import-slots, exports the interior-address names of each exported
  // definition, whatever exported it.
  void exportInteriorNames();

  // The .refptr.X symbols objects define, visited by bindPointerCells.
  std::vector<Symbol *> pointerCells;

  // Under -import-slots, the extern_weak weak externals. An import library's
  // offer satisfies them, as an ELF shared object does, though no member does.
  std::vector<Symbol *> weakRefs;

  // Under -import-slots, the leaders of the data COMDAT groups a DLL's instance
  // may preempt, visited by loadLocalImportMembers and preemptWeakData.
  std::vector<Symbol *> weakData;
  // Under -import-slots, the image's sealed values: the word __imp_X$val, which
  // the fill writes with the word import X holds, and __imp_X$init, which it
  // writes with what X returns when called with the image's base. A copy of
  // another image's cell holds, until then, a thunk through the import of its
  // function, early.
  struct SealedValue {
    Chunk *word;
    DefinedImportData *imp;
    bool init;
    ImportThunkChunk *early = nullptr;
  };
  std::vector<SealedValue> sealedValues;
  void bindSealedValues();
  // Another image's indirect functions, each import with the copy of its cell
  // that the image's calls of it read.
  llvm::DenseMap<DefinedImportData *, Defined *> importedCells;
  bool addImportedCellValues();
  // Exports each exported indirect function's cell.
  void exportIFuncCells();
  static bool isSealedValueName(StringRef name);
  static StringRef sealedValueImport(StringRef name);

  // The thread-local variables bitcode defines, not shown thread-local pre-LTO.
  llvm::DenseSet<Symbol *> threadLocalBitcodeData;

  // Whether s is a thread-local variable: reached through its record where
  // another image may define it, and shared along import paths by that name.
  bool isThreadLocal(Symbol *s) const;
  // The name under which the vague entity s is shared along import paths.
  StringRef getSharedName(Symbol *s);
  // The name under which the guard of the vague variable d is shared.
  std::string getSharedGuardName(Symbol *d);
  // The vague entity shared under name: the thread-local variable whose record
  // has that name, where the image writes it, or else the symbol of that name.
  Symbol *findVagueEntity(StringRef name);

  // Under -import-slots, var's record, through which code reaches it where
  // another image may define it: the one an input defines, or one the image
  // writes. Reports an error and returns null if the image cannot write it.
  Defined *getThreadLocalRecord(DefinedRegular *var);
  // The thread-local variable whose record the image writes as s, if any.
  DefinedRegular *getRecordVariable(Symbol *s);
  // What the words of record, a thread-local variable's record in the image,
  // hold: the address of the image's TLS index, then the offsets in the TLS
  // template of the variable and of its guard. Empty for any other symbol.
  struct ThreadLocalRecordWords {
    Defined *index, *var, *guard;
  };
  std::optional<ThreadLocalRecordWords>
  getThreadLocalRecordWords(Defined *record);
  // Keeps a record the image writes, once liveness shows the link needs it.
  void keepThreadLocalRecord(Defined *record);
  // The records the image writes, kept if something live refers to them.
  std::vector<ThreadLocalRecordChunk *> threadLocalRecords;

  // Under -import-slots, makes each offered COMDAT data definition the DLL's
  // instance, as on ELF: its group's external symbols and guard resolve to the
  // DLL's imports and its sections are discarded. Runs once liveness decided.
  void preemptWeakData();

  // Makes sym, a definition of data, the instance that imp imports.
  void preemptSymbol(Symbol *sym, DefinedImportData *imp);

  // Under -import-slots, keeps each read-only copy of preempted data for the
  // loads its objects describe as only reading values: those take the copy's
  // address, whose bytes match the instance. Runs once offered functions bind.
  void readImmutableDataLocally();

  // Whether data section sc reads the same in every image: read-only, with
  // words holding only addresses that are the same everywhere.
  bool isCanonicalCopy(SectionChunk *sc);

  // A symbol for the copy of data that d defines, which no other file names.
  static DefinedRegular *makeCopy(DefinedRegular *d);

  // The copies of data that preemptWeakData discarded that may still serve
  // value reads, a symbol for each by the import that preempted it.
  llvm::DenseMap<DefinedImportData *, DefinedRegular *> immutableCopies;

  // The copies that accesses which only read may read directly where their
  // load stays, by the symbol that the load's relocation names.
  llvm::DenseMap<Symbol *, Defined *> immutableReads;

  // Under -import-slots, binds each import library's __imp_X to the image's own
  // definition of X, which beats it as on ELF, so import-form references reach
  // it locally. What the image takes from an offering DLL keeps the import; an
  // offer of a delay-loaded or ignored DLL is not taken. Runs before liveness.
  void bindImportsToDefinitions();

  // The forwarders objects' link records list, each jumping only through an
  // import pointer, with that pointer.
  struct Forwarder {
    ObjFile *file;
    Symbol *forwarder;
    Symbol *pointer;
  };
  std::vector<Forwarder> forwarders;

  // The indirect functions objects' link records list, each a stub that jumps
  // through its import pointer, which the image's fill writes with what the
  // resolver returns, and which holds the fallback's address until then.
  struct IFunc {
    ObjFile *file;
    Symbol *function;
    Symbol *resolver;
    Symbol *fallback = nullptr;
  };
  std::vector<IFunc> ifuncs;
  // The indirect function d, as the object defining d lists it.
  const IFunc *getIFunc(Defined *d);

  // The functions objects' link records list whose body Windows selects as it
  // maps the image: each a stub that jumps to its default body, the bodies it
  // may jump to instead, and the diagram that chooses among them.
  struct FunctionOverride {
    ObjFile *file;
    Symbol *function;
    SmallVector<Symbol *, 4> candidates;
    SmallVector<std::array<uint32_t, 3>, 8> nodes;
  };
  std::vector<FunctionOverride> functionOverrides;

  // Replaces each forwarder whose import pointer reaches an image definition by
  // that definition, so references reach it directly and liveness discards the
  // forwarder; one to an import stays. Runs before liveness.
  void bindForwarders();

  // Whether the image does not take imp's DLL's offers: a delay-load thunk is
  // not a function's address, and -ignore-offers names DLLs whose offers the
  // image keeps its own copies of.
  bool ignoresOffersOf(DefinedImportData *imp) const;

  // The symbols preemptWeakData resolved to imports, whose described
  // address-takes become loads of the imports. Kept in preemption order so the
  // offers made from them do not depend on pointer-set hashing.
  llvm::SetVector<Symbol *> preemptedData;

  // Imports preemptWeakData's discarded sections refer to; dropped if unused.
  llvm::DenseSet<ImportFile *> preemptedGroupImports;
  // The guards of preempted data that liveness discarded, which the image
  // offers with the data as it forwards it.
  llvm::SetVector<DefinedImportData *> forwardedGuards;

  // Under -import-slots, the leaders of the default-visibility function COMDAT
  // groups whose addresses objects take, which a DLL may offer, and the count
  // of objects whose address-significance tables have been read for them.
  std::vector<Symbol *> weakFunctions;
  size_t weakFunctionObjects = 0;
  // The counts of indexed archives and bitcode files loadLocalImportMembers
  // has scanned for offers.
  size_t offeredArchives = 0;
  size_t offeredBitcodeFiles = 0;

  // Under -import-slots, binds to a DLL's offered instance each function whose
  // address the image takes, as on ELF: the address is the DLL's, while calls
  // keep the image's copy. Runs once liveness is decided.
  void bindOfferedFunctions();

  // Under -import-slots in a DLL, whether s is a vague entity another image may
  // preempt at run time: data (with its group and guard) and address-
  // significant functions, as on ELF. The DLL reaches it through cells its fill
  // writes, unless -Bsymbolic keeps it local; TLS is preemptible with its
  // record. Calls stay direct.
  bool isPreemptibleVagueEntity(Symbol *s);
  std::optional<llvm::DenseSet<Symbol *>> preemptibleVagueEntities;

  // Under -import-slots in a DLL with a fill, whether s is a type_info object
  // it keeps with default visibility, whose name word the fill binds to that
  // of the program's instance where both describe the same type, so that
  // images that no link joins compare one word. The DLL's references to it stay
  // direct: nothing reads its address but the comparison.
  bool isBoundTypeInfo(Symbol *s);
  // Whether the image's start-up calls __llvm_run_fill, so that the image can
  // have a fill: referenced, or defined by the linker for that reference.
  bool startCallsFill();

  // Under -import-slots in an executable, for each vague entity two images of
  // its load closure keep preemptible, imports its owner's instance (from
  // chooseVagueOwners) or points to its own, and names it in the override
  // table's block, whose entry every DLL's fill copies into its cells.
  bool bindVagueSiblings();
  llvm::MapVector<StringRef, Defined *> vagueOwners;

  // Under -import-slots, in a link that writes an import library, exports each
  // kept default-visibility COMDAT definition another image may observe (data
  // and address-significant functions), as an ELF dynamic symbol table does.
  // What the image takes from an offering DLL is exported as a forwarder to it.
  void addOffers();

  // In a DLL under -import-slots, turns each export that strongly defines an
  // overridable default into an offer under the offer name, so the executable's
  // link selects it rather than the DLL binding importers to its own.
  void offerOverridableDefaults();

  // The strong definition of an overridable default that e exports, which a
  // DLL offers under the offer name, or null.
  Defined *getOverrideOffer(const Export &e);

  // Under -import-slots in a DLL that offers an overridable default, loads the
  // default's import, through which the DLL's own references reach the
  // program's choice. Called in the resolution loop.
  bool loadOverrideOfferImports();

  // The definitions this DLL offers, each with the default's import its own
  // references reach, so a definition the program does not select is preempted,
  // as on ELF; and those it gives up to its joint link's executables, each with
  // the import of theirs.
  llvm::MapVector<Symbol *, DefinedImportData *> preemptedOffers;

  // Under -import-slots in an executable, materialises its import libraries'
  // offer symbols, records each offered default's ABI name in overrideOffers,
  // and pulls the offers the executable does not itself replace. Called in the
  // resolution loop.
  bool loadOverrideOffers();

  // Under -import-slots in an executable, after resolution reads each replaced
  // default's slot (recorded by the runtime's definitions or import library)
  // into overrideReplacements.
  void resolveOverridableReplacements();

  // An overridable default that this executable replaces: the body B of the
  // replacement, its ABI name and its slot in the override table.
  struct OverrideReplacement {
    Defined *body;
    StringRef name;
    uint32_t slot;
  };
  std::vector<OverrideReplacement> overrideReplacements;
  // The defaults this executable replaces by name, before buildOverrideSlots
  // lays them out in overrideBlock.
  std::vector<std::pair<StringRef, Defined *>> overrideReplacementsByName;

  // The override table this executable writes, by slot: each entry's body (or
  // null) and whether its default is replaced (recorded in the anchor's mask)
  // or only forwarded. Empty when nothing is replaced. overrideMask is the
  // anchor's mask; overrideAnchor holds the anchor, reached by name at runtime.
  struct OverrideSlot {
    Defined *target = nullptr;
    bool replaced = false;
  };
  std::vector<OverrideSlot> overrideSlots;
  uint32_t overrideMask = 0;
  Chunk *overrideAnchor = nullptr;
  // The thunks for forwarded slots whose target drops the second argument.
  std::vector<Chunk *> overrideThunks;

  // Under -import-slots, once records are loaded and an input defines a
  // replacement, names __ovr_check (the start-up validation) and defines
  // __ovr_anchor at the anchor chunk the writer fills.
  bool nameOverrideCheck();

  // Lays the replacements out by slot for the writer, marking each body a root.
  void buildOverrideSlots();

  // The overridable defaults inputs define, from /overridable: directives.
  llvm::DenseSet<StringRef> overridables;

  // A default's fixed slot and, for a default the standard specifies as calling
  // another, that other's slot and whether it drops the second argument; and
  // whether an input defines the default rather than an import library.
  struct OverrideSlotFacts {
    uint32_t slot;
    std::optional<uint32_t> forward;
    bool dropsSecond;
    bool own;
  };
  llvm::DenseMap<StringRef, OverrideSlotFacts> overrideSlotFacts;
  // Records the facts that an input's /overridable: directive gives, or that
  // an import library records, and reports a conflict between two of them.
  void addOverrideSlotFacts(StringRef name, OverrideSlotFacts facts,
                            const Twine &source);
  // Whether an input of this link defines the default name with a slot.
  bool isOwnSlotDefault(StringRef name) const {
    auto it = overrideSlotFacts.find(name);
    return it != overrideSlotFacts.end() && it->second.own;
  }

  // Overridable defaults with no fixed slot, replaced by name: those inputs
  // define (from /overridablebyname: directives) and those the linked import
  // libraries' DLLs define (from their records).
  llvm::DenseSet<StringRef> overridablesByName;
  llvm::DenseSet<StringRef> defaultsByName;

  // The vtables of classes whose exact dynamic_casts compare vtable pointers,
  // from /exactcast: directives, of which a joint link keeps one copy; and,
  // as a member of one, the first symbols of the groups the image gives up for
  // another member's, which their imports preempt, and the symbols preempted
  // so, which the image does not offer.
  llvm::DenseSet<StringRef> exactCastVTables;
  // The addresses inside data another image may define that the image's
  // static data holds, by name and offset, from /interior: directives, which
  // a joint link's merge reads from bitcode before LTO.
  llvm::DenseSet<std::pair<StringRef, uint64_t>> interiorDemands;
  llvm::SetVector<Symbol *> jointPreempted;
  llvm::DenseSet<Symbol *> jointGivenUp;
  // As a DLL member of one, the functions it exports whose definitions the
  // executables that load it define too, which take their place in its own
  // references, as an ELF executable's definition preempts a shared object's.
  llvm::DenseSet<Symbol *> jointInterposed;
  // Reads the overridable defaults that the import libraries the link names
  // record, by slot and by name, again only once more libraries have been read.
  size_t overrideRecordsArchives = 0;
  void readOverrideRecords();

  // The record members of the import libraries the link names, in link order.
  SmallVector<MemoryBufferRef, 4> getImportLibraryRecords();

  // A default replaced by name in the table's block: its body, key and bucket,
  // or -1 for the overflow list, which follows the bucketed entries in bucket
  // order.
  struct OverrideByName {
    Defined *target;
    StringRef name;
    uint64_t key;
    int32_t bucket;
    // Whether target is a word that holds the address, as for a vague entity.
    bool indirect = false;
  };
  std::vector<OverrideByName> overrideBlock;

  // The ABI names of the overridable defaults linked import libraries offer.
  // The executable selects an offer for a slot it does not replace itself.
  llvm::DenseSet<StringRef> overrideOffers;
  bool overrideOffersLoaded = false;

  // Whether name is defined by this executable's own inputs as a strong
  // definition, the body that replaces the runtime's weak overridable default.
  bool isLocalOverrideReplacement(StringRef name);

  // A default's override-table slot, from the facts its definition or import
  // library records, or none if name is not such a default. An image taking
  // such an import's address carries a cell for the slot, written by its fill.
  std::optional<uint32_t> overrideImportSlot(StringRef name);

  // The name of the epoch export of the entities the image keeps, if any.
  StringRef offersEpoch;
  llvm::MapVector<Symbol *, DefinedImportData *> offeredFunctions;

  // Exports that forward to a DLL's instance, by its import, which the image's
  // import library names in their place so an importer binds to that DLL.
  llvm::DenseMap<StringRef, DefinedImportData *> forwardedImports;

  // Define placeholders for referenced, undefined __start_X/__stop_X of each
  // section run X, and for _etext, _edata and _end; the writer places them once
  // the output sections are laid out.
  void addStartStopSymbols();
  void addBoundarySymbols();

  // Load lazy objects that are needed for MinGW automatic import and for
  // doing stdcall fixups.
  void loadMinGWSymbols();
  bool handleMinGWAutomaticImport(Symbol *sym, StringRef name);

  // Returns a symbol for a given name. Returns a nullptr if not found.
  Symbol *find(StringRef name);
  Symbol *findUnderscore(StringRef name);
  // Like find(), but returns only a symbol the table already holds, without
  // entering an indexed archive's lazy symbol, so a probe leaves it unchanged.
  Symbol *findNonInserting(StringRef name) {
    return symMap.lookup(llvm::CachedHashStringRef(name));
  }
  // Loads, after resolution, the import member that lazy names, keeping its
  // import in the image, and returns the import.
  DefinedImportData *loadImportMember(LazyArchive *lazy);
  // Drops the exports whose names isReserved accepts, an error unless they
  // come from exporting everything.
  void reserveExportNames(llvm::function_ref<bool(StringRef)> isReserved,
                          StringRef purpose);

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
  // The names of the indexed archives' symbols that start with prefix, which
  // the table may not hold.
  std::vector<StringRef> getArchiveSymbolNames(StringRef prefix);
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
  // The placeholders addBoundarySymbols defined, with whether its ref was weak.
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

  // Iterates symbols in non-determinstic hash table order. The callback must
  // not enter a symbol; it uses findNonInserting, which does not mutate it.
  template <typename T> void forEachSymbol(T callback) {
    for (auto &pair : symMap)
      callback(pair.second);
  }

  std::vector<BitcodeFile *> bitcodeFileInstances;

  DefinedRegular *loadConfigSym = nullptr;
  uint32_t loadConfigSize = 0;
  void initializeLoadConfig();

  std::string printSymbol(Symbol *sym) const;

  void collectWeakDataGroup(DefinedRegular *d,
                            llvm::SmallVectorImpl<SectionChunk *> &chunks,
                            llvm::SmallVectorImpl<DefinedRegular *> &members,
                            bool liveOnly);

private:
  /// Given a name without "__imp_" prefix, returns a defined symbol
  /// with the "__imp_" prefix, if it exists.
  Defined *impSymbol(StringRef name);
  // Makes each export of a symbol that getInstance gives a DLL's instance for
  // a forwarder to that instance.
  void
  forwardExports(llvm::function_ref<DefinedImportData *(Symbol *)> getInstance);
  // The name of the record of the thread-local variable s, which the link
  // keeps once, since every pass that shares s asks for it.
  StringRef getRecordName(Symbol *s);
  llvm::DenseMap<Symbol *, StringRef> recordNames;
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
  // The indexed archives, in read order. A name the table lacks is the lazy
  // symbol of the first of them that defines it.
  std::vector<ArchiveFile *> indexedArchives;
  // Every name an indexed archive defines, keyed to the first archive that does
  // and its symbol's position, so a name the table lacks is found in one probe.
  llvm::DenseMap<llvm::CachedHashStringRef, std::pair<ArchiveFile *, uint32_t>>
      indexedSymbols;
  // Symbols created undefined with the __imp_ prefix under -import-slots,
  // which loadLocalImportMembers visits instead of the whole table.
  std::vector<Symbol *> impUndefs;
  // Under -import-slots: symbols a non-weak-external reference made, and the
  // lazy archive member a weak external kept reachable for one. COFF records no
  // per-reference strength and a weak external consumes the lazy symbol, so
  // without this a later strong reference would resolve to the weak fallback
  // instead of pulling the member ELF would.
  llvm::DenseSet<Symbol *> stronglyReferenced;
  llvm::DenseMap<Symbol *, std::pair<ArchiveFile *, llvm::object::Archive::Symbol>>
      weakFallbackLazy;
  void pullWeakFallbackLazy(Symbol *s);
  bool isImportedStrongly(StringRef name);
  void readImportReferences();
  // The names whose import pointers a reference other than a weak one names,
  // and those the objects' weak externals name, once read.
  std::optional<llvm::DenseSet<StringRef>> strongImportPointers;
  std::optional<llvm::DenseSet<StringRef>> weakExternalNames;
  // The undefined names the objects declare as functions, once read.
  std::optional<llvm::DenseSet<StringRef>> functionNames;
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
