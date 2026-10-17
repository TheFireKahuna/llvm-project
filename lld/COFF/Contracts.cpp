//===- Contracts.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ABI contracts and their witnesses. An object names the contracts that its
// exported and offered definitions provide in directives, and those that its
// references to symbols another image may provide require in its link-only
// records. Within an image every contract of one identity must agree. A DLL
// exports a witness of each contract it provides, a name holding the
// contract's locator and digest; an image whose code requires a contract of a
// DLL that exports a witness for the contract's identity imports the witness
// of the contract it requires, so that a DLL rebuilt with another layout fails
// to load, and a link against a DLL built with another layout fails.
//
//===----------------------------------------------------------------------===//

#include "Contracts.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Driver.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/DataExtractor.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace lld;
using namespace lld::coff;

// The text sizes of a contract's locator, 64 bits, and of its digest, 128
// bits, in unpadded base64url.
static constexpr size_t locatorSize = 11;
static constexpr size_t digestSize = 22;

// The prefix of the name of a witness, by which a DLL exports it, of the
// contract of a type or of the type of a datum.
static StringRef getWitnessPrefix(bool data) {
  return data ? "__abis$" : "__abiw$";
}

static std::string getWitnessName(bool data, StringRef locator,
                                  StringRef digest) {
  return (getWitnessPrefix(data) + locator + "$" + digest).str();
}

bool lld::coff::isContractWitnessName(StringRef name) {
  return name.starts_with(getWitnessPrefix(false)) ||
         name.starts_with(getWitnessPrefix(true));
}

// The identity of a contract as a diagnostic names it: a type, which the
// identity spells as a type_info name does, or the datum it is the type of.
static std::string describe(const COFFLinkerContext &ctx, bool data,
                            StringRef identity, StringRef locator) {
  if (identity.empty())
    return (data ? "the type of the datum with locator "
                 : "the type with locator ") +
           locator.str();
  if (data)
    return "the type of " + maybeDemangleSymbol(ctx, identity);
  std::string name = demangle(("_ZTS" + identity).str());
  StringRef type = name;
  if (!type.consume_front("typeinfo name for "))
    type = identity;
  return ("'" + type + "'").str();
}

// Checks that a contract an input file gives agrees with any other the image's
// files give for its identity, which, as the identities of two types with one
// name are, must be one.
static void checkContract(COFFLinkerContext &ctx, bool data, StringRef locator,
                          StringRef digest, InputFile *file,
                          StringRef identity) {
  std::string key = ((data ? "s" : "w") + locator).str();
  auto [it, inserted] = ctx.contracts.known.try_emplace(
      key, ContractState::Known{digest, file, identity});
  if (inserted)
    return;
  ContractState::Known &known = it->second;
  if (!identity.empty() && !known.identity.empty() &&
      identity != known.identity) {
    Err(ctx) << describe(ctx, data, known.identity, locator) << " in "
             << known.file << " and " << describe(ctx, data, identity, locator)
             << " in " << file
             << " have ABI contracts with the same locator; rename one";
    return;
  }
  if (known.identity.empty())
    known.identity = identity;
  if (known.digest != digest)
    Err(ctx) << "ABI contract mismatch for "
             << describe(ctx, data, known.identity, locator) << ": "
             << known.file << " and " << file
             << " were compiled with different layouts";
}

static bool isBase64URL(StringRef s) {
  return llvm::all_of(
      s, [](char c) { return isAlnum(c) || c == '-' || c == '_'; });
}

void lld::coff::addContractProvides(COFFLinkerContext &ctx, InputFile *file,
                                    StringRef rows, bool data) {
  if (!ctx.config.importSlots)
    return;
  if (auto *bitcode = dyn_cast<BitcodeFile>(file);
      bitcode && !ctx.contracts.bitcodeDefinitions.contains(file)) {
    BitVector &defined = ctx.contracts.bitcodeDefinitions[file];
    defined.resize(bitcode->obj->symbols().size());
    for (auto [i, sym] : llvm::enumerate(bitcode->obj->symbols()))
      if (!sym.isUndefined())
        defined.set(i);
  }
  SmallVector<StringRef, 0> &witnesses = ctx.contracts.provides[file];
  SmallVector<StringRef, 16> split;
  rows.split(split, ',');
  for (StringRef row : split) {
    if (row.size() != locatorSize + digestSize || !isBase64URL(row)) {
      Err(ctx) << file << ": malformed /abiprovide" << (data ? "data" : "")
               << ": directive: " << row;
      return;
    }
    StringRef locator = row.take_front(locatorSize);
    StringRef digest = row.drop_front(locatorSize);
    checkContract(ctx, data, locator, digest, file, "");
    witnesses.push_back(saver().save(getWitnessName(data, locator, digest)));
  }
}

