//===- LTO.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LTO.h"
#include "COFFLinkerContext.h"
#include "Config.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Filesystem.h"
#include "lld/Common/Strings.h"
#include "lld/Common/TargetOptionsCommandFlags.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/DTLTO/DTLTO.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Support/Caching.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::object;
using namespace lld;
using namespace lld::coff;

static AddBufferFn
createAddBufferFn(std::vector<std::unique_ptr<MemoryBuffer>> &files,
                  std::vector<std::string> &filenames) {
  return [&files, &filenames](unsigned task, const Twine &moduleName,
                              std::unique_ptr<MemoryBuffer> mb) {
    files[task] = std::move(mb);
    filenames[task] = moduleName.str();
  };
}

std::string BitcodeCompiler::getThinLTOOutputFile(StringRef path) {
  return lto::getThinLTOOutputFile(path, ctx.config.thinLTOPrefixReplaceOld,
                                   ctx.config.thinLTOPrefixReplaceNew);
}

lto::Config BitcodeCompiler::createConfig() {
  lto::Config c;
  c.Options = initTargetOptionsFromCodeGenFlags();
  c.Options.EmitAddrsig = true;
  for (StringRef C : ctx.config.mllvmOpts)
    c.MllvmArgs.emplace_back(C.str());

  // Always emit a section per function/datum with LTO. LLVM LTO should get most
  // of the benefit of linker GC, but there are still opportunities for ICF.
  c.Options.FunctionSections = true;
  c.Options.DataSections = true;

  // Use static reloc model on 32-bit x86 because it usually results in more
  // compact code, and because there are also known code generation bugs when
  // using the PIC model (see PR34306).
  if (ctx.config.machine == COFF::IMAGE_FILE_MACHINE_I386)
    c.RelocModel = Reloc::Static;
  else
    c.RelocModel = Reloc::PIC_;
#ifndef NDEBUG
  c.DisableVerify = false;
#else
  c.DisableVerify = true;
#endif
  c.DiagHandler = diagnosticHandler;
  c.DwoDir = ctx.config.dwoDir.str();
  c.OptLevel = ctx.config.ltoo;
  c.CPU = getCPUStr();
  c.MAttrs = getMAttrs();
  std::optional<CodeGenOptLevel> optLevelOrNone = CodeGenOpt::getLevel(
      ctx.config.ltoCgo.value_or(args::getCGOptLevel(ctx.config.ltoo)));
  assert(optLevelOrNone && "Invalid optimization level!");
  c.CGOptLevel = *optLevelOrNone;
  c.AlwaysEmitRegularLTOObj = !ctx.config.ltoObjPath.empty();
  c.DebugPassManager = ctx.config.ltoDebugPassManager;
  c.CSIRProfile = std::string(ctx.config.ltoCSProfileFile);
  c.RunCSIRInstr = ctx.config.ltoCSProfileGenerate;
  c.PGOWarnMismatch = ctx.config.ltoPGOWarnMismatch;
  c.HasWholeProgramVisibility = ctx.config.ltoWholeProgramVisibility;
  c.ImageName = sys::path::filename(ctx.config.outputFile).str();
  c.ValidateAllVtablesHaveTypeInfos =
      ctx.config.ltoValidateAllVtablesHaveTypeInfos;
  c.AllVtablesHaveTypeInfos = ctx.config.ltoAllVtablesHaveTypeInfos;
  c.SampleProfile = ctx.config.ltoSampleProfileName;
  c.TimeTraceEnabled = ctx.config.timeTraceEnabled;
  c.TimeTraceGranularity = ctx.config.timeTraceGranularity;
  c.RemarksFilename = ctx.config.optRemarksFilename;
  c.RemarksPasses = ctx.config.optRemarksPasses;
  c.RemarksWithHotness = ctx.config.optRemarksWithHotness;
  c.RemarksHotnessThreshold = ctx.config.optRemarksHotnessThreshold;
  c.RemarksFormat = ctx.config.optRemarksFormat;

  if (ctx.config.emit == EmitKind::LLVM) {
    c.PreCodeGenModuleHook = [this](size_t task, const Module &m) {
      if (std::unique_ptr<raw_fd_ostream> os =
              openLTOOutputFile(ctx.config.outputFile))
        WriteBitcodeToFile(m, *os, false);
      return false;
    };
  } else if (ctx.config.emit == EmitKind::ASM) {
    c.CGFileType = CodeGenFileType::AssemblyFile;
    c.Options.MCOptions.AsmVerbose = true;
  }

  if (!ctx.config.saveTempsArgs.empty())
    checkError(c.addSaveTemps(std::string(ctx.config.outputFile) + ".",
                              /*UseInputModulePath*/ true,
                              ctx.config.saveTempsArgs));

  c.PTO.LoopVectorization = c.OptLevel > 1;
  c.PTO.SLPVectorization = c.OptLevel > 1;

  return c;
}

