//===- InputFiles.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Config.h"
#include "DebugTypes.h"
#include "Driver.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/DWARF.h"
#include "lld/Common/X86Instructions.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/CodeView/DebugSubsectionRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/IR/Mangler.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Triple.h"
#include <cstring>
#include <optional>
#include <utility>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::codeview;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::coff;

using llvm::Triple;
using llvm::support::ulittle32_t;

// Returns the last element of a path, which is supposed to be a filename.
static StringRef getBasename(StringRef path) {
  return sys::path::filename(path, sys::path::Style::windows);
}

// Returns a string in the format of "foo.obj" or "foo.obj(bar.lib)".
std::string lld::toString(const coff::InputFile *file) {
  if (!file)
    return "<internal>";
  if (file->parentName.empty())
    return std::string(file->getName());

  return (getBasename(file->parentName) + "(" + getBasename(file->getName()) +
          ")")
      .str();
}

const COFFSyncStream &coff::operator<<(const COFFSyncStream &s,
                                       const InputFile *f) {
  return s << toString(f);
}

// Whether a weak alias's default is a definition, not a weak reference's
// undefined or zero default.
static bool isDefinition(Symbol *s) {
  auto *abs = dyn_cast_or_null<DefinedAbsolute>(s);
  return isa_and_nonnull<Defined>(s) && !(abs && abs->getVA() == 0);
}

// Makes Source, if undefined, a weak alias to Target. An anti-dependency
// binds only an unaliased symbol and yields to any other alias; among the
// others the first whose default is a definition wins, as ELF linkers choose
// the first weak definition.
static void addWeakAlias(Symbol *source, Symbol *target,
                         Symbol::WeakKind kind = Symbol::Alias) {
  auto *u = dyn_cast<Undefined>(source);
  if (u && (!u->weakAlias || (kind != Symbol::AntiDependency &&
                              (u->isAntiDep() || (!isDefinition(u->weakAlias) &&
                                                  isDefinition(target))))))
    u->setWeakAlias(target, kind);
}

// link.exe takes a weak external with a section number as a definition.
static bool isDefinedWeakExternal(COFFSymbolRef sym) {
  return sym.isWeakExternal() && sym.getSectionNumber() != IMAGE_SYM_UNDEFINED;
}

static bool ignoredSymbolName(StringRef name) {
  return name == "@feat.00" || name == "@comp.id";
}

static coff_symbol_generic *cloneSymbol(COFFSymbolRef sym) {
  if (sym.isBigObj()) {
    auto *copy = make<coff_symbol32>(
        *reinterpret_cast<const coff_symbol32 *>(sym.getRawPtr()));
    return reinterpret_cast<coff_symbol_generic *>(copy);
  } else {
    auto *copy = make<coff_symbol16>(
        *reinterpret_cast<const coff_symbol16 *>(sym.getRawPtr()));
    return reinterpret_cast<coff_symbol_generic *>(copy);
  }
}

// Skip importing DllMain thunks from import libraries.
static bool fixupDllMain(COFFLinkerContext &ctx, llvm::object::Archive *file,
                         const Archive::Symbol &sym, bool &skipDllMain) {
  const Archive::Child &c =
      CHECK(sym.getMember(), file->getFileName() +
                                 ": could not get the member for symbol " +
                                 toCOFFString(ctx, sym));
  MemoryBufferRef mb =
      CHECK(c.getMemoryBufferRef(),
            file->getFileName() +
                ": could not get the buffer for a child buffer of the archive");
  if (identify_magic(mb.getBuffer()) == file_magic::coff_import_library) {
    if (ctx.config.warnImportedDllMain) {
      // We won't place DllMain symbols in the symbol table if they are
      // coming from a import library. This message can be ignored with the flag
      // '/ignore:importeddllmain'
      Warn(ctx)
          << file->getFileName()
          << ": skipping imported DllMain symbol [importeddllmain]\nNOTE: this "
             "might be a mistake when the DLL/library was produced.";
    }
    skipDllMain = true;
    return true;
  }
  return false;
}

ArchiveFile::ArchiveFile(COFFLinkerContext &ctx, MemoryBufferRef m,
                         std::unique_ptr<Archive> &f)
    : InputFile(ctx.symtab, ArchiveKind, m) {
  file.swap(f);
}

std::optional<MemoryBufferRef> ArchiveFile::findMember(StringRef name) {
  std::optional<Archive::Child> c = CHECK(file->findSym(name), this);
  if (!c)
    return std::nullopt;
  return CHECK(c->getMemoryBufferRef(), this);
}

void ArchiveFile::parse() {
  COFFLinkerContext &ctx = symtab.ctx;
  SymbolTable *archiveSymtab = &symtab;

  // Try to read symbols from ECSYMBOLS section on ARM64EC.
  if (ctx.symtab.isEC()) {
    iterator_range<Archive::symbol_iterator> symbols =
        CHECK(file->ec_symbols(), this);
    if (!symbols.empty()) {
      for (const Archive::Symbol &sym : symbols)
        ctx.symtab.addLazyArchive(this, sym);

      // Read both EC and native symbols on ARM64X.
      archiveSymtab = &*ctx.hybridSymtab;
    } else {
      // If the ECSYMBOLS section is missing in the archive, the archive could
      // be either a native-only ARM64 or x86_64 archive. Check the machine type
      // of the object containing a symbol to determine which symbol table to
      // use.
      Archive::symbol_iterator sym = file->symbol_begin();
      if (sym != file->symbol_end()) {
        MachineTypes machine = IMAGE_FILE_MACHINE_UNKNOWN;
        Archive::Child child =
            CHECK(sym->getMember(),
                  file->getFileName() +
                      ": could not get the buffer for a child of the archive");
        MemoryBufferRef mb = CHECK(
            child.getMemoryBufferRef(),
            file->getFileName() +
                ": could not get the buffer for a child buffer of the archive");
        switch (identify_magic(mb.getBuffer())) {
        case file_magic::coff_object: {
          std::unique_ptr<COFFObjectFile> obj =
              CHECK(COFFObjectFile::create(mb),
                    check(child.getName()) + ":" + ": not a valid COFF file");
          machine = MachineTypes(obj->getMachine());
          break;
        }
        case file_magic::coff_import_library:
          machine = MachineTypes(COFFImportFile(mb).getMachine());
          break;
        case file_magic::bitcode: {
          std::unique_ptr<lto::InputFile> obj =
              check(lto::InputFile::create(mb));
          machine = BitcodeFile::getMachineType(obj.get());
          break;
        }
        default:
          break;
        }
        archiveSymtab = &ctx.getSymtab(machine);
      }
    }
  }

  bool skipDllMain = false;
  StringRef mangledDllMain, impMangledDllMain;

  // The calls below will fail if we haven't set the machine type yet. Instead
  // of failing, it is preferable to skip this "imported DllMain" check if we
  // don't know the machine type at this point.
  if (!file->isEmpty() && ctx.config.machine != IMAGE_FILE_MACHINE_UNKNOWN) {
    mangledDllMain = archiveSymtab->mangle("DllMain");
    impMangledDllMain = uniqueSaver().save("__imp_" + mangledDllMain);
  }

  // Read the symbol table to construct Lazy objects.
  for (const Archive::Symbol &sym : file->symbols()) {
    // If an import library provides the DllMain symbol, skip importing it, as
    // we should be using our own DllMain, not another DLL's DllMain.
    if (!mangledDllMain.empty() && (sym.getName() == mangledDllMain ||
                                    sym.getName() == impMangledDllMain)) {
      if (skipDllMain || fixupDllMain(ctx, file.get(), sym, skipDllMain))
        continue;
    }
    archiveSymtab->addLazyArchive(this, sym);
  }
}

// Returns a buffer pointing to a member file containing a given symbol.
void ArchiveFile::addMember(const Archive::Symbol &sym) {
  const Archive::Child &c =
      CHECK(sym.getMember(), "could not get the member for symbol " +
                                 toCOFFString(symtab.ctx, sym));

  // Return an empty buffer if we have already returned the same buffer.
  // FIXME: Remove this once we resolve all defineds before all undefineds in
  //        ObjFile::initializeSymbols().
  if (!seen.insert(c.getChildOffset()).second)
    return;

  symtab.ctx.driver.enqueueArchiveMember(c, sym, getName());
}

void ArchiveFile::addAllMembers(bool lazy) {
  COFFLinkerContext &ctx = symtab.ctx;
  Error err = Error::success();

  // Thin archives refer to .o files, so --reproduces needs the .o files too.
  bool addToTar = file->isThin() && ctx.driver.tar;

  for (const Archive::Child &c : file->children(err)) {
    if (!seen.insert(c.getChildOffset()).second)
      continue;
    MemoryBufferRef mbref =
        CHECK(c.getMemoryBufferRef(),
              file->getFileName() +
                  ": could not get the buffer for a child of the archive");
    if (addToTar) {
      ctx.driver.tar->append(relativeToRoot(check(c.getFullName())),
                             mbref.getBuffer());
    }
    if (file->isThin())
      ctx.driver.addThinArchiveBuffer(mbref, "<whole-archive>", lazy);
    else
      ctx.driver.addArchiveBuffer(mbref, "<whole-archive>", getName(),
                                  c.getChildOffset(), lazy);
  }
  if (err)
    Fatal(ctx) << file->getFileName()
               << ": Archive::children failed: " << toString(std::move(err));
}

ObjFile::ObjFile(SymbolTable &symtab, COFFObjectFile *coffObj, bool lazy)
    : InputFile(symtab, ObjectKind, coffObj->getMemoryBufferRef(), lazy),
      coffObj(coffObj) {}

std::unique_ptr<COFFObjectFile>
ObjFile::createCOFFObject(COFFLinkerContext &ctx, MemoryBufferRef m) {
  // Parse a memory buffer as a COFF file.
  Expected<std::unique_ptr<Binary>> bin = createBinary(m);
  if (!bin)
    Fatal(ctx) << "Could not parse " << m.getBufferIdentifier();

  std::unique_ptr<COFFObjectFile> obj(dyn_cast<COFFObjectFile>(bin->release()));
  if (!obj.get())
    Fatal(ctx) << m.getBufferIdentifier() << " is not a COFF file";

  return obj;
}

// Leaves out of the image what begins c before the type prefix that ends at
// entry, where that is the no-operations which align the entry and the
// prefix's marker instruction follows them. Padding as large as the alignment
// holds something else, a function of no-operations among them.
static void leaveOutPrefixPadding(SectionChunk *c, uint32_t entry,
                                  bool secondWord) {
  ArrayRef<uint8_t> b = c->getContents();
  uint32_t size = secondWord ? 16 : 12, pad = entry - size;
  if (entry < size || entry > b.size() || !pad || pad % 4 || pad > 28 ||
      pad >= c->getAlignment() || memcmp(&b[entry - 12], "\x0f\x1f", 2) ||
      b[entry - 10] != (0x80 | secondWord))
    return;
  b = b.take_front(pad);
  if (c->getMachine() == AMD64)
    while (unsigned n = x86::getNopLength(b))
      b = b.drop_front(n);
  else
    while (!b.empty() && support::endian::read32le(b.data()) == 0xd503201f)
      b = b.drop_front(4);
  if (b.empty())
    c->leftOut = pad / 4;
}

