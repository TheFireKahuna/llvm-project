//===- MinGW.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MinGW.h"
#include "COFFLinkerContext.h"
#include "Driver.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/RuntimeLibcalls.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace lld;
using namespace lld::coff;

AutoExporter::AutoExporter(
    SymbolTable &symtab, const llvm::DenseSet<StringRef> &manualExcludeSymbols)
    : manualExcludeSymbols(manualExcludeSymbols), symtab(symtab) {
  excludeLibs = {
      "libgcc",
      "libgcc_s",
      "libstdc++",
      "libmingw32",
      "libmingwex",
      "libg2c",
      "libsupc++",
      "libobjc",
      "libgcj",
      "libclang_rt.builtins",
      "libclang_rt.builtins-aarch64",
      "libclang_rt.builtins-arm",
      "libclang_rt.builtins-i386",
      "libclang_rt.builtins-x86_64",
      "libclang_rt.profile",
      "libclang_rt.profile-aarch64",
      "libclang_rt.profile-arm",
      "libclang_rt.profile-i386",
      "libclang_rt.profile-x86_64",
      "libcygwin",
      "libmsys-2.0",
      "libc++",
      "libc++abi",
      "libflang_rt.runtime",
      "libunwind",
      "libmsvcrt",
      "libmsvcrt-os",
      "libucrtbase",
      "libucrt",
      "libucrtapp",
      "libpthread",
      "libwinpthread",
  };

  excludeObjects = {
      "crt0.o",    "crt1.o",  "crt1u.o", "crt2.o",  "crt2u.o",    "dllcrt1.o",
      "dllcrt2.o", "gcrt0.o", "gcrt1.o", "gcrt2.o", "crtbegin.o", "crtend.o",
  };

  excludeSymbolPrefixes = {
      // Import symbols
      "__imp_",
      "__IMPORT_DESCRIPTOR_",
      // Extra import symbols from GNU import libraries
      "__nm_",
      // C++ symbols
      "__rtti_",
      "__builtin_",
      // Artificial symbols such as .refptr
      ".",
      // profile generate symbols
      "__profc_",
      "__profd_",
      "__profvp_",
  };

  excludeSymbolSuffixes = {
      "_iname",
      "_NULL_THUNK_DATA",
  };

  if (symtab.machine == I386) {
    excludeSymbols = {
        "__NULL_IMPORT_DESCRIPTOR",
        "__pei386_runtime_relocator",
        "_do_pseudo_reloc",
        "_impure_ptr",
        "__impure_ptr",
        "__fmode",
        "_environ",
        "___dso_handle",
        "__load_config_used",
        // These are the MinGW names that differ from the standard
        // ones (lacking an extra underscore).
        "_DllMain@12",
        "_DllEntryPoint@12",
        "_DllMainCRTStartup@12",
    };
    excludeSymbolPrefixes.insert("__head_");
  } else {
    excludeSymbols = {
        "__NULL_IMPORT_DESCRIPTOR",
        "_pei386_runtime_relocator",
        "do_pseudo_reloc",
        "impure_ptr",
        "_impure_ptr",
        "_fmode",
        "environ",
        "__dso_handle",
        "_load_config_used",
        // These are the MinGW names that differ from the standard
        // ones (lacking an extra underscore).
        "DllMain",
        "DllEntryPoint",
        "DllMainCRTStartup",
    };
    excludeSymbolPrefixes.insert("_head_");
  }
  if (symtab.isEC()) {
    excludeSymbols.insert("__chpe_metadata");
    excludeSymbolPrefixes.insert("__os_arm64x_");
  }
}

void AutoExporter::addWholeArchive(StringRef path) {
  StringRef libName = sys::path::filename(path);
  // Drop the file extension, to match the processing below.
  libName = libName.substr(0, libName.rfind('.'));
  excludeLibs.erase(libName);
}

void AutoExporter::addExcludedSymbol(StringRef symbol) {
  excludeSymbols.insert(symbol);
}