BitcodeCompiler::BitcodeCompiler(COFFLinkerContext &c) : ctx(c) {
  // Initialize indexFile.
  if (!ctx.config.thinLTOIndexOnlyArg.empty())
    indexFile = openFile(ctx.config.thinLTOIndexOnlyArg);

  // Initialize ltoObj.
  lto::ThinBackend backend;
  if (ctx.config.thinLTOIndexOnly) {
    auto OnIndexWrite = [&](StringRef S) { thinIndices.erase(S); };
    backend = lto::createWriteIndexesThinBackend(
        llvm::hardware_concurrency(ctx.config.thinLTOJobs),
        std::string(ctx.config.thinLTOPrefixReplaceOld),
        std::string(ctx.config.thinLTOPrefixReplaceNew),
        std::string(ctx.config.thinLTOPrefixReplaceNativeObject),
        ctx.config.thinLTOEmitImportsFiles, indexFile.get(), OnIndexWrite);
  } else {
    backend = lto::createInProcessThinBackend(
        llvm::heavyweight_hardware_concurrency(ctx.config.thinLTOJobs));
  }

  if (ctx.config.dtltoDistributor.empty())
    ltoObj = std::make_unique<lto::LTO>(createConfig(), backend,
                                        ctx.config.ltoPartitions);
  else
    ltoObj = std::make_unique<lto::DTLTO>(
        createConfig(), ctx.config.ltoPartitions,
        llvm::lto::LTO::LTOKind::LTOK_Default, nullptr,
        ctx.config.thinLTOEmitImportsFiles, ctx.config.thinLTOIndexOnly,
        ctx.config.outputFile, ctx.config.dtltoDistributor,
        ctx.config.dtltoDistributorArgs, ctx.config.dtltoCompiler,
        ctx.config.dtltoCompilerPrependArgs, ctx.config.dtltoCompilerArgs,
        createAddBufferFn(files, file_names),
        !ctx.config.saveTempsArgs.empty());
}

BitcodeCompiler::~BitcodeCompiler() = default;

void BitcodeCompiler::waitForLTOCleanup() {
  if (ltoObj)
    ltoObj->waitForCleanup();
}

static void undefine(Symbol *s) { replaceSymbol<Undefined>(s, s->getName()); }

// Returns whether s resolves to an address in the image that nothing replaces
// after LTO. Imports and absolute symbols are outside the image, and -wrap
// redirects references only after LTO.
static bool isDefinedInImage(Symbol *s) {
  Defined *d = s ? s->getDefined() : nullptr;
  return d && s->canInline &&
         !isa<DefinedAbsolute, DefinedImportData, DefinedImportThunk>(d);
}

// A PE image has no symbol preemption, so a definition in the image is final
// for every reference from it. An undefined __imp_X is a dllimport reference
// to X; when X is in the image, the linker would point a local import at it,
// so the reference is final too, and LTO emits it as a direct reference.
static bool isFinalInImage(BitcodeFile &f, const lto::InputFile::Symbol &objSym,
                           Symbol *sym) {
  // ARM64EC binds symbols through anti-dependencies and thunks, some of
  // which are created only after LTO.
  if (f.symtab.isEC())
    return false;
  StringRef name = sym->getName();
  if (objSym.isUndefined() && name.consume_front("__imp_")) {
    // A defined __imp_X, such as an import, is a pointer whatever X is.
    auto *u = dyn_cast<Undefined>(sym);
    return u && !u->getWeakAlias() && isDefinedInImage(f.symtab.find(name));
  }
  return isDefinedInImage(sym);
}

// The import that offers what objSym defines: that of a thread-local
// variable's record, or else that of the definition.
static Symbol *findOffer(BitcodeFile &f, const lto::InputFile::Symbol &objSym,
                         Symbol *sym) {
  return f.symtab.find(
      ("__imp_" + sym->getName() + (objSym.isTLS() ? "$tls" : "")).str());
}