bool lld::coff::readContractRequirements(ObjFile *file,
                                         ArrayRef<uint8_t> payload) {
  COFFLinkerContext &ctx = file->symtab.ctx;
  auto malformed = [&](const Twine &msg) {
    Err(ctx) << file << ": .llvm_link_records is malformed: " << msg;
    return false;
  };
  DataExtractor data(payload, /*IsLittleEndian=*/true);
  DataExtractor::Cursor cur(0);
  uint64_t count = data.getULEB128(cur);
  if (cur && count > payload.size())
    return malformed("too many contracts");
  SmallVector<ContractState::Contract *, 0> contracts;
  for (uint64_t i = 0; cur && i != count; ++i) {
    uint64_t flags = data.getULEB128(cur);
    uint64_t size = data.getULEB128(cur);
    StringRef identity = data.getBytes(cur, size);
    StringRef digest = data.getBytes(cur, 16);
    if (!cur)
      break;
    auto *c = make<ContractState::Contract>();
    c->data = flags & 1;
    c->identity = identity;
    c->locator = saver().save(getABIContractLocator(identity));
    c->digest = saver().save(encodeBase64URL(arrayRefFromStringRef(digest)));
    contracts.push_back(c);
  }

  std::vector<ContractState::Requirement> requirements;
  ArrayRef<Symbol *> symbols = file->getSymbols();
  while (cur && !data.eof(cur)) {
    uint64_t symIndex = data.getULEB128(cur);
    uint64_t rows = data.getULEB128(cur);
    if (!cur)
      break;
    Expected<COFFSymbolRef> sym = file->getCOFFObj()->getSymbol(symIndex);
    if (!sym || !sym->isSectionDefinition() || sym->getSectionNumber() <= 0) {
      consumeError(sym.takeError());
      return malformed("contract group of symbol " + Twine(symIndex) +
                       ", which is not a section's symbol");
    }
    uint32_t section = sym->getSectionNumber();
    for (uint64_t i = 0; cur && i != rows; ++i) {
      uint64_t index = data.getULEB128(cur);
      uint64_t n = data.getULEB128(cur);
      if (!cur)
        break;
      if (index >= symbols.size() || !symbols[index])
        return malformed("contract row of symbol " + Twine(index) +
                         ", which the object does not have");
      ContractState::Requirement &r = requirements.emplace_back(
          ContractState::Requirement{section, symbols[index], {}});
      for (uint64_t j = 0; cur && j != n; ++j) {
        uint64_t c = data.getULEB128(cur);
        if (cur && c >= contracts.size())
          return malformed("contract row naming contract " + Twine(c) +
                           ", which the object does not have");
        if (cur)
          r.contracts.push_back(contracts[c]);
      }
    }
  }
  if (Error e = cur.takeError())
    return malformed(toString(std::move(e)));

  if (!ctx.config.importSlots)
    return true;
  for (const ContractState::Contract *c : contracts)
    checkContract(ctx, c->data, c->locator, c->digest, file, c->identity);
  if (!requirements.empty()) {
    std::vector<ContractState::Requirement> &v =
        ctx.contracts.requirements[file];
    llvm::append_range(v, requirements);
  }
  return true;
}

// Whether a definition of file is exported, which for bitcode its symbol
// table told, since LTO's output defines the symbols.
static bool definesExport(COFFLinkerContext &ctx, InputFile *file,
                          const DenseSet<Symbol *> &exported) {
  if (auto *bitcode = dyn_cast<BitcodeFile>(file)) {
    const BitVector &defined = ctx.contracts.bitcodeDefinitions.lookup(file);
    for (auto [i, sym] : llvm::enumerate(bitcode->getSymbols()))
      if (i < defined.size() && defined.test(i) && exported.contains(sym))
        return true;
    return false;
  }
  if (auto *obj = dyn_cast<ObjFile>(file))
    for (Symbol *sym : obj->getSymbols())
      if (sym && exported.contains(sym) && isa<Defined>(sym) &&
          sym->getFile() == obj)
        return true;
  return false;
}

