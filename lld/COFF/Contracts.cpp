//===- Contracts.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ABI contracts and their witnesses. An object names the contracts its
// definitions provide (in directives) and those its references require (in
// link records); within an image all contracts of one identity must agree. A
// DLL exports a witness per provided contract, a name holding its locator and
// digest. An image imports the witness of each contract its code requires, so
// a DLL rebuilt with another layout fails to load and a link against one fails.
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
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace lld;
using namespace lld::coff;

// Unpadded base64url text sizes of a 64-bit locator and a 128-bit digest.
static constexpr size_t locatorSize = 11;
static constexpr size_t digestSize = 22;

// Witness name prefix, for a type's contract or a datum type's contract.
static StringRef getWitnessPrefix(bool data) {
  return data ? "__abis$" : "__abiw$";
}

static std::string getWitnessName(bool data, StringRef locator,
                                  StringRef digest) {
  return (getWitnessPrefix(data) + locator + "$" + digest).str();
}

// Separator in a type_info name qualified by its type's contract.
static constexpr StringLiteral qualifiedTypeInfoTag = "$abi$";

static std::string getQualifiedTypeInfoName(StringRef identity,
                                            StringRef digest) {
  return ("_ZTI" + identity + qualifiedTypeInfoTag + digest).str();
}

bool lld::coff::isContractWitnessName(StringRef name) {
  return name.starts_with(getWitnessPrefix(false)) ||
         name.starts_with(getWitnessPrefix(true)) ||
         (isTypeInfoName(name) && name.contains(qualifiedTypeInfoTag));
}

// How a diagnostic names a contract's identity: the type, or the datum.
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

// Checks that a contract a file gives agrees with any other for its identity.
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

Error lld::coff::readContractRequirements(ObjFile *file,
                                          ArrayRef<uint8_t> payload) {
  COFFLinkerContext &ctx = file->symtab.ctx;
  DataExtractor data(payload, /*IsLittleEndian=*/true);
  DataExtractor::Cursor cur(0);
  uint64_t count = data.getULEB128(cur);
  if (cur && count > payload.size())
    return malformedLinkRecords("too many contracts");
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
    Expected<uint32_t> section =
        file->getRecordSection(symIndex, "contract group");
    if (!section)
      return section.takeError();
    for (uint64_t i = 0; cur && i != rows; ++i) {
      uint64_t index = data.getULEB128(cur);
      uint64_t n = data.getULEB128(cur);
      if (!cur)
        break;
      if (index >= symbols.size() || !symbols[index])
        return malformedLinkRecords("contract row of symbol " + Twine(index) +
                                    ", which the object does not have");
      ContractState::Requirement &r = requirements.emplace_back(
          ContractState::Requirement{*section, symbols[index], {}});
      for (uint64_t j = 0; cur && j != n; ++j) {
        uint64_t c = data.getULEB128(cur);
        if (cur && c >= contracts.size())
          return malformedLinkRecords("contract row naming contract " +
                                      Twine(c) +
                                      ", which the object does not have");
        if (cur)
          r.contracts.push_back(contracts[c]);
      }
    }
  }
  if (Error e = cur.takeError())
    return malformedLinkRecords(toString(std::move(e)));

  if (!ctx.config.importSlots)
    return Error::success();
  for (const ContractState::Contract *c : contracts)
    checkContract(ctx, c->data, c->locator, c->digest, file, c->identity);
  if (!requirements.empty()) {
    std::vector<ContractState::Requirement> &v =
        ctx.contracts.requirements[file];
    llvm::append_range(v, requirements);
  }
  return Error::success();
}

