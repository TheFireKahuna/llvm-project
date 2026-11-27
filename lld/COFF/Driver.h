//===- Driver.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_DRIVER_H
#define LLD_COFF_DRIVER_H

#include "Config.h"
#include "SymbolTable.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Reproduce.h"
#include "lld/Common/VersionScript.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TarWriter.h"
#include "llvm/WindowsDriver/MSVCPaths.h"
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace lld::coff {

using llvm::COFF::MachineTypes;
using llvm::COFF::WindowsSubsystem;
using std::optional;

class COFFOptTable : public llvm::opt::OptTable {
public:
  COFFOptTable();
};

// The result of parsing the .drective section. The /export:, /include:,
// /exclude-symbols: and /guardsym: options are handled separately because they
// reference symbols, and the number of symbols can be quite large. The LLVM
// Option library will perform at least one memory allocation per argument, and
// that is prohibitively slow for parsing directives.
struct ParsedDirectives {
  std::vector<StringRef> exports;
  std::vector<StringRef> includes;
  std::vector<StringRef> excludes;
  std::vector<StringRef> guardSyms;
  llvm::opt::InputArgList args;
};

class ArgParser {
public:
  ArgParser(COFFLinkerContext &ctx);

  // Parses command line options.
  llvm::opt::InputArgList parse(llvm::ArrayRef<const char *> args);

  // Tokenizes a given string and then parses as command line options.
  llvm::opt::InputArgList parse(StringRef s) { return parse(tokenize(s)); }

  // Tokenizes a given string and then parses as command line options in
  // .drectve section. The options that reference symbols are returned apart
  // to be processed in fastpath; with symbolsOnly, the others are dropped.
  ParsedDirectives parseDirectives(StringRef s, bool symbolsOnly = false);

private:
  // Concatenate LINK environment variable.
  void addLINK(SmallVector<const char *, 256> &argv);

  std::vector<const char *> tokenize(StringRef s);

  COFFLinkerContext &ctx;
};

class LinkerDriver {
public:
  LinkerDriver(COFFLinkerContext &ctx) : ctx(ctx) {}

  void linkerMain(llvm::ArrayRef<const char *> args);

  void addFile(InputFile *file);

  void addClangLibSearchPaths(const std::string &argv0);

  // Used by ArchiveFile to enqueue and add members.
  void enqueueArchiveMember(const Archive::Child &c, const Archive::Symbol &sym,
                            StringRef parentName);
  void addArchiveBuffer(MemoryBufferRef mbref, StringRef symName,
                        StringRef parentName, uint64_t offsetInArchive,
                        bool lazy);
  void addThinArchiveBuffer(MemoryBufferRef mbref, StringRef symName,
                            bool lazy);

  // Loads every member of lib, as /wholearchive:lib in a directive does,
  // whenever lib is read; without lib, of the libraries the command line named
  // that were read before, as a bare /wholearchive in a directive does.
  void addWholeArchives(StringRef lib = "");

  enum class InputOpt { None, DefaultLib, WholeArchive };
  void enqueuePDB(StringRef Path) { enqueuePath(Path, false); }

  MemoryBufferRef takeBuffer(std::unique_ptr<MemoryBuffer> mb);

  // The version that the version scripts give a symbol, by ELF's precedence:
  // VER_NDX_LOCAL, the index of a version node, or VER_NDX_GLOBAL, which is
  // also the answer for a name that no script has.
  uint16_t versionOf(StringRef name);
  // Whether a version script keeps a symbol out of the image's interface: no
  // object's directive and no export-all exports it.
  bool isLocalByVersionScript(StringRef name);
  // Whether --exclude-libs keeps what a file defines out of the image's
  // interface: the file is a member of an archive the option names, or of any
  // archive under ALL.
  bool isExcludedLib(const InputFile *file);
  // Decides an export that file states, by directive or under export-all:
  // false where the image's interface leaves it out.
  bool admitExport(SymbolTable &symtab, Export &e, const InputFile *file);

  void enqueuePath(StringRef path, bool lazy,
                   InputOpt inputOpt = InputOpt::None);

  // Returns a list of chunks of selected symbols.
  std::vector<Chunk *> getChunks() const;

  std::unique_ptr<llvm::TarWriter> tar; // for /linkrepro

  void pullArm64ECIcallHelper();

private:
  // Searches a file from search paths.
  std::optional<StringRef> findFileIfNew(StringRef filename);
  std::optional<StringRef> findLibIfNew(StringRef filename);
  StringRef findFile(StringRef filename);
  StringRef findLib(StringRef filename);
  StringRef findLibMinGW(StringRef filename);

  // Determines the location of the sysroot based on `args`, environment, etc.
  void detectWinSysRoot(const llvm::opt::InputArgList &args);

  // Detects the MSVC and Windows SDK library directories if that waits for a
  // search to miss and the machine type is known, and appends them to the
  // search paths. Returns whether it added any.
  bool addPendingWinSysRootLibSearchPaths();

  // Adds various search paths based on the sysroot.  Must only be called once
  // config.machine has been set.
  void addWinSysRootLibSearchPaths();

  void setMachine(llvm::COFF::MachineTypes machine);
  llvm::Triple::ArchType getArch();

  uint64_t getDefaultImageBase();

  bool isDecorated(StringRef sym);

  InputFile *addObjectFile(COFFLinkerContext &ctx, MemoryBufferRef mb,
                           StringRef archiveName, uint64_t offsetInArchive,
                           bool lazy);

  std::string getMapFile(const llvm::opt::InputArgList &args,
                         llvm::opt::OptSpecifier os,
                         llvm::opt::OptSpecifier osFile);

  std::string getImplibPath();

  // The import name is calculated as follows:
  //
  //        | LIBRARY w/ ext |   LIBRARY w/o ext   | no LIBRARY
  //   -----+----------------+---------------------+------------------
  //   LINK | {value}        | {value}.{.dll/.exe} | {output name}
  //    LIB | {value}        | {value}.dll         | {output name}.dll
  //
  std::string getImportName(bool asLib);

  // Write fullly resolved path to repro file if /linkreprofullpathrsp
  // is specified.
  void handleReproFile(StringRef path, InputOpt inputOpt);

  void createImportLibrary(bool asLib);
  // Returns the member of the import library that holds its link records.
  std::string createLinkRecords(StringRef libName);

  // Used by the resolver to parse .drectve section contents.
  void parseDirectives(InputFile *file);

  // Parse an /order file. If an option is given, the linker places COMDAT
  // sections int he same order as their names appear in the given file.
  void parseOrderFile(StringRef arg);

  void parseCallGraphFile(StringRef path);

  void parsePDBAltPath();

  // Parses LIB environment which contains a list of search paths.
  void addLibSearchPaths();

  // Library search path. The first element is always "" (current directory).
  std::vector<StringRef> searchPaths;

  // Convert resource files and potentially merge input resource object
  // trees into one resource tree.
  void convertResources();

  void maybeExportMinGWSymbols(const llvm::opt::InputArgList &args);

  // We don't want to add the same file more than once.
  // Files are uniquified by their filesystem and file number.
  std::set<llvm::sys::fs::UniqueID> visitedFiles;

  std::set<std::string> visitedLibs;

  // The libraries read, and whether the command line named them; and the file
  // names of those queued that a directive loads whole.
  std::vector<std::pair<ArchiveFile *, bool>> archives;
  llvm::StringSet<> wholeArchiveNames;

  void addBuffer(std::unique_ptr<MemoryBuffer> mb, InputOpt inputOpt,
                 bool lazy);

  void enqueueTask(std::function<void()> task);
  bool run();

  std::list<std::function<void()>> taskQueue;
  std::vector<MemoryBufferRef> resources;

  // The manifests that .res inputs give an executable holding an object for
  // Windows Itanium or NT-POSIX, which merge into the one it embeds.
  std::vector<MemoryBufferRef> resourceManifests;

  llvm::DenseSet<StringRef> excludedSymbols;
  // The archives that --exclude-libs names.
  llvm::DenseSet<StringRef> excludedLibs;

  void readVersionScripts(const llvm::opt::InputArgList &args);
  void checkVersionScripts();

  // A version script's pattern with no wildcard: its place among them in
  // ELF's order of assignment, its version, and whether an export has its
  // name.
  struct ScriptName {
    uint32_t order;
    uint16_t id;
    bool exported = false;
  };
  ScriptName *findScriptName(StringRef name);

  // The version nodes of the version scripts, behind VER_NDX_LOCAL and
  // VER_NDX_GLOBAL; empty where the link has no script.
  SmallVector<VersionDefinition, 0> versionDefinitions;
  // Their patterns with no wildcard, plain and extern "C++".
  llvm::DenseMap<llvm::CachedHashStringRef, ScriptName> scriptNames[2];
  VersionPatternMatcher scriptWildcards;

  COFFLinkerContext &ctx;

  // The arguments to detect the MSVC and Windows SDK library directories from
  // once a search misses the other paths.
  const llvm::opt::InputArgList *pendingWinSysRootArgs = nullptr;

  llvm::ToolsetLayout vsLayout = llvm::ToolsetLayout::OlderVS;
  std::string vcToolChainPath;
  llvm::SmallString<128> diaPath;
  bool useWinSysRootLibPath = false;
  llvm::SmallString<128> universalCRTLibPath;
  int sdkMajor = 0;
  llvm::SmallString<128> windowsSdkLibPath;

  // For linkreprofullpathrsp
  std::unique_ptr<llvm::raw_fd_ostream> reproFile;

  // Functions below this line are defined in DriverUtils.cpp.

  void printHelp(const char *argv0);

  // Parses a string in the form of "<integer>[,<integer>]".
  void parseNumbers(StringRef arg, uint64_t *addr, uint64_t *size = nullptr);

  void parseGuard(StringRef arg);
  // Records a /guardsym, from the command line if file is null, and binds the
  // ones recorded to their symbols in Configuration::guardSuppressed.
  void parseGuardSym(StringRef arg, InputFile *file);
  void resolveGuardSyms();
  std::vector<std::pair<StringRef, InputFile *>> guardSyms;

  // Parses a string in the form of "<integer>[.<integer>]".
  // Minor's default value is 0.
  void parseVersion(StringRef arg, uint32_t *major, uint32_t *minor);

  // Parses a string in the form of "<subsystem>[,<integer>[.<integer>]]".
  void parseSubsystem(StringRef arg, WindowsSubsystem *sys, uint32_t *major,
                      uint32_t *minor, bool *gotVersion = nullptr);

  void parseMerge(StringRef);
  void parsePDBPageSize(StringRef);
  void parseSection(StringRef);
  void parseSectionStream(StringRef);
  void parseSectionLayout(StringRef);

  void parseSameAddress(StringRef);

  // Parses a MS-DOS stub file
  void parseDosStub(StringRef path);

  // Parses a string in the form of "[:<integer>]"
  void parseFunctionPadMin(llvm::opt::Arg *a);

  // Parses a string in the form of "[:<integer>]"
  void parseDependentLoadFlags(llvm::opt::Arg *a);

  // Parses a string in the form of "EMBED[,=<integer>]|NO".
  void parseManifest(StringRef arg);

  // Parses a string in the form of "level=<string>|uiAccess=<string>"
  void parseManifestUAC(StringRef arg);

  // Parses a string in the form of "cd|net[,(cd|net)]*"
  void parseSwaprun(StringRef arg);

  // Create a resource file containing a manifest XML.
  std::unique_ptr<MemoryBuffer> createManifestRes();
  void takeManifestResources();
  void createSideBySideManifest();
  std::string createDefaultXml();
  std::string createManifestXmlWithInternalMt(StringRef defaultXml);
  std::string createManifestXmlWithExternalMt(StringRef defaultXml);
  std::string createManifestXml();

  std::unique_ptr<llvm::WritableMemoryBuffer>
  createMemoryBufferForManifestRes(size_t manifestRes);

  // Used for dllexported symbols.
  Export parseExport(StringRef arg);

  // Parses a string in the form of "key=value" and check
  // if value matches previous values for the key.
  // This feature used in the directive section to reject
  // incompatible objects.
  void checkFailIfMismatch(StringRef arg, InputFile *source);

  // Convert Windows resource files (.res files) to a .obj file.
  MemoryBufferRef convertResToCOFF(ArrayRef<MemoryBufferRef> mbs,
                                   ArrayRef<ObjFile *> objs);

  // Create export thunks for exported and patchable Arm64EC function symbols.
  void createECExportThunks();
  void maybeCreateECExportThunk(StringRef name, Symbol *&sym);

  bool ltoCompilationDone = false;
};

// Create enum with OPT_xxx values for each option in Options.td
enum {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

} // namespace lld::coff

#endif