void lld::coff::addContractWitnessExports(COFFLinkerContext &ctx) {
  if (!ctx.config.importSlots || ctx.hybridSymtab)
    return;
  std::vector<Export> &exports = ctx.symtab.exports;
  llvm::erase_if(exports, [&](const Export &e) {
    StringRef name = !e.exportAs.empty()  ? StringRef(e.exportAs)
                     : !e.extName.empty() ? StringRef(e.extName)
                                          : StringRef(e.name);
    if (!isContractWitnessName(name) && !isContractWitnessName(e.name))
      return false;
    if (e.source != ExportSource::ExportAll)
      Err(ctx) << "cannot export " << e.name
               << (name == e.name ? "" : " as " + name.str())
               << ": the name is reserved for ABI contract witnesses";
    return true;
  });
  if (exports.empty() || ctx.contracts.provides.empty())
    return;

  // The witnesses of the contracts that the files whose definitions the image
  // exports provide, each once.
  DenseSet<Symbol *> exported;
  for (const Export &e : exports)
    if (e.sym)
      exported.insert(e.sym);
  SmallVector<StringRef, 0> witnesses;
  StringSet<> seen;
  for (auto &[file, names] : ctx.contracts.provides)
    if (definesExport(ctx, file, exported))
      for (StringRef name : names)
        if (seen.insert(name).second)
          witnesses.push_back(name);
  if (witnesses.empty())
    return;

  // Every witness names one read-only byte, through one ordinal.
  ctx.contracts.witness =
      make<DefinedSynthetic>("__llvm_abi_witness", make<StringChunk>(""));
  for (StringRef name : witnesses) {
    Export e;
    e.name = name;
    e.sym = ctx.contracts.witness;
    e.data = true;
    e.isPrivate = true;
    e.source = ExportSource::Export;
    exports.push_back(e);
  }
}

bool lld::coff::isContractWitness(COFFLinkerContext &ctx, const Defined *sym) {
  return sym && sym == ctx.contracts.witness;
}

// The import library of a DLL with witnesses holds the import of each, named
// for the DLL, <witness>$<dll>, and exported as the witness, so that no two
// DLLs' meet in an importer's symbol table. A witness is imported as code, so
// that the delay-load descriptor of a DLL may hold it.
void lld::coff::addContractWitnessImports(
    COFFLinkerContext &ctx, StringRef dllName,
    std::vector<COFFShortExport> &exports) {
  if (!ctx.contracts.witness)
    return;
  std::string dll = dllName.lower();
  for (size_t i = 0, e = exports.size(); i != e; ++i) {
    if (!exports[i].Private || !isContractWitnessName(exports[i].Name))
      continue;
    COFFShortExport w;
    w.Name = exports[i].Name + "$" + dll;
    w.ExportAs = exports[i].Name;
    w.Ordinal = exports[i].Ordinal;
    exports.push_back(std::move(w));
  }
}

// The import that a reference to sym binds to, if any.
static DefinedImportData *getImportOf(Symbol *sym) {
  if (auto *imp = dyn_cast<DefinedImportData>(sym))
    return imp;
  if (auto *thunk = dyn_cast<DefinedImportThunk>(sym))
    return thunk->wrappedSym;
  return nullptr;
}

void lld::coff::bindContractWitnesses(COFFLinkerContext &ctx) {
  if (!ctx.config.importSlots || ctx.hybridSymtab)
    return;
  StringSet<> bound;
  for (auto &[file, requirements] : ctx.contracts.requirements) {
    DenseSet<uint32_t> kept;
    for (Chunk *c : file->getChunks())
      if (auto *sc = dyn_cast<SectionChunk>(c); sc && sc->live)
        kept.insert(sc->getSectionNumber());
    for (const ContractState::Requirement &r : requirements) {
      DefinedImportData *imp =
          kept.contains(r.section) ? getImportOf(r.sym) : nullptr;
      if (!imp)
        continue;
      std::string dll = imp->getDLLName().lower();
      for (const ContractState::Contract *c : r.contracts) {
        std::string name = getWitnessName(c->data, c->locator, c->digest);
        std::string impName = "__imp_" + name + "$" + dll;
        if (!bound.insert(impName).second)
          continue;
        Symbol *s = ctx.symtab.find(impName);
        if (auto *lazy = dyn_cast_or_null<LazyArchive>(s)) {
          Log(ctx) << "Importing " << name << " from " << imp->getDLLName()
                   << " for " << file;
          auto *impFile = make<ImportFile>(ctx, lazy->getMemberBuffer());
          ctx.driver.addFile(impFile);
          impFile->live = true;
          continue;
        }
        if (auto *witness = dyn_cast_or_null<DefinedImportData>(s)) {
          witness->file->live = true;
          continue;
        }

        // A DLL that exports no witness for the identity does not vouch for
        // it; one that exports a witness of another contract was built with
        // another layout.
        std::string prefix =
            ("__imp_" + getWitnessPrefix(c->data) + c->locator + "$").str();
        std::string suffix = "$" + dll;
        bool other = llvm::any_of(
            ctx.symtab.getArchiveSymbolNames(prefix), [&](StringRef n) {
              return n.size() == prefix.size() + digestSize + suffix.size() &&
                     n.ends_with_insensitive(suffix);
            });
        if (other)
          Err(ctx) << "ABI mismatch for "
                   << describe(ctx, c->data, c->identity, c->locator) << ": "
                   << file << " was compiled with a different layout than "
                   << imp->getDLLName() << " exports (needed because " << file
                   << " uses "
                   << maybeDemangleSymbol(ctx, imp->getExternalName()) << ")";
      }
    }
  }
}