Error InputFile::readLinkFacts(StringRef facts, ArrayRef<Symbol *> symbols,
                               function_ref<bool(uint64_t)> defines) {
  for (bool apply : {false, true})
    if (Error e = readCOFFLinkFacts(
            facts,
            [&](uint64_t index, uint64_t tag, StringRef operands) -> Error {
              if (index >= symbols.size())
                return createStringError("fact of symbol " + Twine(index) +
                                         ", which the file does not have");
              Symbol *sym = symbols[index];
              switch (tag) {
              case LinkFactTypePrefix: {
                DataExtractor data(operands, /*IsLittleEndian=*/true);
                DataExtractor::Cursor c(0);
                uint64_t secondWord = data.getULEB128(c);
                if (!c)
                  return c.takeError();
                if (secondWord > LinkFactVfnType)
                  return createStringError("type prefix of symbol " +
                                           Twine(index) + " is out of range");
                if (apply && sym) {
                  sym->typePrefix = 1 + secondWord;
                  if (auto *d = dyn_cast<DefinedRegular>(sym);
                      d && d->file == this && isa<ObjFile>(this))
                    leaveOutPrefixPadding(d->getChunk(), d->getValue(),
                                          secondWord);
                  if (!symtab.ctx.typePrefixFile)
                    symtab.ctx.typePrefixFile = this;
                }
                return Error::success();
              }
              case LinkFactVisibility:
              case LinkFactInit: {
                DataExtractor data(operands, /*IsLittleEndian=*/true);
                DataExtractor::Cursor c(0);
                uint64_t value = data.getULEB128(c);
                if (!c)
                  return c.takeError();
                bool init = tag == LinkFactInit;
                if (value > (init ? LinkFactTLSDynamic : LinkFactHidden))
                  return createStringError(
                      Twine(init ? "initialization" : "visibility") +
                      " of symbol " + Twine(index) + " is out of range");
                if (!apply || !sym)
                  return Error::success();
                // Images share a thread-local variable by its record, which
                // the file states before this.
                TlsRecordChunk *record = symtab.tlsRecords.empty()
                                             ? nullptr
                                             : symtab.tlsRecords.lookup(sym);
                // Every copy of a symbol and every reference to it states the
                // same symbol, which takes the most that one of them states.
                if (init) {
                  SymbolTable::VagueEntity &e =
                      symtab.vagueEntities[record ? record->sym : sym];
                  e.init = std::max<unsigned>(e.init, value);
                } else {
                  // A definition that the link keeps out of its exports is
                  // the image's alone, as a hidden one is.
                  LinkerDriver &driver = symtab.ctx.driver;
                  if (isa<Defined>(sym) && sym->getFile() == this &&
                      (driver.isLocalByVersionScript(sym->getName()) ||
                       driver.isExcludedLib(this)))
                    value = LinkFactHidden;
                  sym->visibility = std::max<unsigned>(sym->visibility, value);
                  if (record)
                    record->sym->visibility = sym->visibility;
                }
                return Error::success();
              }
              case LinkFactTlsRecord: {
                // A discarded copy's guard is the kept copy's, through the
                // symbol table. Bitcode's is named again by the object that
                // LTO makes of it.
                Symbol *guard = nullptr;
                if (!operands.empty()) {
                  DataExtractor data(operands, /*IsLittleEndian=*/true);
                  DataExtractor::Cursor c(0);
                  uint64_t g = data.getULEB128(c);
                  if (!c)
                    return c.takeError();
                  if (g >= symbols.size())
                    return createStringError("guard of symbol " + Twine(index) +
                                             " is out of range");
                  guard = symbols[g];
                }
                // No other unit names a variable that LTO made local.
                if (apply && sym && symtab.find(sym->getName()) == sym)
                  symtab.addTlsRecord(sym, isa<ObjFile>(this) ? guard : nullptr,
                                      this);
                return Error::success();
              }
              case LinkFactDescriptor:
              case LinkFactAddressSignificant:
                if (apply && sym) {
                  SymbolTable::VagueEntity &e = symtab.vagueEntities[sym];
                  e.descriptor |= tag == LinkFactDescriptor;
                  e.function |= tag == LinkFactAddressSignificant;
                }
                return Error::success();
              case LinkFactOverridable:
              case LinkFactOverridableByName: {
                // A default states its slot, then the slot it forwards to,
                // then 1 or 2 where the forward drops an argument.
                DataExtractor data(operands, /*IsLittleEndian=*/true);
                DataExtractor::Cursor c(0);
                uint64_t n = 0, slots[3];
                while (c && !data.eof(c) && n != 3)
                  slots[n++] = data.getULEB128(c);
                if (!c)
                  return c.takeError();
                if (!data.eof(c) || (n && tag == LinkFactOverridableByName) ||
                    (n && slots[0] > LinkFactMaxOverrideSlot) ||
                    (n > 1 && (slots[1] > LinkFactMaxOverrideSlot ||
                               slots[1] == slots[0])) ||
                    (n > 2 && slots[2] - 1 > 1))
                  return createStringError("overridable default of symbol " +
                                           Twine(index) + " is out of range");
                if (!apply || !sym || tag == LinkFactOverridableByName)
                  return Error::success();
                if (n) {
                  symtab.addOverrideDefault(
                      slots[0],
                      {sym->getName(),
                       n > 1 ? int8_t(slots[1]) : int8_t(-1),
                       n > 2 ? uint8_t(slots[2]) : uint8_t(0),
                       {}},
                      "", toString(this));
                  symtab.ownOverrides |= 1u << slots[0];
                } else if (defines(index)) {
                  SymbolTable::VagueEntity &e = symtab.vagueEntities[sym];
                  e.function = e.overridable = true;
                }
                return Error::success();
              }
              default:
                if (tag & LinkRecordCritical)
                  return createStringError("unknown critical tag " +
                                           Twine(tag));
                return Error::success();
              }
            }))
      return e;
  return Error::success();
}

ObjFile *ObjFile::create(COFFLinkerContext &ctx, COFFObjectFile *coffObj,
                         bool lazy) {
  return make<ObjFile>(ctx.getSymtab(MachineTypes(coffObj->getMachine())),
                       coffObj, lazy);
}

void ObjFile::parseLazy() {
  // Native object file.
  uint32_t numSymbols = coffObj->getNumberOfSymbols();
  for (uint32_t i = 0; i < numSymbols; ++i) {
    COFFSymbolRef coffSym = check(coffObj->getSymbol(i));
    if (coffSym.isUndefined() ||
        !(coffSym.isExternal() || isDefinedWeakExternal(coffSym)))
      continue;
    StringRef name = check(coffObj->getSymbolName(coffSym));
    if (coffSym.isAbsolute() && ignoredSymbolName(name))
      continue;
    symtab.addLazyObject(this, name);
    if (!lazy)
      return;
    i += coffSym.getNumberOfAuxSymbols();
  }
}

struct ECMapEntry {
  ulittle32_t src;
  ulittle32_t dst;
  ulittle32_t type;
};

void ObjFile::initializeECThunks() {
  for (SectionChunk *chunk : hybmpChunks) {
    if (chunk->getContents().size() % sizeof(ECMapEntry)) {
      Err(symtab.ctx) << "Invalid .hybmp chunk size "
                      << chunk->getContents().size();
      continue;
    }

    const uint8_t *end =
        chunk->getContents().data() + chunk->getContents().size();
    for (const uint8_t *iter = chunk->getContents().data(); iter != end;
         iter += sizeof(ECMapEntry)) {
      auto entry = reinterpret_cast<const ECMapEntry *>(iter);
      switch (entry->type) {
      case Arm64ECThunkType::Entry:
        symtab.addEntryThunk(getSymbol(entry->src), getSymbol(entry->dst));
        break;
      case Arm64ECThunkType::Exit:
        symtab.addExitThunk(getSymbol(entry->src), getSymbol(entry->dst));
        break;
      case Arm64ECThunkType::GuestExit:
        break;
      default:
        Warn(symtab.ctx) << "Ignoring unknown EC thunk type " << entry->type;
      }
    }
  }
}