bool AutoExporter::shouldExport(Defined *sym) const {
  if (!sym || !sym->getChunk())
    return false;

  // Only allow the symbol kinds that make sense to export; in particular,
  // disallow import symbols.
  if (!isa<DefinedRegular>(sym) && !isa<DefinedCommon>(sym))
    return false;
  if (excludeSymbols.contains(sym->getName()) ||
      manualExcludeSymbols.contains(sym->getName()))
    return false;

  for (StringRef prefix : excludeSymbolPrefixes.keys())
    if (sym->getName().starts_with(prefix))
      return false;
  for (StringRef suffix : excludeSymbolSuffixes.keys())
    if (sym->getName().ends_with(suffix))
      return false;

  // If a corresponding __imp_ symbol exists and is defined, don't export it.
  if (symtab.find(("__imp_" + sym->getName()).str()))
    return false;

  // Check that file is non-null before dereferencing it, symbols not
  // originating in regular object files probably shouldn't be exported.
  if (!sym->getFile())
    return false;

  StringRef libName = sys::path::filename(sym->getFile()->parentName);

  // Drop the file extension.
  libName = libName.substr(0, libName.rfind('.'));
  if (!libName.empty())
    return !excludeLibs.contains(libName);

  StringRef fileName = sys::path::filename(sym->getFile()->getName());
  return !excludeObjects.contains(fileName);
}