// Returns whether a DLL the image imports from offers the COMDAT data that
// objSym defines, whose instance preempts the definition after LTO.
static bool isOfferedWeakData(BitcodeFile &f,
                              const lto::InputFile::Symbol &objSym,
                              Symbol *sym) {
  if (!f.symtab.ctx.config.importSlots || objSym.isUndefined() ||
      objSym.getComdatIndex() == -1 || objSym.isExecutable() ||
      objSym.getVisibility() != GlobalValue::DefaultVisibility ||
      !isShareableVagueName(sym->getName()))
    return false;
  auto *imp = dyn_cast_or_null<DefinedImportData>(findOffer(f, objSym, sym));
  return imp && !f.symtab.ignoresOffersOf(imp);
}

// Returns whether a DLL the image imports from offers the function that objSym
// defines in a COMDAT, whose address-takes the native link binds to the offer
// while calls keep the image's copy.
static bool isOfferedFunction(BitcodeFile &f,
                              const lto::InputFile::Symbol &objSym,
                              Symbol *sym) {
  if (!f.symtab.ctx.config.importSlots || objSym.isUndefined() ||
      objSym.getComdatIndex() == -1 || !objSym.isExecutable() ||
      objSym.getVisibility() != GlobalValue::DefaultVisibility)
    return false;
  auto *imp = dyn_cast_or_null<DefinedImportData>(
      f.symtab.find(("__imp_" + sym->getName()).str()));
  return imp && imp->file->thunkSym && !f.symtab.ignoresOffersOf(imp);
}

// Returns whether the image offers what objSym defines once LTO is done, as
// ld.lld exports a definition of default visibility from a shared object, so
// that LTO keeps it: under -import-slots, in a link that writes an import
// library, a definition with default visibility in a COMDAT that another image
// may observe, data or a function whose address is significant.
static bool isOfferedByImage(BitcodeFile &f,
                             const lto::InputFile::Symbol &objSym,
                             Symbol *sym) {
  const Configuration &config = f.symtab.ctx.config;
  return config.importSlots && (config.dll || !config.implib.empty()) &&
         !f.symtab.isEC() && !objSym.isUndefined() &&
         objSym.getComdatIndex() != -1 &&
         objSym.getVisibility() == GlobalValue::DefaultVisibility &&
         (!objSym.isExecutable() || !objSym.canBeOmittedFromSymbolTable()) &&
         isShareableVagueName(sym->getName());
}

// Returns whether another image's instance may preempt at run time what
// objSym defines, in a DLL whose own references to such a vague entity read
// cells, as an ELF shared object reaches its default-visibility definitions
// through its GOT, so that LTO keeps them in import form: data with default
// visibility in a COMDAT but type_info objects, and functions whose addresses
// are significant, unless -Bsymbolic binds them locally
// (SymbolTable::isPreemptibleVagueEntity).
static bool isPreemptibleInImage(BitcodeFile &f,
                                 const lto::InputFile::Symbol &objSym,
                                 Symbol *sym) {
  const Configuration &config = f.symtab.ctx.config;
  using B = Configuration::Bsymbolic;
  if (!config.importSlots || !config.dll || f.symtab.isEC() ||
      f.symtab.ctx.hybridSymtab || config.bsymbolic == B::All ||
      objSym.isUndefined() || objSym.getComdatIndex() == -1 ||
      f.obj->getComdatTable()[objSym.getComdatIndex()].second ==
          Comdat::NoDeduplicate ||
      objSym.getVisibility() != GlobalValue::DefaultVisibility ||
      !isShareableVagueName(sym->getName()) || isTypeInfoName(sym->getName()) ||
      !isa_and_nonnull<Defined>(f.symtab.findUnderscore("__xi_a")))
    return false;
  if (objSym.isExecutable())
    return config.bsymbolic != B::Functions &&
           !objSym.canBeOmittedFromSymbolTable() &&
           !isOfferedFunction(f, objSym, sym);
  return true;
}