void ObjFile::parse() {
  // Read section and symbol tables.
  initializeChunks();
  initializeSymbols();
  for (uint32_t i : gotTpoffIndexes)
    if (Symbol *s = symbols[i])
      symtab.tpoffWords.try_emplace(s);
  initializeFlags();
  initializeDependencies();
  initializeECThunks();

  // Link records name symbols by index, so they are read last. An object that
  // LLVM made for these environments always has a Symbols group.
  if (!linkRecordsSec && !forkObject)
    return;
  ArrayRef<uint8_t> contents;
  if (linkRecordsSec)
    cantFail(coffObj->getSectionContents(linkRecordsSec, contents));
  uint64_t flags;
  bool hasSymbols = false;
  StringRef facts;
  SmallVector<SectionChunk *, 0> resizable;
  std::vector<SwitchTable> tables;
  std::vector<RelocFact> siteFactsRead;
  // The section that the symbol at index defines, or null where it is none.
  auto sectionOf = [&](uint64_t index) -> const coff_section * {
    if (index >= coffObj->getNumberOfSymbols())
      return nullptr;
    COFFSymbolRef sym = check(coffObj->getSymbol(index));
    int32_t number = sym.getSectionNumber();
    return sym.isSectionDefinition() && number > 0 &&
                   uint32_t(number) <= coffObj->getNumberOfSections()
               ? getSection(number)
               : nullptr;
  };
  Error e = readCOFFLinkRecords(
      toStringRef(contents), flags,
      [&](uint64_t kind, StringRef payload) -> Error {
        switch (kind) {
        case LinkRecordSymbols:
          if (hasSymbols)
            return createStringError("two Symbols groups");
          hasSymbols = true;
          facts = payload;
          return Error::success();
        case LinkRecordSections: {
          // A count, then that many symbols, each of a code section the
          // linker may resize. A section that is not part of the link has no
          // chunk. Each symbol takes a byte or more, which bounds the count.
          DataExtractor data(payload, /*IsLittleEndian=*/true);
          DataExtractor::Cursor c(0);
          uint64_t count = data.getULEB128(c);
          if (c && count > payload.size() - c.tell())
            return createStringError(Twine(count) +
                                     " resizable sections, which the group "
                                     "does not hold");
          for (; c && count; --count) {
            uint64_t index = data.getULEB128(c);
            if (!c)
              break;
            if (index >= coffObj->getNumberOfSymbols())
              return createStringError("resizable section " + Twine(index) +
                                       ", which the file does not have");
            const coff_section *sec = sectionOf(index);
            if (!sec || !(sec->Characteristics & IMAGE_SCN_CNT_CODE))
              return createStringError("resizable section " + Twine(index) +
                                       " is not a code section");
            if (auto *d = dyn_cast_or_null<DefinedRegular>(symbols[index]))
              resizable.push_back(d->getChunk());
          }

          // Then a count and that many dispatches through jump tables, each
          // the symbols of its branch, table and base, the count and the type
          // of the table's entries, the symbol of the load and flags. The
          // branch and the load are in code and the table within its section.
          // A dispatch of a type that is not known, or of a section that is
          // not part of the link, is not kept.
          for (count = c && !data.eof(c) ? data.getULEB128(c) : 0; c && count;
               --count) {
            uint64_t fields[7];
            for (uint64_t &field : fields)
              field = data.getULEB128(c);
            if (!c)
              break;
            static const uint8_t widths[] = {1, 1, 2, 2, 4, 4, 8, 1, 2, 1, 2};
            uint64_t width =
                fields[4] < std::size(widths) ? widths[fields[4]] : 0;
            DefinedRegular *places[4];
            for (int i = 0; i != 4; ++i) {
              uint64_t index = fields[i == 3 ? 5 : i];
              bool inCode = i == 0 || i == 3;
              if (index >= coffObj->getNumberOfSymbols())
                return createStringError("switch table at symbol " +
                                         Twine(index) +
                                         ", which the file does not have");
              COFFSymbolRef sym = check(coffObj->getSymbol(index));
              int32_t number = sym.getSectionNumber();
              if (number <= 0 ||
                  uint32_t(number) > coffObj->getNumberOfSections() ||
                  (inCode &&
                   !(getSection(number)->Characteristics & IMAGE_SCN_CNT_CODE)))
                return createStringError("switch table at symbol " +
                                         Twine(index) + ", which is not in " +
                                         (inCode ? "code" : "a section"));
              uint32_t size = getSection(number)->SizeOfRawData;
              if (i == 1 && width &&
                  (!fields[3] || sym.getValue() > size ||
                   fields[3] > (size - sym.getValue()) / width))
                return createStringError("switch table of " + Twine(fields[3]) +
                                         " entries at symbol " + Twine(index) +
                                         ", which its section does not hold");
              places[i] = dyn_cast_or_null<DefinedRegular>(symbols[index]);
            }
            if (width && !is_contained(places, nullptr))
              tables.push_back({places[0], places[1], places[2], places[3],
                                fields[3], uint8_t(fields[4]),
                                uint8_t(fields[6] & SwitchTableRangeChecked)});
          }

          // Then a count and that many sections with loops a cache line is
          // to keep, each its symbol, then the places 0, 16, 32 and 48 bytes
          // into a 64-byte line, as bits 0 to 3, at which it may not begin,
          // plus sixteen times a count, and that many loops for the link to
          // bar by, which relaxChunk reads.
          for (count = c && !data.eof(c) ? data.getULEB128(c) : 0; c && count;
               --count) {
            uint64_t index = data.getULEB128(c), head = data.getULEB128(c);
            uint64_t begin = c.tell();
            for (uint64_t n = head >> 4; c && n; --n)
              data.getULEB128(c);
            if (!c)
              break;
            if (index >= coffObj->getNumberOfSymbols() || (head & 15) == 15)
              return createStringError("section " + Twine(index) +
                                       " barred from places " +
                                       Twine(head & 15) + " of a line");
            if (auto *d = dyn_cast_or_null<DefinedRegular>(symbols[index])) {
              d->getChunk()->linePlacesBarred = head & 15;
              lineLoops[d->getChunk()] =
                  payload.substr(begin, c.tell() - begin);
            }
          }
          return c.takeError();
        }
        case LinkRecordSpans: {
          // Lists to the group's end, each the symbols of the section that
          // holds its fields and of the code section they span, a count and
          // that many spans: a kind plus sixteen times the distance of its
          // field from the last one, and its kind's operand. A list of a
          // section that is not part of the link is not kept.
          DataExtractor data(payload, /*IsLittleEndian=*/true);
          DataExtractor::Cursor c(0);
          while (c && !data.eof(c)) {
            SectionChunk *secs[2] = {};
            const coff_section *headers[2];
            uint32_t sizes[2];
            for (int i = 0; i != 2; ++i) {
              uint64_t index = data.getULEB128(c);
              if (!c)
                break;
              if (index >= coffObj->getNumberOfSymbols())
                return createStringError("spans of section " + Twine(index) +
                                         ", which the file does not have");
              const coff_section *sec = sectionOf(index);
              if (!sec || (i && !(sec->Characteristics & IMAGE_SCN_CNT_CODE)))
                return createStringError("spans of section " + Twine(index) +
                                         ", which is no " + (i ? "code " : "") +
                                         "section");
              // A section without bytes holds no field.
              headers[i] = sec;
              sizes[i] = sec->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA
                             ? 0
                             : sec->SizeOfRawData;
              if (auto *d = dyn_cast_or_null<DefinedRegular>(symbols[index]))
                secs[i] = d->getChunk();
            }
            uint64_t count = data.getULEB128(c), offset = 0;
            if (c && count > payload.size() - c.tell())
              return createStringError(Twine(count) +
                                       " spans, which the group does not hold");
            uint64_t begin = c.tell();
            bool jumps = false;
            for (uint64_t n = count; c && n; --n) {
              uint64_t head = data.getULEB128(c);
              uint32_t kind = head & 15;
              offset += head >> 4;
              jumps |= kind == SpanJump32;
              int64_t operand = kind == SpanAddend32 ? data.getSLEB128(c)
                                : kind >= SpanData   ? data.getULEB128(c)
                                                     : 0;
              if (!c)
                break;
              // A LEB128 ends where its bytes say.
              static const uint8_t widths[] = {1, 4, 4, 4, 4, 4, 1, 2,
                                               4, 8, 1, 1, 1, 2, 2, 4};
              if (offset > sizes[0] || widths[kind] > sizes[0] - offset ||
                  (kind < SpanData && headers[0] != headers[1]) ||
                  (kind >= SpanData && kind != SpanAddend32 &&
                   uint64_t(operand) > sizes[1]))
                return createStringError("span at offset 0x" +
                                         Twine::utohexstr(offset) +
                                         " is outside its section");
            }
            if (c && secs[0] && secs[1])
              spanLists.push_back({secs[0], secs[1],
                                   payload.substr(begin, c.tell() - begin),
                                   jumps});
          }
          return c.takeError();
        }
        // The facts of the relocations of a section, which its symbol
        // names. A section that is not part of the link has no chunk.
        case LinkRecordSiteFacts:
          return readCOFFSiteFacts(
              payload,
              [&](uint64_t index, uint64_t reloc, uint64_t fact,
                  uint64_t value) -> Error {
                const coff_section *sec = sectionOf(index);
                if (!sec || reloc >= sec->NumberOfRelocations)
                  return createStringError(
                      "site fact of relocation " + Twine(reloc) +
                      " of the section of symbol " + Twine(index) +
                      ", which the file does not have");
                auto *d = dyn_cast_or_null<DefinedRegular>(symbols[index]);
                if (d && isUInt<32>(fact))
                  siteFactsRead.push_back(
                      {d->getChunk(), uint32_t(reloc), uint32_t(fact), value});
                return Error::success();
              });
        default:
          if (kind & LinkRecordCritical)
            return createStringError("unknown critical kind " + Twine(kind));
          return Error::success();
        }
      });
  if (!e && forkObject && !hasSymbols)
    e = createStringError("no Symbols group");
  // The facts are applied only where the whole container is valid.
  if (!e)
    e = readLinkFacts(facts, symbols, [&](uint64_t i) {
      return check(coffObj->getSymbol(i)).getSectionNumber() > 0;
    });
  if (e) {
    Err(symtab.ctx) << this << ": .llvm_link_records: " << std::move(e);
    return;
  }
  fusedChecked = forkObject && (flags & LinkRecordsFusedChecked);
  // What a load states of itself rests on no fused type.
  if (!fusedChecked)
    llvm::erase_if(siteFactsRead, [&](const RelocFact &f) {
      return !forkObject || f.fact != SiteFactValueOnly;
    });
  llvm::stable_sort(siteFactsRead, [](const RelocFact &a, const RelocFact &b) {
    return a.chunk != b.chunk ? std::less<>()(a.chunk, b.chunk)
                              : a.reloc < b.reloc;
  });
  siteFacts = std::move(siteFactsRead);
  for (SectionChunk *c : resizable) {
    c->resizable = true;
    c->movable = c->getArch() != Triple::x86_64 ||
                 llvm::any_of(c->getRelocs(), [](const coff_relocation &rel) {
                   return relocationMovesCode(rel.Type);
                 });
  }
  // A section with a list of its own fields states its spans. The link moves
  // code in other ways too where a relocation says so, which no span follows.
  for (const SpanList &l : spanLists) {
    if (l.field != l.target || !l.target->resizable)
      continue;
    if (l.target->movable)
      Err(symtab.ctx) << this << ": " << l.target->getSectionName()
                      << " states spans, and the link moves its code";
    l.target->compacts = true;
  }
  llvm::stable_sort(spanLists, [](const SpanList &a, const SpanList &b) {
    return a.field->getSectionNumber() < b.field->getSectionNumber();
  });
  if (!spanLists.empty()) {
    spanStarts.resize(coffObj->getNumberOfSections() + 2);
    for (const SpanList &l : spanLists)
      ++spanStarts[l.field->getSectionNumber() + 1];
    for (size_t i = 1, e = spanStarts.size(); i != e; ++i)
      spanStarts[i] += spanStarts[i - 1];
  }
  // The link moves a symbol of a section it resizes by writing the value in
  // its record, so each such symbol gets a record of its own here, before any
  // copy of the symbol is made, and every copy follows. A symbol at the
  // section's start stays there.
  if (!resizable.empty())
    for (Symbol *sym : symbols)
      if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
          d && d->file == this && d->data && (*d->data)->resizable) {
        if (d->getName().starts_with("__cfi_"))
          typePrefixes.push_back({*d->data, d->getValue()});
        if (!d->getValue())
          continue;
        size_t size = coffObj->getSymbolTableEntrySize();
        auto *record =
            reinterpret_cast<coff_symbol_generic *>(bAlloc().Allocate(size, 1));
        memcpy(record, d->sym, size);
        d->sym = record;
        movingSymbols.push_back({*d->data, record});
      }
  llvm::stable_sort(tables, [](const SwitchTable &a, const SwitchTable &b) {
    return std::less<>()(a.branch->getChunk(), b.branch->getChunk());
  });
  switchTables = std::move(tables);
}

// The run of v, which is in the order of the chunks that chunk gives, of sc.
template <class T, class F>
static ArrayRef<T> getRunOf(ArrayRef<T> v, const SectionChunk *sc, F chunk) {
  const T *first = llvm::partition_point(
      v, [&](const T &t) { return std::less<>()(chunk(t), sc); });
  return ArrayRef(first, std::partition_point(first, v.end(), [&](const T &t) {
                    return chunk(t) == sc;
                  }));
}

ArrayRef<SwitchTable> ObjFile::getSwitchTables(const SectionChunk *sc) const {
  return getRunOf(ArrayRef(switchTables), sc,
                  [](const SwitchTable &t) { return t.branch->getChunk(); });
}

ArrayRef<SpanList> ObjFile::getSpans(const SectionChunk *sc) const {
  if (spanStarts.empty())
    return {};
  uint32_t n = sc->getSectionNumber();
  return ArrayRef(spanLists).slice(spanStarts[n],
                                   spanStarts[n + 1] - spanStarts[n]);
}

ArrayRef<RelocFact> ObjFile::getSiteFacts(const SectionChunk *sc) const {
  return getRunOf(ArrayRef(siteFacts), sc,
                  [](const RelocFact &f) { return f.chunk; });
}

std::optional<uint64_t> ObjFile::getSiteFact(const SectionChunk *sc,
                                             size_t reloc,
                                             uint32_t fact) const {
  if (siteFacts.empty())
    return std::nullopt;
  ArrayRef<RelocFact> facts = getSiteFacts(sc);
  for (auto it = llvm::partition_point(
           facts, [&](const RelocFact &f) { return f.reloc < reloc; });
       it != facts.end() && it->reloc == reloc; ++it)
    if (it->fact == fact)
      return it->value;
  return std::nullopt;
}

const coff_section *ObjFile::getSection(uint32_t i) {
  auto sec = coffObj->getSection(i);
  if (!sec)
    Fatal(symtab.ctx) << "getSection failed: #" << i << ": " << sec.takeError();
  return *sec;
}

// We set SectionChunk pointers in the SparseChunks vector to this value
// temporarily to mark comdat sections as having an unknown resolution. As we
// walk the object file's symbol table, once we visit either a leader symbol or
// an associative section definition together with the parent comdat's leader,
// we set the pointer to either nullptr (to mark the section as discarded) or a
// valid SectionChunk for that section.
static SectionChunk *const pendingComdat = reinterpret_cast<SectionChunk *>(1);