// Whether file defines an exported symbol (bitcode: the pre-LTO symbols).
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
  llvm::TimeTraceScope timeScope("Add contract witness exports");
  if (!ctx.config.importSlots)
    return;
  std::vector<Export> &exports = ctx.symtab.exports;
  ctx.symtab.reserveExportNames(isContractWitnessName, "ABI contract witnesses");
  if (exports.empty() || ctx.contracts.provides.empty())
    return;

  // The witnesses of contracts provided by files with an exported definition.
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

  // A type_info object the image owns is also exported under its contract-
  // qualified name, replacing the type's witness: an importer binds that name
  // and checks the contract with no separate witness import. The plain name
  // stays in the export table.
  StringMap<StringRef> typeDigests;
  for (StringRef name : witnesses)
    if (StringRef row = name; row.consume_front(getWitnessPrefix(false)))
      typeDigests[row.take_front(locatorSize)] =
          row.drop_front(locatorSize + 1);
  StringSet<> replaced;
  for (size_t i = 0, n = exports.size(); i != n; ++i) {
    Export &e = exports[i];
    StringRef name = e.extName.empty() ? e.name : e.extName;
    if (!isa_and_nonnull<DefinedRegular>(e.sym) ||
        e.source == ExportSource::ExportAll || e.isPrivate || e.noname ||
        !e.forwardTo.empty() || !e.exportAs.empty() || !isTypeInfoName(name))
      continue;
    StringRef identity = name.drop_front(strlen("_ZTI"));
    std::string locator = getABIContractLocator(identity);
    auto it = typeDigests.find(locator);
    if (it == typeDigests.end())
      continue;
    replaced.insert(getWitnessName(/*data=*/false, locator, it->second));
    Export qualified = e;
    qualified.extName =
        saver().save(getQualifiedTypeInfoName(identity, it->second));
    qualified.isPrivate = true;
    qualified.qualifiedName = qualified.extName;
    qualified.qualifiedCode = true;
    e.isPrivate = true;
    Export alias;
    alias.name = name;
    alias.exportAs = qualified.extName;
    alias.data = e.data;
    ctx.symtab.exportAsAliases.push_back(alias);
    exports.push_back(qualified);
  }
  llvm::erase_if(witnesses,
                 [&](StringRef name) { return replaced.contains(name); });
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
    // Imported as code, so that a delay-load descriptor can hold it.
    e.qualifiedName = name;
    e.qualifiedCode = true;
    exports.push_back(e);
  }
}

bool lld::coff::isContractWitness(COFFLinkerContext &ctx, const Defined *sym) {
  return sym && sym == ctx.contracts.witness;
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
  llvm::TimeTraceScope timeScope("Bind contract witnesses");
  if (!ctx.config.importSlots)
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
        // A DLL owning the type's type_info object vouches for the type by the
        // object's qualified name, which an import of the object already binds.
        std::string qualified;
        if (!c->data && !c->identity.empty()) {
          qualified = getQualifiedTypeInfoName(c->identity, c->digest);
          auto *ti = dyn_cast_or_null<DefinedImportData>(
              ctx.symtab.find(("__imp__ZTI" + c->identity).str()));
          if (ti && ti->file->live && ti->getExternalName() == qualified &&
              ti->getDLLName().equals_insensitive(dll))
            continue;
        }
        std::string name = getWitnessName(c->data, c->locator, c->digest);
        bool vouched = false;
        for (StringRef witness : {StringRef(qualified), StringRef(name)}) {
          if (witness.empty())
            continue;
          std::string impName = ("__imp_" + witness + "$" + dll).str();
          if (bound.contains(impName)) {
            vouched = true;
            break;
          }
          Symbol *s = ctx.symtab.find(impName);
          if (auto *lazy = dyn_cast_or_null<LazyArchive>(s)) {
            Log(ctx) << "Importing " << witness << " from " << imp->getDLLName()
                     << " for " << file;
            ctx.symtab.loadImportMember(lazy);
            vouched = true;
          } else if (auto *w = dyn_cast_or_null<DefinedImportData>(s)) {
            w->file->live = true;
            vouched = true;
          }
          if (vouched) {
            bound.insert(impName);
            break;
          }
        }
        // A DLL exporting no witness for the identity does not vouch; one
        // exporting a witness of another contract was built with another
        // layout, reported once.
        std::string suffix = "$" + dll;
        if (vouched || !bound.insert("__imp_" + name + suffix).second)
          continue;
        auto hasOther = [&](const std::string &prefix) {
          return llvm::any_of(
              ctx.symtab.getArchiveSymbolNames(prefix), [&](StringRef n) {
                return n.size() == prefix.size() + digestSize + suffix.size() &&
                       n.ends_with_insensitive(suffix);
              });
        };
        bool other =
            hasOther(("__imp_" + getWitnessPrefix(c->data) + c->locator + "$")
                         .str()) ||
            (!qualified.empty() &&
             hasOther(
                 ("__imp__ZTI" + c->identity + qualifiedTypeInfoTag).str()));
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