void BitcodeCompiler::add(BitcodeFile &f) {
  lto::InputFile &obj = *f.obj;
  unsigned symNum = 0;
  std::vector<Symbol *> symBodies = f.getSymbols();
  std::vector<lto::SymbolResolution> resols(symBodies.size());

  if (ctx.config.thinLTOIndexOnly)
    thinIndices.insert(obj.getName());

  // Another image may reference what this one exports, so whole-program
  // visibility leaves the vtables it exports public, and where KCFI checks go
  // through per-type thunks an exported function may have its address taken
  // there.
  if (!addedFile &&
      (ctx.config.ltoWholeProgramVisibility ||
       Triple(obj.getTargetTriple()).isWindowsItaniumOrNTPOSIXEnvironment())) {
    exportedSyms.emplace();
    for (const Export &e : f.symtab.exports)
      if (e.sym)
        exportedSyms->insert(e.sym);
  }
  addedFile = true;

  // Under -import-slots, the COMDATs of data that a DLL offers whole, every
  // definition in them by the same DLL, are preempted before LTO, as the
  // native link preempts them after: LTO compiles no copy, and ThinLTO keeps
  // one available for its value. Data that only a constant initializes, with
  // no guard and no initializer, costs no code, and stays prevailing for the
  // native link to preempt, which may keep its copy for loads that only read
  // values through its address (SymbolTable::readImmutableDataLocally).
  SmallVector<DefinedImportData *, 0> offeredComdats;
  if (ctx.config.importSlots && !f.symtab.isEC() && !ctx.hybridSymtab) {
    size_t numComdats = obj.getComdatTable().size();
    offeredComdats.assign(numComdats, nullptr);
    SmallVector<bool, 0> partial(numComdats, false);
    unsigned i = 0;
    for (const lto::InputFile::Symbol &objSym : obj.symbols()) {
      Symbol *sym = symBodies[i++];
      int c = objSym.getComdatIndex();
      if (c == -1 || objSym.isUndefined() || partial[c])
        continue;
      DefinedImportData *imp = nullptr;
      if (sym->getFile() == &f && isOfferedWeakData(f, objSym, sym))
        imp = cast<DefinedImportData>(findOffer(f, objSym, sym));
      if (!imp || (offeredComdats[c] &&
                   !offeredComdats[c]->getDLLName().equals_insensitive(
                       imp->getDLLName()))) {
        partial[c] = true;
        offeredComdats[c] = nullptr;
        continue;
      }
      if (!offeredComdats[c])
        offeredComdats[c] = imp;
    }
    // Only the offered COMDATs, which hold no code, need the test for a
    // thread-local or a guard. Every member of one is defined here, and is
    // thread-local only where objSym is.
    SmallVector<bool, 0> initialized(numComdats, false);
    i = 0;
    for (const lto::InputFile::Symbol &objSym : obj.symbols()) {
      Symbol *sym = symBodies[i++];
      int c = objSym.getComdatIndex();
      if (c == -1 || objSym.isUndefined() || !offeredComdats[c] ||
          initialized[c])
        continue;
      if (objSym.isTLS() || sym->getName().starts_with("_ZGV") ||
          isa_and_nonnull<Defined>(f.symtab.find(getGuardName(sym->getName()))))
        initialized[c] = true;
    }
    for (size_t c = 0; c != numComdats; ++c)
      if (!initialized[c])
        offeredComdats[c] = nullptr;
  }

  // Provide a resolution to the LTO API for each symbol.
  for (const lto::InputFile::Symbol &objSym : obj.symbols()) {
    Symbol *sym = symBodies[symNum];
    lto::SymbolResolution &r = resols[symNum];
    ++symNum;

    // Ideally we shouldn't check for SF_Undefined but currently IRObjectFile
    // reports two symbols for module ASM defined. Without this check, lld
    // flags an undefined in IR with a definition in ASM as prevailing.
    // Once IRObjectFile is fixed to report only one symbol this hack can
    // be removed.
    r.Prevailing = !objSym.isUndefined() && sym->getFile() == &f;
    if (int c = objSym.getComdatIndex(); r.Prevailing && c != -1 &&
                                         !offeredComdats.empty() &&
                                         offeredComdats[c]) {
      r.Prevailing = false;
      preemptedSyms.emplace_back(
          &f.symtab, sym, cast<DefinedImportData>(findOffer(f, objSym, sym)));
      continue;
    }
    // Preempted after LTO, offered data is kept, and the references to it
    // reach it through its import.
    bool offered = isOfferedWeakData(f, objSym, sym);
    r.VisibleToRegularObj =
        sym->isUsedInRegularObj || offered ||
        (r.Prevailing && (isOfferedByImage(f, objSym, sym) ||
                          isOfferedFunction(f, objSym, sym)));
    r.ExportDynamic = exportedSyms && exportedSyms->contains(sym);
    r.FinalDefinitionInLinkageUnit = !offered &&
                                     !isPreemptibleInImage(f, objSym, sym) &&
                                     isFinalInImage(f, objSym, sym);
    // The object LTO generates defines the symbol again. It is undefined only
    // once every file is added, so that the files after this one still see
    // where it is defined.
    if (r.Prevailing)
      prevailingSyms.push_back(sym);

    // We tell LTO to not apply interprocedural optimization for wrapped
    // (with -wrap) symbols because otherwise LTO would inline them while
    // their values are still not final.
    r.LinkerRedefined = !sym->canInline;
  }
  checkError(ltoObj->add(std::move(f.obj), resols));
}