void ObjFile::initializeChunks() {
  uint32_t numSections = coffObj->getNumberOfSections();
  sparseChunks.resize(numSections + 1);
  for (uint32_t i = 1; i < numSections + 1; ++i) {
    const coff_section *sec = getSection(i);
    if (sec->Characteristics & IMAGE_SCN_LNK_COMDAT)
      sparseChunks[i] = pendingComdat;
    else
      sparseChunks[i] = readSection(i, nullptr, "");
  }
}

SectionChunk *ObjFile::readSection(uint32_t sectionNumber,
                                   const coff_aux_section_definition *def,
                                   StringRef leaderName) {
  const coff_section *sec = getSection(sectionNumber);

  StringRef name;
  if (Expected<StringRef> e = coffObj->getSectionName(sec))
    name = *e;
  else
    Fatal(symtab.ctx) << "getSectionName failed: #" << sectionNumber << ": "
                      << e.takeError();

  if (name == ".drectve") {
    ArrayRef<uint8_t> data;
    cantFail(coffObj->getSectionContents(sec, data));
    directives = StringRef((const char *)data.data(), data.size());
    return nullptr;
  }

  if (name == ".llvm_addrsig") {
    addrsigSec = sec;
    return nullptr;
  }

  if (name == ".llvm_link_records") {
    linkRecordsSec = sec;
    return nullptr;
  }

  if (name == ".llvm.call-graph-profile") {
    callgraphSec = sec;
    return nullptr;
  }

  if (symtab.ctx.config.discardSection.contains(name))
    return nullptr;

  // Object files may have DWARF debug info or MS CodeView debug info
  // (or both).
  //
  // DWARF sections don't need any special handling from the perspective
  // of the linker; they are just a data section containing relocations.
  // We can just link them to complete debug info.
  //
  // CodeView needs linker support. We need to interpret debug info,
  // and then write it to a separate .pdb file.

  // Ignore DWARF debug info unless requested to be included.
  if (!symtab.ctx.config.includeDwarfChunks && name.starts_with(".debug_"))
    return nullptr;

  if (sec->Characteristics & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
    return nullptr;
  SectionChunk *c;
  if (isArm64EC(getMachineType()))
    c = make<SectionChunkEC>(this, sec);
  else
    c = make<SectionChunk>(this, sec);
  if (def)
    c->checksum = def->CheckSum;
  for (const coff_relocation &rel : c->getRelocs())
    if (rel.SymbolTableIndex >= coffObj->getNumberOfSymbols() &&
        !relocationNamesNoSymbol(rel.Type))
      Err(symtab.ctx) << this << ": relocation in " << name
                      << " has invalid symbol index "
                      << Twine(rel.SymbolTableIndex);
    else if (rel.Type >= IMAGE_REL_LLVM_GOTTPOFF32 &&
             rel.Type <= IMAGE_REL_LLVM_GOTTPOFF_LOW12L)
      gotTpoffIndexes.push_back(rel.SymbolTableIndex);

  // CodeView sections are stored to a different vector because they are not
  // linked in the regular manner.
  if (c->isCodeView())
    debugChunks.push_back(c);
  else if (name == ".gfids$y")
    guardFidChunks.push_back(c);
  else if (name == ".giats$y")
    guardIATChunks.push_back(c);
  else if (name == ".gljmp$y")
    guardLJmpChunks.push_back(c);
  else if (name == ".gehcont$y")
    guardEHContChunks.push_back(c);
  else if (name == ".sxdata")
    sxDataChunks.push_back(c);
  else if (isArm64EC(getMachineType()) && name == ".hybmp$x")
    hybmpChunks.push_back(c);
  else if (symtab.ctx.config.tailMerge && sec->NumberOfRelocations == 0 &&
           name == ".rdata" && leaderName.starts_with("??_C@"))
    // COFF sections that look like string literal sections (i.e. no
    // relocations, in .rdata, leader symbol name matches the MSVC name mangling
    // for string literals) are subject to string tail merging.
    MergeChunk::addSection(symtab.ctx, c);
  else if (name == ".rsrc" || name.starts_with(".rsrc$"))
    resourceChunks.push_back(c);
  else if ((symtab.ctx.config.functionOverride || forkObject) &&
           (name.ends_with("_$fo_bdd$") || name.ends_with("_$fo_rvas$"))) {
    // A function override's diagram and candidates are read by name, never
    // laid out. An object that LLVM made for Windows Itanium or NT-POSIX
    // holds them in COMDAT sections, which are read once it is known as one.
    c->live = false;
    symtab.functionOverrideSections.push_back(c);
  } else if (!(sec->Characteristics & llvm::COFF::IMAGE_SCN_LNK_INFO))
    chunks.push_back(c);

  return c;
}

void ObjFile::includeResourceChunks() {
  chunks.insert(chunks.end(), resourceChunks.begin(), resourceChunks.end());
}

void ObjFile::readAssociativeDefinition(
    COFFSymbolRef sym, const coff_aux_section_definition *def) {
  readAssociativeDefinition(sym, def, def->getNumber(sym.isBigObj()));
}

void ObjFile::readAssociativeDefinition(COFFSymbolRef sym,
                                        const coff_aux_section_definition *def,
                                        uint32_t parentIndex) {
  SectionChunk *parent = sparseChunks[parentIndex];
  int32_t sectionNumber = sym.getSectionNumber();

  auto diag = [&]() {
    StringRef name = check(coffObj->getSymbolName(sym));

    StringRef parentName;
    const coff_section *parentSec = getSection(parentIndex);
    if (Expected<StringRef> e = coffObj->getSectionName(parentSec))
      parentName = *e;
    Err(symtab.ctx) << toString(this) << ": associative comdat " << name
                    << " (sec " << sectionNumber
                    << ") has invalid reference to section " << parentName
                    << " (sec " << parentIndex << ")";
  };

  if (parent == pendingComdat) {
    // This can happen if an associative comdat refers to another associative
    // comdat that appears after it (invalid per COFF spec) or to a section
    // without any symbols.
    diag();
    return;
  }

  // Check whether the parent is prevailing. If it is, so are we, and we read
  // the section; otherwise mark it as discarded.
  if (parent) {
    SectionChunk *c = readSection(sectionNumber, def, "");
    sparseChunks[sectionNumber] = c;
    if (c) {
      c->selection = IMAGE_COMDAT_SELECT_ASSOCIATIVE;
      parent->addAssociative(c);
    }
  } else {
    sparseChunks[sectionNumber] = nullptr;
  }
}

void ObjFile::recordPrevailingSymbolForMingw(
    COFFSymbolRef sym, DenseMap<StringRef, uint32_t> &prevailingSectionMap) {
  // For comdat symbols in executable sections, where this is the copy
  // of the section chunk we actually include instead of discarding it,
  // add the symbol to a map to allow using it for implicitly
  // associating .[px]data$<func> sections to it.
  // Use the suffix from the .text$<func> instead of the leader symbol
  // name, for cases where the names differ (i386 mangling/decorations,
  // cases where the leader is a weak symbol named .weak.func.default*).
  int32_t sectionNumber = sym.getSectionNumber();
  SectionChunk *sc = sparseChunks[sectionNumber];
  if (sc && sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE) {
    StringRef name = sc->getSectionName().split('$').second;
    prevailingSectionMap[name] = sectionNumber;
  }
}

void ObjFile::maybeAssociateSEHForMingw(
    COFFSymbolRef sym, const coff_aux_section_definition *def,
    const DenseMap<StringRef, uint32_t> &prevailingSectionMap) {
  StringRef name = check(coffObj->getSymbolName(sym));
  if (name.consume_front(".pdata$") || name.consume_front(".xdata$") ||
      name.consume_front(".eh_frame$")) {
    // For MinGW, treat .[px]data$<func> and .eh_frame$<func> as implicitly
    // associative to the symbol <func>.
    auto parentSym = prevailingSectionMap.find(name);
    if (parentSym != prevailingSectionMap.end())
      readAssociativeDefinition(sym, def, parentSym->second);
  }
}

Symbol *ObjFile::createRegular(COFFSymbolRef sym) {
  SectionChunk *sc = sparseChunks[sym.getSectionNumber()];
  if (sym.isExternal() || isDefinedWeakExternal(sym)) {
    StringRef name = check(coffObj->getSymbolName(sym));
    if (sc)
      return symtab.addRegular(this, name, sym.getGeneric(), sc,
                               sym.getValue());
    // For MinGW symbols named .weak.* that point to a discarded section,
    // don't create an Undefined symbol. If nothing ever refers to the symbol,
    // everything should be fine. If something actually refers to the symbol
    // (e.g. the undefined weak alias), linking will fail due to undefined
    // references at the end.
    if (symtab.ctx.config.mingw && name.starts_with(".weak."))
      return nullptr;
    return symtab.addUndefined(name, this, false);
  }
  if (sc) {
    const coff_symbol_generic *symGen = sym.getGeneric();
    if (sym.isSection()) {
      auto *customSymGen = cloneSymbol(sym);
      customSymGen->Value = 0;
      symGen = customSymGen;
    }
    return make<DefinedRegular>(this, /*Name*/ "", /*IsCOMDAT*/ false,
                                /*IsExternal*/ false, symGen, sc);
  }
  return nullptr;
}

void ObjFile::initializeSymbols() {
  uint32_t numSymbols = coffObj->getNumberOfSymbols();
  symbols.resize(numSymbols);

  SmallVector<
      std::tuple<Symbol *, const coff_aux_weak_external *, Symbol::WeakKind>, 8>
      weakAliases;
  SmallVector<Symbol *, 0> imps, weakRefs;
  std::vector<uint32_t> pendingIndexes;
  pendingIndexes.reserve(numSymbols);

  DenseMap<StringRef, uint32_t> prevailingSectionMap;
  std::vector<const coff_aux_section_definition *> comdatDefs(
      coffObj->getNumberOfSections() + 1);
  COFFLinkerContext &ctx = symtab.ctx;

  for (uint32_t i = 0; i < numSymbols; ++i) {
    COFFSymbolRef coffSym = check(coffObj->getSymbol(i));
    bool prevailingComdat;
    if (coffSym.isUndefined()) {
      symbols[i] = createUndefined(coffSym, false);
      if (symbols[i]->getName().starts_with("__imp_"))
        imps.push_back(symbols[i]);
    } else if (coffSym.isWeakExternal() &&
               coffSym.getSectionNumber() == IMAGE_SYM_UNDEFINED) {
      auto aux = coffSym.getAux<coff_aux_weak_external>();
      if (aux->TagIndex >= numSymbols) {
        Err(ctx) << this << ": weak external "
                 << check(coffObj->getSymbolName(coffSym))
                 << " has invalid symbol index " << Twine(aux->TagIndex);
        symbols[i] = createUndefined(coffSym, false);
        i += coffSym.getNumberOfAuxSymbols();
        continue;
      }
      bool overrideLazy = true;

      // On ARM64EC, external function calls emit a pair of weak-dependency
      // aliases: func to #func and #func to the func guess exit thunk
      // (instead of a single undefined func symbol, which would be emitted on
      // other targets). Allow such aliases to be overridden by lazy archive
      // symbols, just as we would for undefined symbols.
      if (isArm64EC(getMachineType()) &&
          aux->Characteristics == IMAGE_WEAK_EXTERN_ANTI_DEPENDENCY) {
        COFFSymbolRef targetSym = check(coffObj->getSymbol(aux->TagIndex));
        if (!targetSym.isAnyUndefined()) {
          // If the target is defined, it may be either a guess exit thunk or
          // the actual implementation. If it's the latter, consider the alias
          // to be part of the implementation and override potential lazy
          // archive symbols.
          StringRef targetName = check(coffObj->getSymbolName(targetSym));
          StringRef name = check(coffObj->getSymbolName(coffSym));
          std::optional<std::string> mangledName =
              getArm64ECMangledFunctionName(name);
          overrideLazy = mangledName == targetName;
        } else {
          overrideLazy = false;
        }
      }

      // Weak externals bind as link.exe binds them, except GNU as's weak
      // definition (a NOLIBRARY one whose default is defined), which binds as
      // an ELF weak definition does. An earlier strong reference drops a
      // NOLIBRARY or LIBRARY default, and an ANTI_DEPENDENCY one sets none for
      // a name an earlier file mentioned.
      Symbol *prior = symtab.find(check(coffObj->getSymbolName(coffSym)));
      auto *priorUndef = dyn_cast_or_null<Undefined>(prior);
      bool sets = true;
      Symbol::WeakKind kind = Symbol::Alias;
      switch (aux->Characteristics) {
      case IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY:
        if (check(coffObj->getSymbol(aux->TagIndex)).getSectionNumber() > 0) {
          kind = Symbol::WeakDefinition;
          if (prior && prior->isLazy())
            symtab.warnWeakDefinition(prior, prior->getFile(), this);
          break;
        }
        kind = Symbol::NoLibrary;
        // A bitcode file's weak reference defers the undefined symbol.
        sets = !priorUndef || priorUndef->weakAlias || prior->deferUndefined;
        break;
      case IMAGE_WEAK_EXTERN_SEARCH_LIBRARY:
        kind = Symbol::Library;
        overrideLazy = false;
        sets = !priorUndef || priorUndef->weakAlias;
        break;
      case IMAGE_WEAK_EXTERN_ANTI_DEPENDENCY:
        kind = Symbol::AntiDependency;
        sets = !prior || prior->isLazy() || symtab.isEC();
        break;
      }
      symbols[i] = createUndefined(coffSym, overrideLazy);
      if (sets)
        weakAliases.emplace_back(symbols[i], aux, kind);
      if (kind == Symbol::NoLibrary || kind == Symbol::Library)
        weakRefs.push_back(symbols[i]);
    } else if (std::optional<Symbol *> optSym =
                   createDefined(coffSym, comdatDefs, prevailingComdat)) {
      symbols[i] = *optSym;
      if (ctx.config.mingw && prevailingComdat)
        recordPrevailingSymbolForMingw(coffSym, prevailingSectionMap);
    } else {
      // createDefined() returns std::nullopt if a symbol belongs to a section
      // that was pending at the point when the symbol was read. This can happen
      // in two cases:
      // 1) section definition symbol for a comdat leader;
      // 2) symbol belongs to a comdat section associated with another section.
      // In both of these cases, we can expect the section to be resolved by
      // the time we finish visiting the remaining symbols in the symbol
      // table. So we postpone the handling of this symbol until that time.
      pendingIndexes.push_back(i);
    }
    i += coffSym.getNumberOfAuxSymbols();
  }

  for (uint32_t i : pendingIndexes) {
    COFFSymbolRef sym = check(coffObj->getSymbol(i));
    if (const coff_aux_section_definition *def = sym.getSectionDefinition()) {
      if (def->Selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE)
        readAssociativeDefinition(sym, def);
      else if (ctx.config.mingw)
        maybeAssociateSEHForMingw(sym, def, prevailingSectionMap);
    }
    if (sparseChunks[sym.getSectionNumber()] == pendingComdat) {
      StringRef name = check(coffObj->getSymbolName(sym));
      Log(ctx) << "comdat section " << name
               << " without leader and unassociated, discarding";
      continue;
    }
    symbols[i] = createRegular(sym);
  }

  for (auto [sym, aux, kind] : weakAliases)
    addWeakAlias(sym, symbols[aux->TagIndex], kind);

  // Weakness belongs to the reference. An object reaches X through __imp_X,
  // so one that keeps no weak reference to X refers to X strongly, which
  // cancels a weak reference of another object in either order, where an
  // object for Windows Itanium or NT-POSIX names X or __imp_X: a link of
  // other producers' objects alone binds as theirs do.
  auto ours = [](Symbol *x, Symbol *imp) {
    return x->forkReferenced || imp->forkReferenced;
  };
  for (Symbol *x : weakRefs)
    if (Symbol *imp = symtab.find(("__imp_" + x->getName()).str());
        imp && imp->strongImport && ours(x, imp))
      symtab.addStrongReference(x);
  for (Symbol *imp : imps) {
    Symbol *x = symtab.find(imp->getName().substr(strlen("__imp_")));
    if (is_contained(weakRefs, x))
      continue;
    imp->strongImport = true;
    if (x && ours(x, imp))
      symtab.addStrongReference(x);
  }

  // Free the memory used by sparseChunks now that symbol loading is finished.
  decltype(sparseChunks)().swap(sparseChunks);
}

Symbol *ObjFile::createUndefined(COFFSymbolRef sym, bool overrideLazy) {
  StringRef name = check(coffObj->getSymbolName(sym));
  Symbol *s = symtab.addUndefined(name, this, overrideLazy);
  if (!sym.isWeakExternal())
    symtab.addStrongReference(s);

  // Add an anti-dependency alias for undefined AMD64 symbols on the ARM64EC
  // target.
  if (symtab.isEC() && getMachineType() == AMD64) {
    auto u = dyn_cast<Undefined>(s);
    if (u && !u->weakAlias) {
      if (std::optional<std::string> mangledName =
              getArm64ECMangledFunctionName(name)) {
        Symbol *m = symtab.addUndefined(saver().save(*mangledName), this,
                                        /*overrideLazy=*/false);
        u->setWeakAlias(m, Symbol::AntiDependency);
      }
    }
  }
  return s;
}

static const coff_aux_section_definition *findSectionDef(COFFObjectFile *obj,
                                                         int32_t section) {
  uint32_t numSymbols = obj->getNumberOfSymbols();
  for (uint32_t i = 0; i < numSymbols; ++i) {
    COFFSymbolRef sym = check(obj->getSymbol(i));
    if (sym.getSectionNumber() != section)
      continue;
    if (const coff_aux_section_definition *def = sym.getSectionDefinition())
      return def;
  }
  return nullptr;
}

uint32_t ObjFile::getComdatLength(const SectionChunk *c) {
  const coff_aux_section_definition *def =
      findSectionDef(coffObj.get(), c->getSectionNumber());
  return def ? uint32_t(def->Length) : c->getSize();
}

void ObjFile::handleComdatSelection(
    COFFSymbolRef sym, COMDATType &selection, bool &prevailing,
    DefinedRegular *leader,
    const llvm::object::coff_aux_section_definition *def) {
  if (prevailing)
    return;
  // There's already an existing comdat for this symbol: `Leader`.
  // Use the comdats's selection field to determine if the new
  // symbol in `Sym` should be discarded, produce a duplicate symbol
  // error, etc.

  SectionChunk *leaderChunk = leader->getChunk();
  COMDATType leaderSelection = leaderChunk->selection;
  COFFLinkerContext &ctx = symtab.ctx;

  assert(leader->data && "Comdat leader without SectionChunk?");
  // Which models a thread-local's copies are of is known once every file's
  // directives are read.
  if (leaderChunk->getSectionName().split('$').first == ".tls")
    symtab.tlsCopies.push_back({leader, this});
  if (isa<BitcodeFile>(leader->file)) {
    // If the leader is only a LTO symbol, we don't know e.g. its final size
    // yet, so we can't do the full strict comdat selection checking yet.
    if (selection == IMAGE_COMDAT_SELECT_EXACT_MATCH)
      symtab.exactMatchCopies.push_back(
          {leader, make<SectionChunk>(this, getSection(sym))});
    selection = leaderSelection = IMAGE_COMDAT_SELECT_ANY;
  }

  if ((selection == IMAGE_COMDAT_SELECT_ANY &&
       leaderSelection == IMAGE_COMDAT_SELECT_LARGEST) ||
      (selection == IMAGE_COMDAT_SELECT_LARGEST &&
       leaderSelection == IMAGE_COMDAT_SELECT_ANY)) {
    // cl.exe picks "any" for vftables when building with /GR- and
    // "largest" when building with /GR. To be able to link object files
    // compiled with each flag, "any" and "largest" are merged as "largest".
    leaderSelection = selection = IMAGE_COMDAT_SELECT_LARGEST;
  }

  // GCCs __declspec(selectany) doesn't actually pick "any" but "same size as".
  // Clang on the other hand picks "any". To be able to link two object files
  // with a __declspec(selectany) declaration, one compiled with gcc and the
  // other with clang, we merge them as proper "same size as"
  if (ctx.config.mingw && ((selection == IMAGE_COMDAT_SELECT_ANY &&
                            leaderSelection == IMAGE_COMDAT_SELECT_SAME_SIZE) ||
                           (selection == IMAGE_COMDAT_SELECT_SAME_SIZE &&
                            leaderSelection == IMAGE_COMDAT_SELECT_ANY))) {
    leaderSelection = selection = IMAGE_COMDAT_SELECT_SAME_SIZE;
  }

  // Other than that, comdat selections must match.  This is a bit more
  // strict than link.exe which allows merging "any" and "largest" if "any"
  // is the first symbol the linker sees, and it allows merging "largest"
  // with everything (!) if "largest" is the first symbol the linker sees.
  // Making this symmetric independent of which selection is seen first
  // seems better though.
  // (This behavior matches ModuleLinker::getComdatResult().)
  if (selection != leaderSelection) {
    Log(ctx) << "conflicting comdat type for " << symtab.printSymbol(leader)
             << ": " << (int)leaderSelection << " in " << leader->getFile()
             << " and " << (int)selection << " in " << this;
    symtab.reportDuplicate(leader, this);
    return;
  }

  switch (selection) {
  case IMAGE_COMDAT_SELECT_NODUPLICATES:
    symtab.reportDuplicate(leader, this);
    break;

  case IMAGE_COMDAT_SELECT_ANY:
    // Nothing to do.
    break;

  case IMAGE_COMDAT_SELECT_SAME_SIZE:
    if (leaderChunk->getSize() != getSection(sym)->SizeOfRawData) {
      if (!ctx.config.mingw) {
        symtab.reportDuplicate(leader, this);
      } else {
        const coff_aux_section_definition *leaderDef = nullptr;
        if (leaderChunk->file)
          leaderDef = findSectionDef(leaderChunk->file->getCOFFObj(),
                                     leaderChunk->getSectionNumber());
        if (!leaderDef || leaderDef->Length != def->Length)
          symtab.reportDuplicate(leader, this);
      }
    }
    break;

  case IMAGE_COMDAT_SELECT_EXACT_MATCH: {
    SectionChunk newChunk(this, getSection(sym));
    // link.exe only compares section contents here and doesn't complain
    // if the two comdat sections have e.g. different alignment.
    // Match that.
    if (leaderChunk->getContents() != newChunk.getContents())
      symtab.reportDuplicate(leader, this, &newChunk, sym.getValue());
    break;
  }

  case IMAGE_COMDAT_SELECT_ASSOCIATIVE:
    // createDefined() is never called for IMAGE_COMDAT_SELECT_ASSOCIATIVE.
    // (This means lld-link doesn't produce duplicate symbol errors for
    // associative comdats while link.exe does, but associate comdats
    // are never extern in practice.)
    llvm_unreachable("createDefined not called for associative comdats");

  case IMAGE_COMDAT_SELECT_LARGEST:
    if (leaderChunk->getSize() < getSection(sym)->SizeOfRawData) {
      // Replace the existing comdat symbol with the new one.
      StringRef name = check(coffObj->getSymbolName(sym));
      // FIXME: This is incorrect: With /opt:noref, the previous sections
      // make it into the final executable as well. Correct handling would
      // be to undo reading of the whole old section that's being replaced,
      // or doing one pass that determines what the final largest comdat
      // is for all IMAGE_COMDAT_SELECT_LARGEST comdats and then reading
      // only the largest one.
      replaceSymbol<DefinedRegular>(leader, this, name, /*IsCOMDAT*/ true,
                                    /*IsExternal*/ true, sym.getGeneric(),
                                    nullptr);
      prevailing = true;
    }
    break;

  case IMAGE_COMDAT_SELECT_NEWEST:
    llvm_unreachable("should have been rejected earlier");
  }
}

std::optional<Symbol *> ObjFile::createDefined(
    COFFSymbolRef sym,
    std::vector<const coff_aux_section_definition *> &comdatDefs,
    bool &prevailing) {
  prevailing = false;
  auto getName = [&]() { return check(coffObj->getSymbolName(sym)); };

  if (sym.isCommon()) {
    auto *c = make<CommonChunk>(sym);
    chunks.push_back(c);
    return symtab.addCommon(this, getName(), sym.getValue(), sym.getGeneric(),
                            c);
  }

  COFFLinkerContext &ctx = symtab.ctx;
  if (sym.isAbsolute()) {
    StringRef name = getName();

    if (name == "@feat.00")
      feat00Flags = sym.getValue();
    else if (name == "@comp.id" && sym.getValue() == LLVMCompId)
      forkObject = ctx.hasForkObject = true;
    // Skip special symbols.
    if (ignoredSymbolName(name))
      return nullptr;

    if (sym.isExternal() || isDefinedWeakExternal(sym))
      return symtab.addAbsolute(name, sym);
    return make<DefinedAbsolute>(ctx, name, sym);
  }

  int32_t sectionNumber = sym.getSectionNumber();
  if (sectionNumber == llvm::COFF::IMAGE_SYM_DEBUG)
    return nullptr;

  if (sym.isEmptySectionDeclaration()) {
    // The symbol names the section of that name another object defines,
    // which the driver binds once every object is read.
    auto *symGen = cloneSymbol(sym);
    // Ignore the Value offset of these symbols, as it may be a bitmask.
    symGen->Value = 0;
    auto *d = make<DefinedRegular>(this, /*name=*/"", /*isCOMDAT=*/false,
                                   /*isExternal=*/false, symGen);
    ctx.sectionSymbols.push_back(d);
    return d;
  }

  if (llvm::COFF::isReservedSectionNumber(sectionNumber))
    Fatal(ctx) << toString(this) << ": " << getName()
               << " should not refer to special section "
               << Twine(sectionNumber);

  if ((uint32_t)sectionNumber >= sparseChunks.size())
    Fatal(ctx) << toString(this) << ": " << getName()
               << " should not refer to non-existent section "
               << Twine(sectionNumber);

  // Comdat handling.
  // A comdat symbol consists of two symbol table entries.
  // The first symbol entry has the name of the section (e.g. .text), fixed
  // values for the other fields, and one auxiliary record.
  // The second symbol entry has the name of the comdat symbol, called the
  // "comdat leader".
  // When this function is called for the first symbol entry of a comdat,
  // it sets comdatDefs and returns std::nullopt, and when it's called for the
  // second symbol entry it reads comdatDefs and then sets it back to nullptr.

  // Handle comdat leader.
  if (const coff_aux_section_definition *def = comdatDefs[sectionNumber]) {
    comdatDefs[sectionNumber] = nullptr;
    DefinedRegular *leader;

    if (sym.isExternal()) {
      std::tie(leader, prevailing) =
          symtab.addComdat(this, getName(), sym.getGeneric());
    } else {
      leader = make<DefinedRegular>(this, /*Name*/ "", /*IsCOMDAT*/ false,
                                    /*IsExternal*/ false, sym.getGeneric());
      prevailing = true;
    }

    if (def->Selection < (int)IMAGE_COMDAT_SELECT_NODUPLICATES ||
        // Intentionally ends at IMAGE_COMDAT_SELECT_LARGEST: link.exe
        // doesn't understand IMAGE_COMDAT_SELECT_NEWEST either.
        def->Selection > (int)IMAGE_COMDAT_SELECT_LARGEST) {
      Fatal(ctx) << "unknown comdat type "
                 << std::to_string((int)def->Selection) << " for " << getName()
                 << " in " << toString(this);
    }
    COMDATType selection = (COMDATType)def->Selection;

    if (leader->isCOMDAT)
      handleComdatSelection(sym, selection, prevailing, leader, def);

    if (prevailing) {
      SectionChunk *c = readSection(sectionNumber, def, getName());
      sparseChunks[sectionNumber] = c;
      if (!c)
        return nullptr;
      c->sym = cast<DefinedRegular>(leader);
      c->selection = selection;
      cast<DefinedRegular>(leader)->data = &c->repl;
    } else {
      sparseChunks[sectionNumber] = nullptr;
    }
    return leader;
  }

  // Prepare to handle the comdat leader symbol by setting the section's
  // ComdatDefs pointer if we encounter a non-associative comdat.
  if (sparseChunks[sectionNumber] == pendingComdat) {
    if (const coff_aux_section_definition *def = sym.getSectionDefinition()) {
      if (def->Selection != IMAGE_COMDAT_SELECT_ASSOCIATIVE)
        comdatDefs[sectionNumber] = def;
    }
    return std::nullopt;
  }

  return createRegular(sym);
}

MachineTypes ObjFile::getMachineType() const {
  return static_cast<MachineTypes>(coffObj->getMachine());
}

ArrayRef<uint8_t> ObjFile::getDebugSection(StringRef secName) {
  if (SectionChunk *sec = SectionChunk::findByName(debugChunks, secName))
    return sec->consumeDebugMagic();
  return {};
}

// OBJ files systematically store critical information in a .debug$S stream,
// even if the TU was compiled with no debug info. At least two records are
// always there. S_OBJNAME stores a 32-bit signature, which is loaded into the
// PCHSignature member. S_COMPILE3 stores compile-time cmd-line flags. This is
// currently used to initialize the hotPatchable member.
void ObjFile::initializeFlags() {
  ArrayRef<uint8_t> data = getDebugSection(".debug$S");
  if (data.empty())
    return;

  DebugSubsectionArray subsections;

  BinaryStreamReader reader(data, llvm::endianness::little);
  ExitOnError exitOnErr;
  exitOnErr(reader.readArray(subsections, data.size()));

  for (const DebugSubsectionRecord &ss : subsections) {
    if (ss.kind() != DebugSubsectionKind::Symbols)
      continue;

    unsigned offset = 0;

    // Only parse the first two records. We are only looking for S_OBJNAME
    // and S_COMPILE3, and they usually appear at the beginning of the
    // stream.
    for (unsigned i = 0; i < 2; ++i) {
      Expected<CVSymbol> sym = readSymbolFromStream(ss.getRecordData(), offset);
      if (!sym) {
        consumeError(sym.takeError());
        return;
      }
      if (sym->kind() == SymbolKind::S_COMPILE3) {
        auto cs =
            cantFail(SymbolDeserializer::deserializeAs<Compile3Sym>(sym.get()));
        hotPatchable =
            (cs.Flags & CompileSym3Flags::HotPatch) != CompileSym3Flags::None;
      }
      if (sym->kind() == SymbolKind::S_OBJNAME) {
        auto objName = cantFail(SymbolDeserializer::deserializeAs<ObjNameSym>(
            sym.get()));
        if (objName.Signature)
          pchSignature = objName.Signature;
      }
      offset += sym->length();
    }
  }
}

// Depending on the compilation flags, OBJs can refer to external files,
// necessary to merge this OBJ into the final PDB. We currently support two
// types of external files: Precomp/PCH OBJs, when compiling with /Yc and /Yu.
// And PDB type servers, when compiling with /Zi. This function extracts these
// dependencies and makes them available as a TpiSource interface (see
// DebugTypes.h). Both cases only happen with cl.exe: clang-cl produces regular
// output even with /Yc and /Yu and with /Zi.
void ObjFile::initializeDependencies() {
  COFFLinkerContext &ctx = symtab.ctx;
  if (!ctx.config.debug)
    return;

  bool isPCH = false;

  ArrayRef<uint8_t> data = getDebugSection(".debug$P");
  if (!data.empty())
    isPCH = true;
  else
    data = getDebugSection(".debug$T");

  // symbols but no types, make a plain, empty TpiSource anyway, because it
  // simplifies adding the symbols later.
  if (data.empty()) {
    if (!debugChunks.empty())
      debugTypesObj = makeTpiSource(ctx, this);
    return;
  }

  // Get the first type record. It will indicate if this object uses a type
  // server (/Zi) or a PCH file (/Yu).
  CVTypeArray types;
  BinaryStreamReader reader(data, llvm::endianness::little);
  cantFail(reader.readArray(types, reader.getLength()));
  CVTypeArray::Iterator firstType = types.begin();
  if (firstType == types.end())
    return;

  // Remember the .debug$T or .debug$P section.
  debugTypes = data;

  // This object file is a PCH file that others will depend on.
  if (isPCH) {
    debugTypesObj = makePrecompSource(ctx, this);
    return;
  }

  // This object file was compiled with /Zi. Enqueue the PDB dependency.
  if (firstType->kind() == LF_TYPESERVER2) {
    TypeServer2Record ts = cantFail(
        TypeDeserializer::deserializeAs<TypeServer2Record>(firstType->data()));
    debugTypesObj = makeUseTypeServerSource(ctx, this, ts);
    enqueuePdbFile(ts.getName(), this);
    return;
  }

  // This object was compiled with /Yu. It uses types from another object file
  // with a matching signature.
  if (firstType->kind() == LF_PRECOMP) {
    PrecompRecord precomp = cantFail(
        TypeDeserializer::deserializeAs<PrecompRecord>(firstType->data()));
    // We're better off trusting the LF_PRECOMP signature. In some cases the
    // S_OBJNAME record doesn't contain a valid PCH signature.
    if (precomp.Signature)
      pchSignature = precomp.Signature;
    debugTypesObj = makeUsePrecompSource(ctx, this, precomp);
    // Drop the LF_PRECOMP record from the input stream.
    debugTypes = debugTypes.drop_front(firstType->RecordData.size());
    return;
  }

  // This is a plain old object file.
  debugTypesObj = makeTpiSource(ctx, this);
}

// The casing of the PDB path stamped in the OBJ can differ from the actual path
// on disk. With this, we ensure to always use lowercase as a key for the
// pdbInputFileInstances map, at least on Windows.
static std::string normalizePdbPath(StringRef path) {
#if defined(_WIN32)
  return path.lower();
#else // LINUX
  return std::string(path);
#endif
}

// If existing, return the actual PDB path on disk.
static std::optional<std::string>
findPdbPath(StringRef pdbPath, ObjFile *dependentFile, StringRef outputPath) {
  // Ensure the file exists before anything else. In some cases, if the path
  // points to a removable device, Driver::enqueuePath() would fail with an
  // error (EAGAIN, "resource unavailable try again") which we want to skip
  // silently.
  if (llvm::sys::fs::exists(pdbPath))
    return normalizePdbPath(pdbPath);

  StringRef objPath = !dependentFile->parentName.empty()
                          ? dependentFile->parentName
                          : dependentFile->getName();

  // Currently, type server PDBs are only created by MSVC cl, which only runs
  // on Windows, so we can assume type server paths are Windows style.
  StringRef pdbName = sys::path::filename(pdbPath, sys::path::Style::windows);

  // Check if the PDB is in the same folder as the OBJ.
  SmallString<128> path;
  sys::path::append(path, sys::path::parent_path(objPath), pdbName);
  if (llvm::sys::fs::exists(path))
    return normalizePdbPath(path);

  // Check if the PDB is in the output folder.
  path.clear();
  sys::path::append(path, sys::path::parent_path(outputPath), pdbName);
  if (llvm::sys::fs::exists(path))
    return normalizePdbPath(path);

  return std::nullopt;
}

PDBInputFile::PDBInputFile(COFFLinkerContext &ctx, MemoryBufferRef m)
    : InputFile(ctx.symtab, PDBKind, m) {}

PDBInputFile::~PDBInputFile() = default;

PDBInputFile *PDBInputFile::findFromRecordPath(const COFFLinkerContext &ctx,
                                               StringRef path,
                                               ObjFile *fromFile) {
  auto p = findPdbPath(path.str(), fromFile, ctx.config.outputFile);
  if (!p)
    return nullptr;
  auto it = ctx.pdbInputFileInstances.find(*p);
  if (it != ctx.pdbInputFileInstances.end())
    return it->second;
  return nullptr;
}

void PDBInputFile::parse() {
  symtab.ctx.pdbInputFileInstances[mb.getBufferIdentifier().str()] = this;

  std::unique_ptr<pdb::IPDBSession> thisSession;
  Error E = pdb::NativeSession::createFromPdb(
      MemoryBuffer::getMemBuffer(mb, false), thisSession);
  if (E) {
    loadErrorStr.emplace(toString(std::move(E)));
    return; // fail silently at this point - the error will be handled later,
            // when merging the debug type stream
  }

  session.reset(static_cast<pdb::NativeSession *>(thisSession.release()));

  pdb::PDBFile &pdbFile = session->getPDBFile();
  auto expectedInfo = pdbFile.getPDBInfoStream();
  // All PDB Files should have an Info stream.
  if (!expectedInfo) {
    loadErrorStr.emplace(toString(expectedInfo.takeError()));
    return;
  }
  debugTypesObj = makeTypeServerSource(symtab.ctx, this);
}

// Used only for DWARF debug info, which is not common (except in MinGW
// environments). This returns an optional pair of file name and line
// number for where the variable was defined.
std::optional<std::pair<StringRef, uint32_t>>
ObjFile::getVariableLocation(StringRef var) {
  if (!dwarf) {
    dwarf = make<DWARFCache>(DWARFContext::create(*getCOFFObj()));
    if (!dwarf)
      return std::nullopt;
  }
  if (symtab.machine == I386)
    var.consume_front("_");
  std::optional<std::pair<std::string, unsigned>> ret =
      dwarf->getVariableLoc(var);
  if (!ret)
    return std::nullopt;
  return std::make_pair(saver().save(ret->first), ret->second);
}

// Used only for DWARF debug info, which is not common (except in MinGW
// environments).
std::optional<DILineInfo> ObjFile::getDILineInfo(uint32_t offset,
                                                 uint32_t sectionIndex) {
  if (!dwarf) {
    dwarf = make<DWARFCache>(DWARFContext::create(*getCOFFObj()));
    if (!dwarf)
      return std::nullopt;
  }

  return dwarf->getDILineInfo(offset, sectionIndex);
}

void ObjFile::enqueuePdbFile(StringRef path, ObjFile *fromFile) {
  auto p = findPdbPath(path.str(), fromFile, symtab.ctx.config.outputFile);
  if (!p)
    return;
  auto it = symtab.ctx.pdbInputFileInstances.emplace(*p, nullptr);
  if (!it.second)
    return; // already scheduled for load
  symtab.ctx.driver.enqueuePDB(*p);
}

ImportFile::ImportFile(COFFLinkerContext &ctx, MemoryBufferRef m)
    : InputFile(ctx.getSymtab(getMachineType(m)), ImportKind, m),
      live(!ctx.config.doGC) {}

MachineTypes ImportFile::getMachineType(MemoryBufferRef m) {
  uint16_t machine =
      reinterpret_cast<const coff_import_header *>(m.getBufferStart())->Machine;
  return MachineTypes(machine);
}

bool ImportFile::isSameImport(const ImportFile *other) const {
  if (!externalName.empty())
    return other->externalName == externalName;
  return hdr->OrdinalHint == other->hdr->OrdinalHint;
}

ImportThunkChunk *ImportFile::makeImportThunk() {
  switch (hdr->Machine) {
  case AMD64:
    return make<ImportThunkChunkX64>(symtab.ctx, impSym);
  case I386:
    return make<ImportThunkChunkX86>(symtab.ctx, impSym);
  case ARM64:
    return make<ImportThunkChunkARM64>(symtab.ctx, impSym, ARM64);
  case ARMNT:
    return make<ImportThunkChunkARM>(symtab.ctx, impSym);
  }
  llvm_unreachable("unknown machine type");
}

// For a reference from an object for Windows Itanium or NT-POSIX, a
// definition in the link beats an import library's pointer, as an
// executable's definition beats a shared object's on ELF. A vague entity
// keeps the import where the image takes the DLL's offer: the DLL's instance
// then stands for the image's, whatever refers to the pointer. No other
// image's definition is that of a hidden or protected symbol, so a weak
// reference to one that the link does not define keeps its default.
bool ImportFile::yieldsToDefinition(Symbol *imp) const {
  if (!imp || symtab.isEC())
    return false;
  Symbol *x = symtab.find(imp->getName().substr(strlen("__imp_")));
  bool isProtected =
      x && x->visibility >= LinkFactProtected && !isa<DefinedImportData>(x);
  DefinedRegular *d = x ? symtab.getEntityDefinition(x) : nullptr;
  if (auto it = symtab.vagueEntities.find(x);
      d && it != symtab.vagueEntities.end())
    return !symtab.isPreemptible(x, it->second.function) ||
           symtab.replaces(it->second) ||
           it->second.function != (hdr->getType() == llvm::COFF::IMPORT_CODE) ||
           !symtab.honoursOffer(this);
  return imp->forkReferenced && (isProtected || d);
}

void ImportFile::parse() {
  const auto *hdr =
      reinterpret_cast<const coff_import_header *>(mb.getBufferStart());

  // Check if the total size is valid.
  if (mb.getBufferSize() < sizeof(*hdr) ||
      mb.getBufferSize() != sizeof(*hdr) + hdr->SizeOfData)
    Fatal(symtab.ctx) << "broken import library";

  // Read names and create an __imp_ symbol.
  StringRef buf = mb.getBuffer().substr(sizeof(*hdr));
  auto split = buf.split('\0');
  buf = split.second;
  StringRef name;
  if (isArm64EC(hdr->Machine)) {
    if (std::optional<std::string> demangledName =
            getArm64ECDemangledFunctionName(split.first))
      name = saver().save(*demangledName);
  }
  if (name.empty())
    name = saver().save(split.first);
  StringRef impName = saver().save("__imp_" + name);
  dllName = buf.split('\0').first;
  StringRef extName;
  switch (hdr->getNameType()) {
  case IMPORT_ORDINAL:
    extName = "";
    break;
  case IMPORT_NAME:
    extName = name;
    break;
  case IMPORT_NAME_NOPREFIX:
    extName = ltrim1(name, "?@_");
    break;
  case IMPORT_NAME_UNDECORATE:
    extName = ltrim1(name, "?@_");
    extName = extName.substr(0, extName.find('@'));
    break;
  case IMPORT_NAME_EXPORTAS:
    extName = buf.substr(dllName.size() + 1).split('\0').first;
    break;
  }

  this->hdr = hdr;
  externalName = extName;

  // The reference that loaded the import binds to the definition instead.
  if (Symbol *imp = symtab.find(impName); yieldsToDefinition(imp)) {
    if (imp->isLazy())
      replaceSymbol<Undefined>(imp, impName);
    live = false;
    return;
  }

  // An offered function that the link defines keeps its definition for its
  // calls, so the import is of its address alone and has no thunk.
  bool isCode = hdr->getType() == llvm::COFF::IMPORT_CODE &&
                !symtab.vagueEntities.count(symtab.find(name));

  if (!symtab.isEC()) {
    impSym = symtab.addImportData(impName, this, location);
  } else {
    // In addition to the regular IAT, ARM64EC also contains an auxiliary IAT,
    // which holds addresses that are guaranteed to be callable directly from
    // ARM64 code. Function symbol naming is swapped: __imp_ symbols refer to
    // the auxiliary IAT, while __imp_aux_ symbols refer to the regular IAT. For
    // data imports, the naming is reversed.
    StringRef auxImpName = saver().save("__imp_aux_" + name);
    if (isCode) {
      impSym = symtab.addImportData(auxImpName, this, location);
      impECSym = symtab.addImportData(impName, this, auxLocation);
    } else {
      impSym = symtab.addImportData(impName, this, location);
      impECSym = symtab.addImportData(auxImpName, this, auxLocation);
    }
    if (!impECSym)
      return;

    StringRef auxImpCopyName = saver().save("__auximpcopy_" + name);
    auxImpCopySym = symtab.addImportData(auxImpCopyName, this, auxCopyLocation);
    if (!auxImpCopySym)
      return;
  }
  // If this was a duplicate, we logged an error but may continue;
  // in this case, impSym is nullptr.
  if (!impSym)
    return;

  // An import that only weak references name, one of them an object's for
  // Windows Itanium or NT-POSIX, is bound by the image's fill, which leaves
  // it null where the DLL lacks it, unless a strong reference names it too.
  if (auto *x = dyn_cast_or_null<Undefined>(symtab.find(name));
      x && x->weakAlias && x->forkReferenced && !impSym->strongImport &&
      (x->weakKind == Symbol::NoLibrary || x->weakKind == Symbol::Library))
    binding = Weak;

  if (hdr->getType() == llvm::COFF::IMPORT_CONST)
    static_cast<void>(symtab.addImportData(name, this, location));

  // If type is function, we need to create a thunk which jump to an
  // address pointed by the __imp_ symbol. (This allows you to call
  // DLL functions just like regular non-DLL functions.)
  if (isCode) {
    if (!symtab.isEC()) {
      thunkSym = symtab.addImportThunk(name, impSym, makeImportThunk());
    } else {
      thunkSym = symtab.addImportThunk(
          name, impSym, make<ImportThunkChunkX64>(symtab.ctx, impSym));

      if (std::optional<std::string> mangledName =
              getArm64ECMangledFunctionName(name)) {
        StringRef auxThunkName = saver().save(*mangledName);
        auxThunkSym = symtab.addImportThunk(
            auxThunkName, impECSym,
            make<ImportThunkChunkARM64>(symtab.ctx, impECSym, ARM64EC));
      }

      StringRef impChkName = saver().save("__impchk_" + name);
      impchkThunk = make<ImportThunkChunkARM64EC>(this);
      impchkThunk->sym = symtab.addImportThunk(impChkName, impSym, impchkThunk);
      symtab.ctx.driver.pullArm64ECIcallHelper();
    }
  }
}

BitcodeFile::BitcodeFile(SymbolTable &symtab, MemoryBufferRef mb,
                         std::unique_ptr<lto::InputFile> &o, bool lazy)
    : InputFile(symtab, BitcodeKind, mb, lazy) {
  obj.swap(o);
}

BitcodeFile *BitcodeFile::create(COFFLinkerContext &ctx, MemoryBufferRef mb,
                                 StringRef archiveName,
                                 uint64_t offsetInArchive, bool lazy) {
  std::string path = mb.getBufferIdentifier().str();
  if (ctx.config.thinLTOIndexOnly)
    path = replaceThinLTOSuffix(mb.getBufferIdentifier(),
                                ctx.config.thinLTOObjectSuffixReplace.first,
                                ctx.config.thinLTOObjectSuffixReplace.second);

  // ThinLTO assumes that all MemoryBufferRefs given to it have a unique
  // name. If two archives define two members with the same name, this
  // causes a collision which result in only one of the objects being taken
  // into consideration at LTO time (which very likely causes undefined
  // symbols later in the link stage). So we append file offset to make
  // filename unique.
  MemoryBufferRef mbref(mb.getBuffer(),
                        saver().save(archiveName.empty()
                                         ? path
                                         : archiveName +
                                               sys::path::filename(path) +
                                               utostr(offsetInArchive)));

  std::unique_ptr<lto::InputFile> obj = check(lto::InputFile::create(mbref));
  obj->setArchivePathAndName(archiveName, mb.getBufferIdentifier());
  return make<BitcodeFile>(ctx.getSymtab(getMachineType(obj.get())), mb, obj,
                           lazy);
}

BitcodeFile::~BitcodeFile() = default;

void BitcodeFile::parse() {
  llvm::StringSaver &saver = lld::saver();
  if (Triple(obj->getTargetTriple()).isWindowsItaniumOrNTPOSIXEnvironment())
    forkObject = symtab.ctx.hasForkObject = true;

  std::vector<std::pair<Symbol *, bool>> comdat(obj->getComdatTable().size());
  for (size_t i = 0; i != obj->getComdatTable().size(); ++i)
    // FIXME: Check nodeduplicate
    comdat[i] =
        symtab.addComdat(this, saver.save(obj->getComdatTable()[i].first));
  auto getFakeSC = [&](const lto::InputFile::Symbol &objSym) {
    return objSym.isExecutable() ? &symtab.ctx.ltoTextSectionChunk.chunk
                                 : &symtab.ctx.ltoDataSectionChunk.chunk;
  };
  for (const lto::InputFile::Symbol &objSym : obj->symbols()) {
    StringRef symName = saver.save(objSym.getName());
    int comdatIndex = objSym.getComdatIndex();
    Symbol *sym;
    SectionChunk *fakeSC = getFakeSC(objSym);
    if (objSym.isUndefined()) {
      sym = symtab.addUndefined(symName, this, false);
      if (objSym.isWeak())
        sym->deferUndefined = true;
      else
        symtab.addStrongReference(sym);
      // If one LTO object file references (i.e. has an undefined reference to)
      // a symbol with an __imp_ prefix, the LTO compilation itself sees it
      // as unprefixed but with a dllimport attribute instead, and doesn't
      // understand the relation to a concrete IR symbol with the __imp_ prefix.
      //
      // For such cases, mark the symbol as used in a regular object (i.e. the
      // symbol must be retained) so that the linker can associate the
      // references in the end. If the symbol is defined in an import library
      // or in a regular object file, this has no effect, but if it is defined
      // in another LTO object file, this makes sure it is kept, to fulfill
      // the reference when linking the output of the LTO compilation.
      if (symName.starts_with("__imp_"))
        sym->isUsedInRegularObj = true;
    } else if (objSym.isCommon()) {
      sym = symtab.addCommon(this, symName, objSym.getCommonSize());
    } else if (objSym.isWeak() && objSym.isIndirect()) {
      // Weak external, whose alias is set once every symbol is read.
      sym = symtab.addUndefined(symName, this, true);
    } else if (comdatIndex != -1) {
      if (symName == obj->getComdatTable()[comdatIndex].first) {
        sym = comdat[comdatIndex].first;
        if (cast<DefinedRegular>(sym)->data == nullptr)
          cast<DefinedRegular>(sym)->data = &fakeSC->repl;
      } else if (comdat[comdatIndex].second) {
        sym = symtab.addRegular(this, symName, nullptr, fakeSC);
      } else {
        sym = symtab.addUndefined(symName, this, false);
      }
    } else {
      sym =
          symtab.addRegular(this, symName, nullptr, fakeSC, 0, objSym.isWeak());
    }
    symbols.push_back(sym);
    if (objSym.isUsed())
      symtab.ctx.config.gcroot.push_back(sym);
  }

  // An alias to a local symbol, which the symbol table does not list, is a
  // weak definition. Any other is a weak external, which LTO does not link
  // and which the link binds, as it does the references to it, to its target,
  // so the object that LTO generates defines the target. So is a declaration that names its
  // alternate, which may be anywhere.
  auto bind = [&](Symbol *s, StringRef target) {
    Symbol *alias = symtab.addUndefined(target, this, false);
    if (auto *u = dyn_cast<Undefined>(s))
      u->setWeakAlias(alias);
    alias->isUsedInRegularObj = true;
  };
  for (auto [objSym, sym] : llvm::zip(obj->symbols(), symbols)) {
    if (!objSym.isWeak())
      continue;
    StringRef alt = objSym.getCOFFWeakExternalFallback();
    if (objSym.isUndefined() && !alt.empty()) {
      bind(sym, alt);
    } else if (!objSym.isIndirect()) {
      continue;
    } else if (Symbol *alias = symtab.find(alt)) {
      addWeakAlias(sym, alias);
      sym->isUsedInRegularObj = alias->isUsedInRegularObj = true;
    } else if (isa<Undefined>(sym)) {
      symtab.addRegular(this, sym->getName(), nullptr, getFakeSC(objSym), 0,
                        true);
    }
  }
  directives = saver.save(obj->getCOFFLinkerOpts());

  // A link fact names a symbol by its index among all the symbols of the
  // bitcode, of which obj->symbols() holds the ones that take part in the
  // link.
  std::vector<Symbol *> named;
  ArrayRef<int> indices = obj->getCOFFLinkFactSymbols();
  for (int i : indices)
    named.push_back(i < 0 ? nullptr : symbols[i]);
  if (Error e =
          readLinkFacts(obj->getCOFFLinkFacts(), named, [&](uint64_t i) {
            const lto::InputFile::Symbol &s = obj->symbols()[indices[i]];
            return !s.isWeak() && !s.isUndefined();
          }))
    Err(symtab.ctx) << this << ": link facts: " << std::move(e);
}

void BitcodeFile::parseLazy() {
  // The names may be in a symbol table that obj builds, which LTO frees with
  // obj, so the table keeps copies, as parse() does.
  for (const lto::InputFile::Symbol &sym : obj->symbols())
    if (!sym.isUndefined()) {
      symtab.addLazyObject(this, saver().save(sym.getName()));
      if (!lazy)
        return;
    }
}

MachineTypes BitcodeFile::getMachineType(const llvm::lto::InputFile *obj) {
  Triple t(obj->getTargetTriple());
  switch (t.getArch()) {
  case Triple::x86_64:
    return AMD64;
  case Triple::x86:
    return I386;
  case Triple::arm:
  case Triple::thumb:
    return ARMNT;
  case Triple::aarch64:
    return t.isWindowsArm64EC() ? ARM64EC : ARM64;
  default:
    return IMAGE_FILE_MACHINE_UNKNOWN;
  }
}

std::string lld::coff::replaceThinLTOSuffix(StringRef path, StringRef suffix,
                                            StringRef repl) {
  if (path.consume_back(suffix))
    return (path + repl).str();
  return std::string(path);
}

static bool isRVACode(COFFObjectFile *coffObj, uint64_t rva, InputFile *file) {
  for (size_t i = 1, e = coffObj->getNumberOfSections(); i <= e; i++) {
    const coff_section *sec = CHECK(coffObj->getSection(i), file);
    if (rva >= sec->VirtualAddress &&
        rva <= sec->VirtualAddress + sec->VirtualSize) {
      return (sec->Characteristics & COFF::IMAGE_SCN_CNT_CODE) != 0;
    }
  }
  return false;
}

void DLLFile::parse() {
  if (!coffObj->getPE32Header() && !coffObj->getPE32PlusHeader()) {
    Err(symtab.ctx) << toString(this) << " is not a PE-COFF executable";
    return;
  }

  for (const auto &exp : coffObj->export_directories()) {
    StringRef dllName, symbolName;
    uint32_t exportRVA;
    checkError(exp.getDllName(dllName));
    checkError(exp.getSymbolName(symbolName));
    checkError(exp.getExportRVA(exportRVA));

    if (symbolName.empty())
      continue;

    bool code = isRVACode(coffObj.get(), exportRVA, this);

    Symbol *s = make<Symbol>();
    s->dllName = dllName;
    s->symbolName = symbolName;
    s->importType = code ? ImportType::IMPORT_CODE : ImportType::IMPORT_DATA;
    s->nameType = ImportNameType::IMPORT_NAME;

    if (coffObj->getMachine() == I386) {
      s->symbolName = symbolName = saver().save("_" + symbolName);
      s->nameType = ImportNameType::IMPORT_NAME_NOPREFIX;
    }

    StringRef impName = saver().save("__imp_" + symbolName);
    symtab.addLazyDLLSymbol(this, s, impName);
    if (code)
      symtab.addLazyDLLSymbol(this, s, symbolName);
    if (symtab.isEC()) {
      StringRef impAuxName = saver().save("__imp_aux_" + symbolName);
      symtab.addLazyDLLSymbol(this, s, impAuxName);

      if (code) {
        std::optional<std::string> mangledName =
            getArm64ECMangledFunctionName(symbolName);
        if (mangledName)
          symtab.addLazyDLLSymbol(this, s, *mangledName);
      }
    }
  }
}

MachineTypes DLLFile::getMachineType() const {
  auto machine = static_cast<MachineTypes>(coffObj->getMachine());
  return machine == ARM64X ? ARM64 : machine;
}

ImportFile *ImportFile::create(COFFLinkerContext &ctx, MachineTypes machine,
                               StringRef name, StringRef dll, ImportType type,
                               ImportNameType nameType, StringRef exportAs,
                               uint16_t hint) {
  // +2 for NULs, and the name to import by and its NUL.
  size_t impSize = dll.size() + name.size() + 2 +
                   (exportAs.empty() ? 0 : exportAs.size() + 1);
  size_t size = sizeof(coff_import_header) + impSize;
  char *buf = bAlloc().Allocate<char>(size);
  memset(buf, 0, size);
  auto *imp = reinterpret_cast<coff_import_header *>(buf);
  imp->Sig2 = 0xFFFF;
  imp->Machine = static_cast<uint16_t>(machine);
  imp->SizeOfData = impSize;
  imp->OrdinalHint = hint;
  imp->TypeInfo = (nameType << 2) | type;

  // Write symbol name and DLL name.
  char *p = buf + sizeof(*imp);
  memcpy(p, name.data(), name.size());
  p += name.size() + 1;
  memcpy(p, dll.data(), dll.size());
  if (!exportAs.empty())
    memcpy(p + dll.size() + 1, exportAs.data(), exportAs.size());
  return make<ImportFile>(ctx, MemoryBufferRef(StringRef(buf, size), dll));
}

void ImportFile::makeUnnamedThunk() {
  thunkSym = make<DefinedImportThunk>(
      symtab.ctx, impSym->getName().substr(strlen("__imp_")), impSym,
      makeImportThunk());
}

void DLLFile::makeImport(DLLFile::Symbol *s) {
  if (seen.insert(s->symbolName).second)
    symtab.ctx.driver.addFile(
        ImportFile::create(symtab.ctx, getMachineType(), s->symbolName,
                           s->dllName, s->importType, s->nameType));
}