void lld::coff::writeDefFile(COFFLinkerContext &ctx, StringRef name,
                             const std::vector<Export> &exports) {
  llvm::TimeTraceScope timeScope("Write .def file");
  std::error_code ec;
  raw_fd_ostream os(name, ec, sys::fs::OF_None);
  if (ec)
    Fatal(ctx) << "cannot open " << name << ": " << ec.message();

  os << "EXPORTS\n";
  for (const Export &e : exports) {
    os << "    " << e.exportName << " "
       << "@" << e.ordinal;
    if (auto *def = dyn_cast_or_null<Defined>(e.sym)) {
      if (def && def->getChunk() &&
          !(def->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
        os << " DATA";
    }
    os << "\n";
  }
}

static StringRef mangle(Twine sym, MachineTypes machine) {
  assert(machine != IMAGE_FILE_MACHINE_UNKNOWN);
  if (machine == I386)
    return saver().save("_" + sym);
  return saver().save(sym);
}

// Handles -wrap option.
//
// This function instantiates wrapper symbols. At this point, they seem
// like they are not being used at all, so we explicitly set some flags so
// that LTO won't eliminate them.
// Binds __real_X, which something refers to, to X. As with GNU ld, a strong
// reference loads X if an archive or an import library offers it, and a weak
// one, which loads no archive member elsewhere, does not.
static void addWrappedReal(SymbolTable &symtab, WrappedSymbol &w,
                           Symbol *real) {
  w.real = real;
  real->deferUndefined = true;
  real->canInline = false;
  real->isUsedInRegularObj = true;
  auto *u = dyn_cast<Undefined>(real);
  if (w.sym->isLazy() && !(u && u->weakAlias))
    symtab.addUndefined(w.sym->getName());
}

bool lld::coff::addWrappedSymbols(SymbolTable &symtab,
                                  opt::InputArgList &args) {
  size_t numWrapped = symtab.wrapped.size();
  bool bound = false;
  for (WrappedSymbol &w : symtab.wrapped)
    if (!w.real)
      if (Symbol *real =
              symtab.find(mangle("__real_" + w.name, symtab.machine))) {
        addWrappedReal(symtab, w, real);
        bound = true;
      }
  for (auto *arg : args.filtered(OPT_wrap))
    addWrappedSymbol(symtab, arg->getValue());
  for (StringRef name : symtab.directiveWraps)
    addWrappedSymbol(symtab, name, /*fromDirective=*/true);
  symtab.wrapsAdded = true;
  return bound || symtab.wrapped.size() != numWrapped;
}

void lld::coff::addWrappedSymbol(SymbolTable &symtab, StringRef name,
                                 bool fromDirective) {
  Symbol *sym = symtab.findUnderscore(name);
  if (!sym || llvm::any_of(symtab.wrapped, [&](const WrappedSymbol &w) {
        return w.sym == sym;
      }))
    return;
  // The program's own X stands, and its wrapper is not loaded.
  if (fromDirective && isa<DefinedCOFF>(sym))
    return;

  // As with GNU ld, a wrap renames references and loads nothing else, so it
  // waits while nothing refers to X, __imp_X or __real_X. The LTO code
  // generator may call a runtime library function that no bitcode symbol
  // names, so with bitcode such a function counts as referenced.
  auto isReferenced = [](Symbol *s) { return s && !s->isLazy(); };
  Symbol *real = symtab.find(mangle("__real_" + name, symtab.machine));
  if (!isReferenced(sym) && !isReferenced(real) &&
      !isReferenced(symtab.find(("__imp_" + sym->getName()).str())) &&
      (symtab.bitcodeFileInstances.empty() ||
       RTLIB::RuntimeLibcallsInfo::lookupLibcallImplName(name).empty()))
    return;

  Symbol *wrap = symtab.addUndefined(mangle("__wrap_" + name, symtab.machine));
  symtab.wrapped.push_back({sym, nullptr, wrap, name, fromDirective});
  if (real)
    addWrappedReal(symtab, symtab.wrapped.back(), real);

  // These symbols may seem undefined initially, but don't bail out
  // at symtab.reportUnresolvable() due to them, but let wrapSymbols
  // below sort things out before checking finally with
  // symtab.resolveRemainingUndefines().
  sym->deferUndefined = true;
  // We want to tell LTO not to inline symbols to be overwritten
  // because LTO doesn't know the final symbol contents after renaming.
  sym->canInline = false;

  // Tell LTO not to eliminate these symbols.
  sym->isUsedInRegularObj = true;
  if (!isa<Undefined>(wrap))
    wrap->isUsedInRegularObj = true;
}

// Do renaming for -wrap by updating pointers to symbols.
//
// When this function is executed, only InputFiles and symbol table
// contain pointers to symbol objects. We visit them to replace pointers,
// so that wrapped symbols are swapped as instructed by the command line.
void lld::coff::wrapSymbols(SymbolTable &symtab) {
  DenseMap<Symbol *, Symbol *> map;
  for (const WrappedSymbol &w : symtab.wrapped) {
    // A weak __real_X keeps its default while nothing loaded X.
    if (w.real && !w.sym->isLazy())
      map[w.real] = w.sym;
    // A definition of X loaded since the wrap was added stands too.
    if (w.fromDirective && isa<DefinedCOFF>(w.sym))
      continue;
    map[w.sym] = w.wrap;
    if (Defined *d = dyn_cast<Defined>(w.wrap)) {
      Symbol *imp = symtab.find(("__imp_" + w.sym->getName()).str());
      // Create a new defined local import for the wrap symbol. If
      // no imp prefixed symbol existed, there's no need for it.
      // (We can't easily distinguish whether any object file actually
      // referenced it or not, though.)
      if (imp) {
        if (Symbol *wrapimp =
                symtab.find(("__imp_" + w.wrap->getName()).str())) {
          map[imp] = wrapimp;
        } else {
          DefinedLocalImport *localwrapimp = make<DefinedLocalImport>(
              symtab.ctx, saver().save("__imp_" + w.wrap->getName()), d);
          symtab.localImportChunks.push_back(localwrapimp->getChunk());
          map[imp] = localwrapimp;
        }
      }
    }
  }

  // Update pointers in input files.
  parallelForEach(symtab.ctx.objFileInstances, [&](ObjFile *file) {
    MutableArrayRef<Symbol *> syms = file->getMutableSymbols();
    for (auto &sym : syms)
      if (Symbol *s = map.lookup(sym))
        sym = s;
  });

  // Without garbage collection every loaded import stays in the image. Leave
  // out, as the collector would, the imports that the wrap left unreachable:
  // X's, which a reference read before the wrap loaded, and an imported
  // wrapper of a runtime library function the generated code never called,
  // whose X is still lazy. Nothing must refer to the thunk or its __imp_.
  COFFLinkerContext &ctx = symtab.ctx;
  if (ctx.config.doGC || ctx.hybridSymtab)
    return;
  SmallVector<DefinedImportThunk *, 0> thunks;
  for (const WrappedSymbol &w : symtab.wrapped) {
    auto *t = dyn_cast<DefinedImportThunk>(w.sym);
    if (t && !w.real)
      thunks.push_back(t);
    t = dyn_cast<DefinedImportThunk>(w.wrap);
    if (t && w.sym->isLazy())
      thunks.push_back(t);
  }
  if (thunks.empty())
    return;
  DenseSet<Symbol *> unused;
  for (DefinedImportThunk *t : thunks) {
    unused.insert(t);
    unused.insert(t->wrappedSym);
  }
  for (Symbol *s : ctx.config.gcroot)
    unused.erase(s);
  for (ObjFile *file : ctx.objFileInstances)
    for (Symbol *s : file->getSymbols())
      unused.erase(s);
  for (DefinedImportThunk *t : thunks) {
    if (unused.contains(t) && unused.contains(t->wrappedSym)) {
      t->wrappedSym->file->live = false;
      t->getChunk()->live = false;
    }
  }
}