// Merge all the bitcode files we have seen, codegen the result
// and return the resulting objects.
std::vector<InputFile *> BitcodeCompiler::compile() {
  llvm::TimeTraceScope timeScope("Bitcode compile");
  for (Symbol *sym : std::exchange(prevailingSyms, {}))
    undefine(sym);
  for (auto [symtab, sym, imp] : std::exchange(preemptedSyms, {}))
    symtab->preemptSymbol(sym, imp);

  unsigned maxTasks = ltoObj->getMaxTasks();
  buf.resize(maxTasks);
  files.resize(maxTasks);
  file_names.resize(maxTasks);

  // The /lldltocache option specifies the path to a directory in which to cache
  // native object files for ThinLTO incremental builds. If a path was
  // specified, configure LTO to use it as the cache directory.
  FileCache cache;
  if (!ctx.config.ltoCache.empty())
    cache = check(localCache("ThinLTO", "Thin", ctx.config.ltoCache,
                             createAddBufferFn(files, file_names),
                             !ctx.config.dtltoDistributor.empty()));

  checkError(ltoObj->run(
      [&](size_t task, const Twine &moduleName) {
        buf[task].first = moduleName.str();
        return std::make_unique<CachedFileStream>(
            std::make_unique<raw_svector_ostream>(buf[task].second));
      },
      cache));

  // Emit empty index files for non-indexed files
  for (StringRef s : thinIndices) {
    std::string path = getThinLTOOutputFile(s);
    openFile(path + ".thinlto.bc");
    if (ctx.config.thinLTOEmitImportsFiles)
      openFile(path + ".imports");
  }

  // ThinLTO with index only option is required to generate only the index
  // files. After that, we exit from linker and ThinLTO backend runs in a
  // distributed environment.
  if (ctx.config.thinLTOIndexOnly) {
    if (!ctx.config.ltoObjPath.empty())
      saveBuffer(buf[0].second, ctx.config.ltoObjPath);
    if (indexFile)
      indexFile->close();
    return {};
  }

  if (!ctx.config.ltoCache.empty())
    check(pruneCache(ctx.config.ltoCache, ctx.config.ltoCachePolicy, files));

  std::vector<InputFile *> ret;
  bool emitASM = ctx.config.emit == EmitKind::ASM;
  const char *Ext = emitASM ? ".s" : ".obj";
  for (unsigned i = 0; i != maxTasks; ++i) {
    StringRef bitcodeFilePath;
    // Get the native object contents either from a MemoryBuffer, for example
    // from the cache or an external DTLTO backend compilation, or by reading
    // from memory. Do not use the provided MemoryBuffer directly, or the PDB
    // will not be deterministic.
    StringRef objBuf;
    if (files[i]) {
      objBuf = files[i]->getBuffer();
      bitcodeFilePath = file_names[i];
    } else {
      objBuf = buf[i].second;
      bitcodeFilePath = buf[i].first;
    }
    if (objBuf.empty())
      continue;

    // If the input bitcode file is path/to/a.obj, then the corresponding lto
    // object file name will look something like: path/to/main.exe.lto.a.obj.
    StringRef ltoObjName;
    if (bitcodeFilePath == "ld-temp.o") {
      ltoObjName =
          saver().save(Twine(ctx.config.outputFile) + ".lto" +
                       (i == 0 ? Twine("") : Twine('.') + Twine(i)) + Ext);
    } else {
      StringRef directory = sys::path::parent_path(bitcodeFilePath);
      StringRef baseName = sys::path::stem(bitcodeFilePath);
      StringRef outputFileBaseName = sys::path::filename(ctx.config.outputFile);
      SmallString<64> path;
      sys::path::append(path, directory,
                        outputFileBaseName + ".lto." + baseName + Ext);
      sys::path::remove_dots(path, true);
      ltoObjName = saver().save(path.str());
    }
    if (llvm::is_contained(ctx.config.saveTempsArgs, "prelink") || emitASM)
      saveBuffer(objBuf, ltoObjName);
    if (!emitASM)
      ret.push_back(ObjFile::create(ctx, MemoryBufferRef(objBuf, ltoObjName)));
  }

  return ret;
}

void BitcodeCompiler::setBitcodeLibFuncs(ArrayRef<StringRef> bitcodeLibFuncs) {
  ltoObj->setBitcodeLibFuncs(bitcodeLibFuncs);
}
