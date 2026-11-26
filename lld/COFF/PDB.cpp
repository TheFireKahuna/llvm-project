//===- PDB.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PDB.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Config.h"
#include "DebugTypes.h"
#include "Driver.h"
#include "Relocations.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "TypeMerger.h"
#include "Writer.h"
#include "lld/Common/Timer.h"
#include "llvm/DebugInfo/CodeView/DebugFrameDataSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugInlineeLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugSubsectionRecord.h"
#include "llvm/DebugInfo/CodeView/RecordName.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecordHelpers.h"
#include "llvm/DebugInfo/CodeView/SymbolSerializer.h"
#include "llvm/DebugInfo/CodeView/TypeIndexDiscovery.h"
#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/MSF/MSFError.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptorBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/GSIStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTableBuilder.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/CRC.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/TimeProfiler.h"
#include <memory>
#include <optional>

using namespace llvm;
using namespace llvm::codeview;
using namespace lld;
using namespace lld::coff;

using llvm::object::coff_section;
using namespace llvm::support::endian;
using llvm::pdb::StringTableFixup;

namespace {
class DebugSHandler;

// A point of code that the link resized, from which CodeView measures
// distances: its chunk, with the relocations the scan kept for it, its offset
// there in the object and in the image, and the offset in its debug section
// of the relocation that names it.
struct CodeBase {
  const SectionChunk *chunk = nullptr;
  const ScannedSection *scanned = nullptr;
  uint32_t offset = 0;
  uint32_t outputOffset = 0;
  uint32_t reloc = 0;

  explicit operator bool() const { return chunk; }
  // The distance in the image to the point that the object has d bytes on,
  // or with end to where the instruction that ends there ends.
  uint32_t map(uint32_t d, bool end = false) const {
    if (!chunk)
      return d;
    return getOutputOffset(*chunk, *scanned, offset + d, end) - outputOffset;
  }
};

// What the copy of the symbol records of one subsection keeps to remap their
// distances across resized code.
struct CodeRemap {
  // The start of the procedure whose records are being copied, then that of
  // each S_SEPCODE that follows its end: what an inline site's annotations
  // count code offsets from.
  SmallVector<CodeBase, 2> code;
  // Whether this pass reports what it cannot remap.
  bool warn;
  BumpPtrAllocator alloc;
  // Where the subsection's records end in their section.
  uint32_t end = 0;

  bool resized() const {
    return any_of(code, [](const CodeBase &b) { return bool(b); });
  }
};

class PDBLinker {
  friend DebugSHandler;

public:
  PDBLinker(COFFLinkerContext &ctx)
      : builder(bAlloc()), tMerger(ctx, bAlloc()), ctx(ctx) {
    for (ObjFile *file : ctx.objFileInstances)
      for (auto &[c, s] : file->relocations)
        resizedCode |= s.aux && s.aux->resized;
    // This isn't strictly necessary, but link.exe usually puts an empty string
    // as the first "valid" string in the string table, so we do the same in
    // order to maintain as much byte-for-byte compatibility as possible.
    pdbStrTab.insert("");
  }

  /// Emit the basic PDB structure: initial streams, headers, etc.
  void initialize(llvm::codeview::DebugInfo *buildId);

  /// Add natvis files specified on the command line.
  void addNatvisFiles();

  /// Add named streams specified on the command line.
  void addNamedStreams();

  /// Link CodeView from each object file in the symbol table into the PDB.
  void addObjectsToPDB();

  /// Add every live, defined public symbol to the PDB.
  void addPublicsToPDB();

  /// Link info for each import file in the symbol table into the PDB.
  void addImportFilesToPDB();

  void createModuleDBI(ObjFile *file);

  /// Link CodeView from a single object file into the target (output) PDB.
  /// When a precompiled headers object is linked, its TPI map might be provided
  /// externally.
  void addDebug(TpiSource *source);

  void addDebugSymbols(TpiSource *source);

  // Analyze the symbol records to separate module symbols from global symbols,
  // find string references, and calculate how large the symbol stream will be
  // in the PDB.
  void analyzeSymbolSubsection(SectionChunk *debugChunk,
                               uint32_t &moduleSymOffset,
                               uint32_t &nextRelocIndex,
                               std::vector<StringTableFixup> &stringTableFixups,
                               BinaryStreamRef symData);

  // Write all module symbols from all live debug symbol subsections of the
  // given object file into the given stream writer.
  Error writeAllModuleSymbolRecords(ObjFile *file, BinaryStreamWriter &writer);

  // Callback to copy and relocate debug symbols during PDB file writing.
  static Error commitSymbolsForObject(void *ctx, void *obj,
                                      BinaryStreamWriter &writer);

  // Copy the symbol record, relocate it, and fix the alignment if necessary.
  // Rewrite type indices in the record. Replace unrecognized symbol records
  // with S_SKIP records.
  // With remap, the record's distances across resized code are those of the
  // image, and the record may have another length or be several.
  void writeSymbolRecord(SectionChunk *debugChunk,
                         ArrayRef<uint8_t> sectionContents, CVSymbol sym,
                         size_t alignedSize, uint32_t &nextRelocIndex,
                         std::vector<uint8_t> &storage, CodeRemap *remap);

  void remapSymbolRecord(SectionChunk *debugChunk, CVSymbol sym,
                         uint32_t relocIndex, std::vector<uint8_t> &storage,
                         CodeRemap &remap);

  /// Add the section map, section contributions and fixups to the PDB.
  void addSections(ArrayRef<uint8_t> sectionTable, ArrayRef<uint8_t> fixups);

  /// Write the PDB to disk and store the Guid generated for it in *Guid.
  void commit(codeview::GUID *guid);

  // Collect some statistics regarding the final PDB
  void collectStats();

private:
  void pdbMakeAbsolute(SmallVectorImpl<char> &fileName);
  void translateIdSymbols(MutableArrayRef<uint8_t> &recordData,
                          TpiSource *source);
  void addCommonLinkerModuleSymbols(StringRef path,
                                    pdb::DbiModuleDescriptorBuilder &mod);

  pdb::PDBFileBuilder builder;

  TypeMerger tMerger;

  COFFLinkerContext &ctx;

  // Whether the link changed the size of any code, whose CodeView is then
  // remapped.
  bool resizedCode = false;

  /// PDBs use a single global string table for filenames in the file checksum
  /// table.
  DebugStringTableSubsection pdbStrTab;

  llvm::SmallString<128> nativePath;
};

/// Represents an unrelocated DEBUG_S_FRAMEDATA subsection.
struct UnrelocatedFpoData {
  SectionChunk *debugChunk = nullptr;
  ArrayRef<uint8_t> subsecData;
  uint32_t relocIndex = 0;
};

/// The size of the magic bytes at the beginning of a symbol section or stream.
enum : uint32_t { kSymbolStreamMagicSize = 4 };

class DebugSHandler {
  COFFLinkerContext &ctx;
  PDBLinker &linker;

  /// The object file whose .debug$S sections we're processing.
  ObjFile &file;

  /// The DEBUG_S_STRINGTABLE subsection.  These strings are referred to by
  /// index from other records in the .debug$S section.  All of these strings
  /// need to be added to the global PDB string table, and all references to
  /// these strings need to have their indices re-written to refer to the
  /// global PDB string table.
  DebugStringTableSubsectionRef cvStrTab;

  /// The DEBUG_S_FILECHKSMS subsection.  As above, these are referred to
  /// by other records in the .debug$S section and need to be merged into the
  /// PDB.
  DebugChecksumsSubsectionRef checksums;

  /// The DEBUG_S_FRAMEDATA subsection(s).  There can be more than one of
  /// these and they need not appear in any specific order.  However, they
  /// contain string table references which need to be re-written, so we
  /// collect them all here and re-write them after all subsections have been
  /// discovered and processed.
  std::vector<UnrelocatedFpoData> frameDataSubsecs;

  /// List of string table references in symbol records. Later they will be
  /// applied to the symbols during PDB writing.
  std::vector<StringTableFixup> stringTableFixups;

  /// Sum of the size of all module symbol records across all .debug$S sections.
  /// Includes record realignment and the size of the symbol stream magic
  /// prefix.
  uint32_t moduleStreamSize = kSymbolStreamMagicSize;

  /// Next relocation index in the current .debug$S section. Resets every
  /// handleDebugS call.
  uint32_t nextRelocIndex = 0;

  /// The file checksum table of the PDB's module, whose layout is that of the
  /// object's.
  std::shared_ptr<DebugChecksumsSubsection> newChecksums =
      std::make_shared<DebugChecksumsSubsection>(linker.pdbStrTab);

  void advanceRelocIndex(SectionChunk *debugChunk, ArrayRef<uint8_t> subsec);

  void addUnrelocatedSubsection(SectionChunk *debugChunk,
                                const DebugSubsectionRecord &ss);

  bool addRemappedLines(SectionChunk *debugChunk, ArrayRef<uint8_t> subsec,
                        CodeBase base);

  void addFrameDataSubsection(SectionChunk *debugChunk,
                              const DebugSubsectionRecord &ss);

public:
  DebugSHandler(COFFLinkerContext &ctx, PDBLinker &linker, ObjFile &file)
      : ctx(ctx), linker(linker), file(file) {}

  void handleDebugS(SectionChunk *debugChunk);

  void finish();
};
}

// Visual Studio's debugger requires absolute paths in various places in the
// PDB to work without additional configuration:
// https://docs.microsoft.com/en-us/visualstudio/debugger/debug-source-files-common-properties-solution-property-pages-dialog-box
void PDBLinker::pdbMakeAbsolute(SmallVectorImpl<char> &fileName) {
  // The default behavior is to produce paths that are valid within the context
  // of the machine that you perform the link on.  If the linker is running on
  // a POSIX system, we will output absolute POSIX paths.  If the linker is
  // running on a Windows system, we will output absolute Windows paths.  If the
  // user desires any other kind of behavior, they should explicitly pass
  // /pdbsourcepath, in which case we will treat the exact string the user
  // passed in as the gospel and not normalize, canonicalize it.
  if (sys::path::is_absolute(fileName, sys::path::Style::windows) ||
      sys::path::is_absolute(fileName, sys::path::Style::posix))
    return;

  // It's not absolute in any path syntax.  Relative paths necessarily refer to
  // the local file system, so we can make it native without ending up with a
  // nonsensical path.
  if (ctx.config.pdbSourcePath.empty()) {
    sys::path::native(fileName);
    sys::fs::make_absolute(fileName);
    sys::path::remove_dots(fileName, true);
    return;
  }

  // Try to guess whether /PDBSOURCEPATH is a unix path or a windows path.
  // Since PDB's are more of a Windows thing, we make this conservative and only
  // decide that it's a unix path if we're fairly certain.  Specifically, if
  // it starts with a forward slash.
  SmallString<128> absoluteFileName = ctx.config.pdbSourcePath;
  sys::path::Style guessedStyle = absoluteFileName.starts_with("/")
                                      ? sys::path::Style::posix
                                      : sys::path::Style::windows;
  sys::path::append(absoluteFileName, guessedStyle, fileName);
  sys::path::native(absoluteFileName, guessedStyle);
  sys::path::remove_dots(absoluteFileName, true, guessedStyle);

  fileName = std::move(absoluteFileName);
}

static void addTypeInfo(pdb::TpiStreamBuilder &tpiBuilder,
                        TypeCollection &typeTable) {
  // Start the TPI or IPI stream header.
  tpiBuilder.setVersionHeader(pdb::PdbTpiV80);

  // Flatten the in memory type table and hash each type.
  typeTable.ForEachRecord([&](TypeIndex ti, const CVType &type) {
    auto hash = pdb::hashTypeRecord(type);
    if (auto e = hash.takeError())
      fatal("type hashing error");
    tpiBuilder.addTypeRecord(type.RecordData, *hash);
  });
}

static void addGHashTypeInfo(COFFLinkerContext &ctx,
                             pdb::PDBFileBuilder &builder) {
  // Start the TPI or IPI stream header.
  builder.getTpiBuilder().setVersionHeader(pdb::PdbTpiV80);
  builder.getIpiBuilder().setVersionHeader(pdb::PdbTpiV80);
  for (TpiSource *source : ctx.tpiSourceList) {
    builder.getTpiBuilder().addTypeRecords(source->mergedTpi.recs,
                                           source->mergedTpi.recSizes,
                                           source->mergedTpi.recHashes);
    builder.getIpiBuilder().addTypeRecords(source->mergedIpi.recs,
                                           source->mergedIpi.recSizes,
                                           source->mergedIpi.recHashes);
  }
}

static void
recordStringTableReferences(CVSymbol sym, uint32_t symOffset,
                            std::vector<StringTableFixup> &stringTableFixups) {
  // For now we only handle S_FILESTATIC, but we may need the same logic for
  // S_DEFRANGE and S_DEFRANGE_SUBFIELD.  However, I cannot seem to generate any
  // PDBs that contain these types of records, so because of the uncertainty
  // they are omitted here until we can prove that it's necessary.
  switch (sym.kind()) {
  case SymbolKind::S_FILESTATIC: {
    // FileStaticSym::ModFileOffset
    uint32_t ref = *reinterpret_cast<const ulittle32_t *>(&sym.data()[8]);
    stringTableFixups.push_back({ref, symOffset + 8});
    break;
  }
  case SymbolKind::S_DEFRANGE:
  case SymbolKind::S_DEFRANGE_SUBFIELD:
    log("Not fixing up string table reference in S_DEFRANGE / "
        "S_DEFRANGE_SUBFIELD record");
    break;
  default:
    break;
  }
}

static SymbolKind symbolKind(ArrayRef<uint8_t> recordData) {
  const RecordPrefix *prefix =
      reinterpret_cast<const RecordPrefix *>(recordData.data());
  return static_cast<SymbolKind>(uint16_t(prefix->RecordKind));
}

/// MSVC translates S_PROC_ID_END to S_END, and S_[LG]PROC32_ID to S_[LG]PROC32
void PDBLinker::translateIdSymbols(MutableArrayRef<uint8_t> &recordData,
                                   TpiSource *source) {
  RecordPrefix *prefix = reinterpret_cast<RecordPrefix *>(recordData.data());

  SymbolKind kind = symbolKind(recordData);

  if (kind == SymbolKind::S_PROC_ID_END) {
    prefix->RecordKind = SymbolKind::S_END;
    return;
  }

  // In an object file, GPROC32_ID has an embedded reference which refers to the
  // single object file type index namespace.  This has already been translated
  // to the PDB file's ID stream index space, but we need to convert this to a
  // symbol that refers to the type stream index space.  So we remap again from
  // ID index space to type index space.
  if (kind == SymbolKind::S_GPROC32_ID || kind == SymbolKind::S_LPROC32_ID) {
    SmallVector<TiReference, 1> refs;
    auto content = recordData.drop_front(sizeof(RecordPrefix));
    CVSymbol sym(recordData);
    discoverTypeIndicesInSymbol(sym, refs);
    assert(refs.size() == 1);
    assert(refs.front().Count == 1);

    TypeIndex *ti =
        reinterpret_cast<TypeIndex *>(content.data() + refs[0].Offset);
    // `ti` is the index of a FuncIdRecord or MemberFuncIdRecord which lives in
    // the IPI stream, whose `FunctionType` member refers to the TPI stream.
    // Note that LF_FUNC_ID and LF_MFUNC_ID have the same record layout, and
    // in both cases we just need the second type index.
    if (!ti->isSimple() && !ti->isNoneType()) {
      TypeIndex newType = TypeIndex(SimpleTypeKind::NotTranslated);
      if (ctx.config.debugGHashes) {
        auto idToType = tMerger.funcIdToType.find(*ti);
        if (idToType != tMerger.funcIdToType.end())
          newType = idToType->second;
      } else {
        if (tMerger.getIDTable().contains(*ti)) {
          CVType funcIdData = tMerger.getIDTable().getType(*ti);
          if (funcIdData.length() >= 8 && (funcIdData.kind() == LF_FUNC_ID ||
                                           funcIdData.kind() == LF_MFUNC_ID)) {
            newType = *reinterpret_cast<const TypeIndex *>(&funcIdData.data()[8]);
          }
        }
      }
      if (newType == TypeIndex(SimpleTypeKind::NotTranslated)) {
        Warn(ctx) << formatv(
            "procedure symbol record for `{0}` in {1} refers to PDB "
            "item index {2:X} which is not a valid function ID record",
            getSymbolName(CVSymbol(recordData)), source->file->getName(),
            ti->getIndex());
      }
      *ti = newType;
    }

    kind = (kind == SymbolKind::S_GPROC32_ID) ? SymbolKind::S_GPROC32
                                              : SymbolKind::S_LPROC32;
    prefix->RecordKind = uint16_t(kind);
  }
}

namespace {
struct ScopeRecord {
  ulittle32_t ptrParent;
  ulittle32_t ptrEnd;
};
} // namespace

/// Given a pointer to a symbol record that opens a scope, return a pointer to
/// the scope fields.
static ScopeRecord *getSymbolScopeFields(void *sym) {
  return reinterpret_cast<ScopeRecord *>(reinterpret_cast<char *>(sym) +
                                         sizeof(RecordPrefix));
}

// To open a scope, push the offset of the current symbol record onto the
// stack.
static void scopeStackOpen(SmallVectorImpl<uint32_t> &stack,
                           std::vector<uint8_t> &storage) {
  stack.push_back(storage.size());
}

// To close a scope, update the record that opened the scope.
static void scopeStackClose(COFFLinkerContext &ctx,
                            SmallVectorImpl<uint32_t> &stack,
                            std::vector<uint8_t> &storage,
                            uint32_t storageBaseOffset, ObjFile *file) {
  if (stack.empty()) {
    Warn(ctx) << "symbol scopes are not balanced in " << file->getName();
    return;
  }

  // Update ptrEnd of the record that opened the scope to point to the
  // current record, if we are writing into the module symbol stream.
  uint32_t offOpen = stack.pop_back_val();
  uint32_t offEnd = storageBaseOffset + storage.size();
  uint32_t offParent = stack.empty() ? 0 : (stack.back() + storageBaseOffset);
  ScopeRecord *scopeRec = getSymbolScopeFields(&(storage)[offOpen]);
  scopeRec->ptrParent = offParent;
  scopeRec->ptrEnd = offEnd;
}

static bool symbolGoesInModuleStream(const CVSymbol &sym,
                                     unsigned symbolScopeDepth) {
  switch (sym.kind()) {
  case SymbolKind::S_GDATA32:
  case SymbolKind::S_GTHREAD32:
  // We really should not be seeing S_PROCREF and S_LPROCREF in the first place
  // since they are synthesized by the linker in response to S_GPROC32 and
  // S_LPROC32, but if we do see them, don't put them in the module stream I
  // guess.
  case SymbolKind::S_PROCREF:
  case SymbolKind::S_LPROCREF:
    return false;
  // S_UDT and S_CONSTANT records go in the module stream if it is not a global record.
  case SymbolKind::S_UDT:
  case SymbolKind::S_CONSTANT:
    return symbolScopeDepth > 0;
  // S_GDATA32 does not go in the module stream, but S_LDATA32 does.
  case SymbolKind::S_LDATA32:
  case SymbolKind::S_LTHREAD32:
  default:
    return true;
  }
}

static bool symbolGoesInGlobalsStream(const CVSymbol &sym,
                                      unsigned symbolScopeDepth) {
  switch (sym.kind()) {
  case SymbolKind::S_GDATA32:
  case SymbolKind::S_GTHREAD32:
  case SymbolKind::S_GPROC32:
  case SymbolKind::S_LPROC32:
  case SymbolKind::S_GPROC32_ID:
  case SymbolKind::S_LPROC32_ID:
  // We really should not be seeing S_PROCREF and S_LPROCREF in the first place
  // since they are synthesized by the linker in response to S_GPROC32 and
  // S_LPROC32, but if we do see them, copy them straight through.
  case SymbolKind::S_PROCREF:
  case SymbolKind::S_LPROCREF:
    return true;
  // Records that go in the globals stream, unless they are function-local.
  case SymbolKind::S_UDT:
  case SymbolKind::S_LDATA32:
  case SymbolKind::S_LTHREAD32:
  case SymbolKind::S_CONSTANT:
    return symbolScopeDepth == 0;
  default:
    return false;
  }
}

static void addGlobalSymbol(pdb::GSIStreamBuilder &builder, uint16_t modIndex,
                            unsigned symOffset,
                            std::vector<uint8_t> &symStorage) {
  CVSymbol sym{ArrayRef(symStorage)};
  switch (sym.kind()) {
  case SymbolKind::S_CONSTANT:
  case SymbolKind::S_UDT:
  case SymbolKind::S_GDATA32:
  case SymbolKind::S_GTHREAD32:
  case SymbolKind::S_LTHREAD32:
  case SymbolKind::S_LDATA32:
  case SymbolKind::S_PROCREF:
  case SymbolKind::S_LPROCREF: {
    // sym is a temporary object, so we have to copy and reallocate the record
    // to stabilize it.
    uint8_t *mem = bAlloc().Allocate<uint8_t>(sym.length());
    memcpy(mem, sym.data().data(), sym.length());
    builder.addGlobalSymbol(CVSymbol(ArrayRef(mem, sym.length())));
    break;
  }
  case SymbolKind::S_GPROC32:
  case SymbolKind::S_LPROC32: {
    SymbolRecordKind k = SymbolRecordKind::ProcRefSym;
    if (sym.kind() == SymbolKind::S_LPROC32)
      k = SymbolRecordKind::LocalProcRef;
    ProcRefSym ps(k);
    ps.Module = modIndex;
    // For some reason, MSVC seems to add one to this value.
    ++ps.Module;
    ps.Name = getSymbolName(sym);
    ps.SumName = 0;
    ps.SymOffset = symOffset;
    builder.addGlobalSymbol(ps);
    break;
  }
  default:
    llvm_unreachable("Invalid symbol kind!");
  }
}

// Check if the given symbol record was padded for alignment. If so, zero out
// the padding bytes and update the record prefix with the new size.
static void fixRecordAlignment(MutableArrayRef<uint8_t> recordBytes,
                               size_t oldSize) {
  size_t alignedSize = recordBytes.size();
  if (oldSize == alignedSize)
    return;
  reinterpret_cast<RecordPrefix *>(recordBytes.data())->RecordLen =
      alignedSize - 2;
  memset(recordBytes.data() + oldSize, 0, alignedSize - oldSize);
}

// Replace any record with a skip record of the same size. This is useful when
// we have reserved size for a symbol record, but type index remapping fails.
static void replaceWithSkipRecord(MutableArrayRef<uint8_t> recordBytes) {
  memset(recordBytes.data(), 0, recordBytes.size());
  auto *prefix = reinterpret_cast<RecordPrefix *>(recordBytes.data());
  prefix->RecordKind = SymbolKind::S_SKIP;
  prefix->RecordLen = recordBytes.size() - 2;
}

// Returns the point that the first section-relative relocation of debugChunk
// in [begin, end) names, where that is in resized code. The relocations
// before relocIndex lie before begin.
static CodeBase getCodeBase(const SectionChunk &debugChunk, uint32_t relocIndex,
                            uint32_t begin, uint32_t end) {
  ArrayRef<coff_relocation> rels = debugChunk.getRelocs();
  uint16_t secRel = debugChunk.getArch() == Triple::x86_64
                        ? uint16_t(COFF::IMAGE_REL_AMD64_SECREL)
                        : uint16_t(COFF::IMAGE_REL_ARM64_SECREL);
  for (; relocIndex < rels.size(); ++relocIndex) {
    const coff_relocation &rel = rels[relocIndex];
    if (rel.VirtualAddress >= end)
      break;
    if (rel.VirtualAddress < begin || rel.Type != secRel)
      continue;
    // Debug information describes the body of a function override.
    auto *d = dyn_cast_or_null<Defined>(debugChunk.getRelocTarget(rel));
    if (FunctionOverrideThunk *t = FunctionOverrideThunk::get(d))
      d = t->fo.body;
    auto *dr = dyn_cast_or_null<DefinedRegular>(d);
    SectionChunk *c = dr && dr->data ? dr->getChunk() : nullptr;
    ScannedSection *s = c && c->resizable ? c->getScanned() : nullptr;
    if (!s || !s->aux || !s->aux->resized)
      break;
    uint32_t offset = getInputOffset(*s, dr) +
                      read32le(&debugChunk.getContents()[rel.VirtualAddress]);
    return {c, s, offset, getOutputOffset(*c, *s, offset), rel.VirtualAddress};
  }
  return {};
}

// Writes to out the annotations of an inline site, whose code offsets are
// from the first of code or the one they select, as the image has them. An
// entry whose instructions the link deleted keeps its place and covers no
// byte, as one that MC wrote for no instruction does. Returns false if the
// link cannot remap them.
static bool remapAnnotations(ArrayRef<uint8_t> annotations,
                             ArrayRef<CodeBase> code,
                             SmallVectorImpl<char> &out) {
  CodeBase base = code[0];
  uint32_t in = 0, at = 0;
  int32_t line = 0;
  bool pending = false;
  auto flushLine = [&] {
    if (pending) {
      compressAnnotation(BinaryAnnotationsOpCode::ChangeLineOffset, out);
      compressAnnotation(encodeSignedNumber(line), out);
    }
    pending = false;
  };
  // The distance in the image over the next d bytes of the object.
  auto advance = [&](uint32_t d) {
    uint32_t from = std::exchange(at, base.map(in += d));
    return at - from;
  };
  for (const DecodedAnnotation &a : make_range(
           BinaryAnnotationIterator(annotations), BinaryAnnotationIterator())) {
    switch (a.OpCode) {
    case BinaryAnnotationsOpCode::Invalid:
      break;
    // A line offset is written with the code offset that follows it.
    case BinaryAnnotationsOpCode::ChangeLineOffset:
      flushLine();
      pending = true;
      line = a.S1;
      break;
    case BinaryAnnotationsOpCode::ChangeCodeOffset:
      compressLineAndCodeDelta(pending ? line : 0, advance(a.U1), out);
      pending = false;
      break;
    case BinaryAnnotationsOpCode::ChangeCodeOffsetAndLineOffset:
      flushLine();
      compressLineAndCodeDelta(a.S1, advance(a.U1), out);
      break;
    // The offset goes on from the end of the length.
    case BinaryAnnotationsOpCode::ChangeCodeLength:
      flushLine();
      compressAnnotation(a.OpCode, out);
      compressAnnotation(advance(a.U1), out);
      break;
    // The offsets that follow are from the start of separated code.
    case BinaryAnnotationsOpCode::ChangeCodeOffsetBase:
      if (a.U1 >= code.size())
        return false;
      flushLine();
      out.append(a.Bytes.begin(), a.Bytes.end());
      base = code[a.U1];
      in = at = 0;
      break;
    // These set an offset in a way MC does not write.
    case BinaryAnnotationsOpCode::CodeOffset:
    case BinaryAnnotationsOpCode::ChangeCodeLengthAndCodeOffset:
      return false;
    default:
      flushLine();
      out.append(a.Bytes.begin(), a.Bytes.end());
      break;
    }
  }
  flushLine();
  return true;
}

// Gives the S_DEFRANGE* record rec of length bytes, whose range is at off in
// it and begins at base, the range and the gaps of the image. Returns false,
// with the record as it was, if one record cannot hold them.
static bool remapDefRange(uint8_t *rec, size_t length, size_t off,
                          CodeBase base) {
  uint8_t *range = rec + off + offsetof(LocalVariableAddrRange, Range);
  uint32_t size = base.map(read16le(range));
  if (size > MaxDefRange)
    return false;
  write16le(range, size);
  for (uint8_t *gap = range + 2; gap + 4 <= rec + length; gap += 4) {
    uint32_t begin = read16le(gap), end = begin + read16le(gap + 2);
    write16le(gap, base.map(begin));
    write16le(gap + 2, base.map(end) - base.map(begin));
  }
  return true;
}

// Replaces the S_DEFRANGE* record sym, which ends out in size bytes and whose
// range begins at base, with the records that hold its range and gaps in the
// image, packed as MC packs them.
template <class T>
static void splitDefRange(CVSymbol sym, size_t size, CodeBase base,
                          BumpPtrAllocator &alloc, std::vector<uint8_t> &out) {
  T rec(SymbolRecordKind(sym.kind()));
  if (Error e = SymbolDeserializer::deserializeAs<T>(sym, rec)) {
    consumeError(std::move(e));
    return;
  }
  out.resize(out.size() - size);
  alloc.Reset();
  // The ranges between the gaps: where each begins, and what MC packs.
  SmallVector<uint32_t, 4> starts;
  SmallVector<std::pair<unsigned, unsigned>, 4> sizes;
  uint32_t begin = 0, last = 0;
  auto add = [&](uint32_t end) {
    uint32_t b = base.map(begin), e = base.map(end);
    starts.push_back(b);
    sizes.push_back({b - last, e - b});
    last = e;
  };
  for (const LocalVariableAddrGap &g : rec.Gaps) {
    add(g.GapStartOffset);
    begin = g.GapStartOffset + g.Range;
  }
  add(rec.Range.Range);
  uint32_t start = rec.Range.OffsetStart;
  packDefRange(sizes, [&](size_t i, size_t j, unsigned bias, uint16_t size) {
    rec.Range.OffsetStart = start + starts[i] + bias;
    rec.Range.Range = size;
    rec.Gaps.clear();
    for (size_t k = i + 1; k != j; ++k)
      rec.Gaps.push_back({uint16_t(starts[k] - sizes[k].first - starts[i]),
                          uint16_t(sizes[k].first)});
    CVSymbol s =
        SymbolSerializer::writeOneSymbol(rec, alloc, CodeViewContainer::Pdb);
    out.insert(out.end(), s.data().begin(), s.data().end());
  });
}

// Gives the record that ends storage, the copy of sym, the distances that the
// image has across the code that the link resized. A record whose length that
// changes is serialised anew, and a range of a variable that the format's
// records cannot hold becomes several.
void PDBLinker::remapSymbolRecord(SectionChunk *debugChunk, CVSymbol sym,
                                  uint32_t relocIndex,
                                  std::vector<uint8_t> &storage,
                                  CodeRemap &remap) {
  size_t size = alignTo(sym.length(), alignOf(CodeViewContainer::Pdb));
  uint8_t *rec = storage.data() + storage.size() - size;
  uint32_t va = sym.data().data() - debugChunk->getContents().data();
  CodeBase base = getCodeBase(*debugChunk, relocIndex, va, va + sym.length());
  auto remap32 = [&](size_t off) {
    write32le(rec + off, base.map(read32le(rec + off)));
  };
  auto warn = [&](const Twine &what) {
    if (remap.warn)
      Warn(ctx) << debugChunk->file << ": " << what
                << " is not remapped to the code that the link resized";
  };
  auto remap16 = [&](size_t off, bool end = false) {
    uint32_t v = base.map(read16le(rec + off), end);
    if (!isUInt<16>(v))
      warn("a length of more than 16 bits");
    write16le(rec + off, v);
  };
  // The record without the padding of its copy.
  CVSymbol copy(ArrayRef(rec, sym.length()));
  SymbolKind kind = sym.kind();
  switch (kind) {
  // The code size and the offsets at which debugging starts and ends.
  case S_GPROC32:
  case S_LPROC32:
  case S_GPROC32_ID:
  case S_LPROC32_ID:
    if (base)
      for (size_t off : {16, 20, 24})
        remap32(off);
    remap.code.assign(1, base);
    // The procedure's separated code follows the end of its scope.
    for (uint32_t at = va + sym.length(), depth = 1, len; at + 4 <= remap.end;
         at += len) {
      const uint8_t *next = debugChunk->getContents().data() + at;
      len = 2 + read16le(next);
      SymbolKind k = SymbolKind(uint16_t(read16le(next + 2)));
      if (depth == 0 && k == S_SEPCODE)
        remap.code.push_back(
            getCodeBase(*debugChunk, relocIndex, at, at + len));
      else if (depth == 0)
        break;
      if (symbolOpensScope(k))
        ++depth;
      else if (symbolEndsScope(k))
        --depth;
    }
    return;
  case S_BLOCK32:
  case S_WITH32:
  case S_SEPCODE:
    if (base)
      remap32(12);
    return;
  case S_THUNK32:
    if (base)
      remap16(22);
    return;
  // The size of the call, which ends at its return address.
  case S_HEAPALLOCSITE:
    if (base)
      remap16(10, /*end=*/true);
    return;
  // These hold addresses, which their relocations give, and no distance.
  case S_LABEL32:
  case S_CALLSITEINFO:
  case S_ANNOTATION:
  case S_ARMSWITCHTABLE:
  case S_LDATA32:
  case S_GDATA32:
  case S_LTHREAD32:
  case S_GTHREAD32:
    return;
  case S_INLINESITE2:
    if (remap.resized())
      warn("an S_INLINESITE2 record");
    return;
  // The annotations go where they were if they are as long as they were.
  case S_INLINESITE: {
    if (!remap.resized() || sym.length() < 16)
      return;
    SmallVector<char, 64> out;
    if (!remapAnnotations(ArrayRef(rec + 16, sym.length() - 16), remap.code,
                          out) ||
        out.size() > MaxRecordLength - 16) {
      warn("the line table of an inline site");
      out.clear();
    }
    if (alignTo(16 + out.size(), 4) == size) {
      memcpy(rec + 16, out.data(), out.size());
      memset(rec + 16 + out.size(), 0, size - 16 - out.size());
      return;
    }
    InlineSiteSym site(SymbolRecordKind::InlineSiteSym);
    cantFail(SymbolDeserializer::deserializeAs(copy, site));
    site.AnnotationData.assign(out.begin(), out.end());
    storage.resize(storage.size() - size);
    remap.alloc.Reset();
    CVSymbol s = SymbolSerializer::writeOneSymbol(site, remap.alloc,
                                                  CodeViewContainer::Pdb);
    storage.insert(storage.end(), s.data().begin(), s.data().end());
    return;
  }
#define DEFRANGE(Kind, Type)                                                   \
  case Kind:                                                                   \
    if (base && !remapDefRange(rec, sym.length(), base.reloc - va, base))      \
      splitDefRange<Type>(copy, size, base, remap.alloc, storage);             \
    return;
    DEFRANGE(S_DEFRANGE, DefRangeSym)
    DEFRANGE(S_DEFRANGE_SUBFIELD, DefRangeSubfieldSym)
    DEFRANGE(S_DEFRANGE_REGISTER, DefRangeRegisterSym)
    DEFRANGE(S_DEFRANGE_FRAMEPOINTER_REL, DefRangeFramePointerRelSym)
    DEFRANGE(S_DEFRANGE_SUBFIELD_REGISTER, DefRangeSubfieldRegisterSym)
    DEFRANGE(S_DEFRANGE_REGISTER_REL, DefRangeRegisterRelSym)
    DEFRANGE(S_DEFRANGE_REGISTER_REL_INDIR, DefRangeRegisterRelIndirSym)
#undef DEFRANGE
  // Another procedure's inline sites are not this one's.
  default:
    if (symbolOpensScope(kind))
      remap.code.clear();
    if (base)
      warn("a symbol record of kind 0x" + utohexstr(kind));
    return;
  }
}

// Copy the symbol record, relocate it, and fix the alignment if necessary.
// Rewrite type indices in the record. Replace unrecognized symbol records with
// S_SKIP records.
void PDBLinker::writeSymbolRecord(SectionChunk *debugChunk,
                                  ArrayRef<uint8_t> sectionContents,
                                  CVSymbol sym, size_t alignedSize,
                                  uint32_t &nextRelocIndex,
                                  std::vector<uint8_t> &storage,
                                  CodeRemap *remap) {
  uint32_t relocIndex = nextRelocIndex;
  // Allocate space for the new record at the end of the storage.
  storage.resize(storage.size() + alignedSize);
  auto recordBytes = MutableArrayRef<uint8_t>(storage).take_back(alignedSize);

  // Copy the symbol record and relocate it.
  debugChunk->writeAndRelocateSubsection(sectionContents, sym.data(),
                                         nextRelocIndex, recordBytes.data());
  fixRecordAlignment(recordBytes, sym.length());

  // Re-map all the type index references.
  TpiSource *source = debugChunk->file->debugTypesObj;
  if (!source->remapTypesInSymbolRecord(recordBytes)) {
    Log(ctx) << "ignoring unknown symbol record with kind 0x"
             << utohexstr(sym.kind());
    replaceWithSkipRecord(recordBytes);
  }

  // An object file may have S_xxx_ID symbols, but these get converted to
  // "real" symbols in a PDB.
  translateIdSymbols(recordBytes, source);

  if (remap)
    remapSymbolRecord(debugChunk, sym, relocIndex, storage, *remap);
}

void PDBLinker::analyzeSymbolSubsection(
    SectionChunk *debugChunk, uint32_t &moduleSymOffset,
    uint32_t &nextRelocIndex, std::vector<StringTableFixup> &stringTableFixups,
    BinaryStreamRef symData) {
  ObjFile *file = debugChunk->file;
  uint32_t moduleSymStart = moduleSymOffset;

  uint32_t scopeLevel = 0;
  std::vector<uint8_t> storage;
  ArrayRef<uint8_t> sectionContents = debugChunk->getContents();
  CodeRemap remap{{}, /*warn=*/true, {}};
  ArrayRef<coff_relocation> rels = debugChunk->getRelocs();

  ArrayRef<uint8_t> symsBuffer;
  cantFail(symData.readBytes(0, symData.getLength(), symsBuffer));
  remap.end = symsBuffer.end() - sectionContents.begin();

  if (symsBuffer.empty())
    Warn(ctx) << "empty symbols subsection in " << file->getName();

  Error ec = forEachCodeViewRecord<CVSymbol>(
      symsBuffer, [&](CVSymbol sym) -> llvm::Error {
        // Track the current scope.
        if (symbolOpensScope(sym.kind()))
          ++scopeLevel;
        else if (symbolEndsScope(sym.kind()))
          --scopeLevel;

        uint32_t alignedSize =
            alignTo(sym.length(), alignOf(CodeViewContainer::Pdb));

        // Copy global records. Some global records (mainly procedures)
        // reference the current offset into the module stream.
        if (symbolGoesInGlobalsStream(sym, scopeLevel)) {
          storage.clear();
          writeSymbolRecord(debugChunk, sectionContents, sym, alignedSize,
                            nextRelocIndex, storage,
                            resizedCode ? &remap : nullptr);
          addGlobalSymbol(builder.getGsiBuilder(),
                          file->moduleDBI->getModuleIndex(), moduleSymOffset,
                          storage);

          if (ctx.pdbStats.has_value())
            ++ctx.pdbStats->globalSymbols;
        } else if (resizedCode) {
          // Where the link resized code, a record that may hold a distance
          // across it may have another length in the PDB: one with a
          // relocation, and one that opens a scope, as an inline site does,
          // whose distances are from its procedure's start. The length does
          // not depend on what its relocations and type indices become.
          uint32_t va = sym.data().data() - sectionContents.data();
          while (nextRelocIndex < rels.size() &&
                 rels[nextRelocIndex].VirtualAddress < va)
            ++nextRelocIndex;
          if (symbolOpensScope(sym.kind()) ||
              (nextRelocIndex < rels.size() &&
               rels[nextRelocIndex].VirtualAddress < va + sym.length())) {
            storage.assign(sym.data().begin(), sym.data().end());
            storage.resize(alignedSize);
            remapSymbolRecord(debugChunk, sym, nextRelocIndex, storage, remap);
            alignedSize = storage.size();
          }
        }

        // Update the module stream offset and record any string table index
        // references. There are very few of these and they will be rewritten
        // later during PDB writing.
        if (symbolGoesInModuleStream(sym, scopeLevel)) {
          recordStringTableReferences(sym, moduleSymOffset, stringTableFixups);
          moduleSymOffset += alignedSize;

          if (ctx.pdbStats.has_value())
            ++ctx.pdbStats->moduleSymbols;
        }

        return Error::success();
      });

  // If we encountered corrupt records, ignore the whole subsection. If we wrote
  // any partial records, undo that. For globals, we just keep what we have and
  // continue.
  if (ec) {
    Warn(ctx) << "corrupt symbol records in " << file->getName();
    moduleSymOffset = moduleSymStart;
    consumeError(std::move(ec));
  }
}

Error PDBLinker::writeAllModuleSymbolRecords(ObjFile *file,
                                             BinaryStreamWriter &writer) {
  ExitOnError exitOnErr;
  std::vector<uint8_t> storage;
  SmallVector<uint32_t, 4> scopes;

  // Visit all live .debug$S sections a second time, and write them to the PDB.
  for (SectionChunk *debugChunk : file->getDebugChunks()) {
    if (!debugChunk->live || debugChunk->getSize() == 0 ||
        debugChunk->getSectionName() != ".debug$S")
      continue;

    ArrayRef<uint8_t> sectionContents = debugChunk->getContents();
    auto contents =
        SectionChunk::consumeDebugMagic(sectionContents, ".debug$S");
    DebugSubsectionArray subsections;
    BinaryStreamReader reader(contents, llvm::endianness::little);
    exitOnErr(reader.readArray(subsections, contents.size()));

    uint32_t nextRelocIndex = 0;
    for (const DebugSubsectionRecord &ss : subsections) {
      if (ss.kind() != DebugSubsectionKind::Symbols)
        continue;

      uint32_t moduleSymStart = writer.getOffset();
      scopes.clear();
      storage.clear();
      CodeRemap remap{{}, /*warn=*/false, {}};
      ArrayRef<uint8_t> symsBuffer;
      BinaryStreamRef sr = ss.getRecordData();
      cantFail(sr.readBytes(0, sr.getLength(), symsBuffer));
      remap.end = symsBuffer.end() - sectionContents.begin();
      auto ec = forEachCodeViewRecord<CVSymbol>(
          symsBuffer, [&](CVSymbol sym) -> llvm::Error {
            // Track the current scope. Only update records in the postmerge
            // pass.
            if (symbolOpensScope(sym.kind()))
              scopeStackOpen(scopes, storage);
            else if (symbolEndsScope(sym.kind()))
              scopeStackClose(ctx, scopes, storage, moduleSymStart, file);

            // Copy, relocate, and rewrite each module symbol.
            if (symbolGoesInModuleStream(sym, scopes.size())) {
              uint32_t alignedSize =
                  alignTo(sym.length(), alignOf(CodeViewContainer::Pdb));
              writeSymbolRecord(debugChunk, sectionContents, sym, alignedSize,
                                nextRelocIndex, storage,
                                resizedCode ? &remap : nullptr);
            }
            return Error::success();
          });

      // If we encounter corrupt records in the second pass, ignore them. We
      // already warned about them in the first analysis pass.
      if (ec) {
        consumeError(std::move(ec));
        storage.clear();
      }

      // Writing bytes has a very high overhead, so write the entire subsection
      // at once.
      // TODO: Consider buffering symbols for the entire object file to reduce
      // overhead even further.
      if (Error e = writer.writeBytes(storage))
        return e;
    }
  }

  return Error::success();
}

Error PDBLinker::commitSymbolsForObject(void *ctx, void *obj,
                                        BinaryStreamWriter &writer) {
  return static_cast<PDBLinker *>(ctx)->writeAllModuleSymbolRecords(
      static_cast<ObjFile *>(obj), writer);
}

static pdb::SectionContrib createSectionContrib(COFFLinkerContext &ctx,
                                                const Chunk *c, uint32_t modi) {
  OutputSection *os = c ? ctx.getOutputSection(c) : nullptr;
  pdb::SectionContrib sc;
  memset(&sc, 0, sizeof(sc));
  sc.ISect = os ? os->sectionIndex : llvm::pdb::kInvalidStreamIndex;
  sc.Off = c && os ? c->getRVA() + c->getLeftOut() - os->getRVA() : 0;
  sc.Size = c ? c->getSize() - c->getLeftOut() : -1;
  if (auto *secChunk = dyn_cast_or_null<SectionChunk>(c)) {
    sc.Characteristics = secChunk->header->Characteristics;
    sc.Imod = secChunk->file->moduleDBI->getModuleIndex();
    ArrayRef<uint8_t> contents = secChunk->getContents();
    JamCRC crc(0);
    crc.update(contents);
    sc.DataCrc = crc.getCRC();
  } else {
    sc.Characteristics = os ? os->header.Characteristics : 0;
    sc.Imod = modi;
  }
  sc.RelocCrc = 0; // FIXME

  return sc;
}

static uint32_t
translateStringTableIndex(COFFLinkerContext &ctx, uint32_t objIndex,
                          const DebugStringTableSubsectionRef &objStrTable,
                          DebugStringTableSubsection &pdbStrTable) {
  auto expectedString = objStrTable.getString(objIndex);
  if (!expectedString) {
    Warn(ctx) << "Invalid string table reference";
    consumeError(expectedString.takeError());
    return 0;
  }

  return pdbStrTable.insert(*expectedString);
}

void DebugSHandler::handleDebugS(SectionChunk *debugChunk) {
  // Note that we are processing the *unrelocated* section contents. They will
  // be relocated later during PDB writing.
  ArrayRef<uint8_t> contents = debugChunk->getContents();
  contents = SectionChunk::consumeDebugMagic(contents, ".debug$S");
  DebugSubsectionArray subsections;
  BinaryStreamReader reader(contents, llvm::endianness::little);
  ExitOnError exitOnErr;
  exitOnErr(reader.readArray(subsections, contents.size()));
  debugChunk->sortRelocations();

  // Reset the relocation index, since this is a new section.
  nextRelocIndex = 0;

  for (const DebugSubsectionRecord &ss : subsections) {
    // Ignore subsections with the 'ignore' bit. Some versions of the Visual C++
    // runtime have subsections with this bit set.
    if (uint32_t(ss.kind()) & codeview::SubsectionIgnoreFlag)
      continue;

    switch (ss.kind()) {
    case DebugSubsectionKind::StringTable: {
      assert(!cvStrTab.valid() &&
             "Encountered multiple string table subsections!");
      exitOnErr(cvStrTab.initialize(ss.getRecordData()));
      break;
    }
    case DebugSubsectionKind::FileChecksums:
      assert(!checksums.valid() &&
             "Encountered multiple checksum subsections!");
      exitOnErr(checksums.initialize(ss.getRecordData()));
      break;
    case DebugSubsectionKind::Lines:
    case DebugSubsectionKind::InlineeLines:
      addUnrelocatedSubsection(debugChunk, ss);
      break;
    case DebugSubsectionKind::FrameData:
      addFrameDataSubsection(debugChunk, ss);
      break;
    case DebugSubsectionKind::Symbols:
      linker.analyzeSymbolSubsection(debugChunk, moduleStreamSize,
                                     nextRelocIndex, stringTableFixups,
                                     ss.getRecordData());
      break;

    case DebugSubsectionKind::CrossScopeImports:
    case DebugSubsectionKind::CrossScopeExports:
      // These appear to relate to cross-module optimization, so we might use
      // these for ThinLTO.
      break;

    case DebugSubsectionKind::ILLines:
    case DebugSubsectionKind::FuncMDTokenMap:
    case DebugSubsectionKind::TypeMDTokenMap:
    case DebugSubsectionKind::MergedAssemblyInput:
      // These appear to relate to .Net assembly info.
      break;

    case DebugSubsectionKind::CoffSymbolRVA:
      // Unclear what this is for.
      break;

    case DebugSubsectionKind::XfgHashType:
    case DebugSubsectionKind::XfgHashVirtual:
      break;

    default:
      Warn(ctx) << "ignoring unknown debug$S subsection kind 0x"
                << utohexstr(uint32_t(ss.kind())) << " in file "
                << toString(&file);
      break;
    }
  }
}

void DebugSHandler::advanceRelocIndex(SectionChunk *sc,
                                      ArrayRef<uint8_t> subsec) {
  ptrdiff_t vaBegin = subsec.data() - sc->getContents().data();
  assert(vaBegin > 0);
  auto relocs = sc->getRelocs();
  for (; nextRelocIndex < relocs.size(); ++nextRelocIndex) {
    if (relocs[nextRelocIndex].VirtualAddress >= (uint32_t)vaBegin)
      break;
  }
}

namespace {
/// Wrapper class for unrelocated line and inlinee line subsections, which
/// require only relocation and type index remapping to add to the PDB.
class UnrelocatedDebugSubsection : public DebugSubsection {
public:
  UnrelocatedDebugSubsection(DebugSubsectionKind k, SectionChunk *debugChunk,
                             ArrayRef<uint8_t> subsec, uint32_t relocIndex)
      : DebugSubsection(k), debugChunk(debugChunk), subsec(subsec),
        relocIndex(relocIndex) {}

  Error commit(BinaryStreamWriter &writer) const override;
  uint32_t calculateSerializedSize() const override { return subsec.size(); }

  SectionChunk *debugChunk;
  ArrayRef<uint8_t> subsec;
  uint32_t relocIndex;
};
} // namespace

Error UnrelocatedDebugSubsection::commit(BinaryStreamWriter &writer) const {
  std::vector<uint8_t> relocatedBytes(subsec.size());
  uint32_t tmpRelocIndex = relocIndex;
  debugChunk->writeAndRelocateSubsection(debugChunk->getContents(), subsec,
                                         tmpRelocIndex, relocatedBytes.data());

  // Remap type indices in inlinee line records in place. Skip the remapping if
  // there is no type source info.
  if (kind() == DebugSubsectionKind::InlineeLines &&
      debugChunk->file->debugTypesObj) {
    TpiSource *source = debugChunk->file->debugTypesObj;
    DebugInlineeLinesSubsectionRef inlineeLines;
    BinaryStreamReader storageReader(relocatedBytes, llvm::endianness::little);
    ExitOnError exitOnErr;
    exitOnErr(inlineeLines.initialize(storageReader));
    for (const InlineeSourceLine &line : inlineeLines) {
      TypeIndex &inlinee = *const_cast<TypeIndex *>(&line.Header->Inlinee);
      if (!source->remapTypeIndex(inlinee, TiRefKind::IndexRef)) {
        log("bad inlinee line record in " + debugChunk->file->getName() +
            " with bad inlinee index 0x" + utohexstr(inlinee.getIndex()));
      }
    }
  }

  return writer.writeBytes(relocatedBytes);
}

void DebugSHandler::addUnrelocatedSubsection(SectionChunk *debugChunk,
                                             const DebugSubsectionRecord &ss) {
  ArrayRef<uint8_t> subsec;
  BinaryStreamRef sr = ss.getRecordData();
  cantFail(sr.readBytes(0, sr.getLength(), subsec));
  advanceRelocIndex(debugChunk, subsec);
  if (ss.kind() == DebugSubsectionKind::Lines && linker.resizedCode) {
    uint32_t va = subsec.data() - debugChunk->getContents().data();
    CodeBase base = getCodeBase(*debugChunk, nextRelocIndex, va, va + 4);
    if (base && addRemappedLines(debugChunk, subsec, base))
      return;
  }
  file.moduleDBI->addDebugSubsection(
      std::make_shared<UnrelocatedDebugSubsection>(ss.kind(), debugChunk,
                                                   subsec, nextRelocIndex));
}

// Adds the line table of code that the link resized, which begins at base,
// with the offsets and the size that the code has in the image. An entry of
// which no byte is left, as the link deleted its instructions, is left out,
// so each entry that remains begins an instruction of its function. Returns
// false where the table cannot be read.
bool DebugSHandler::addRemappedLines(SectionChunk *debugChunk,
                                     ArrayRef<uint8_t> subsec, CodeBase base) {
  DebugLinesSubsectionRef ref;
  if (Error e =
          ref.initialize(BinaryStreamReader(subsec, endianness::little))) {
    consumeError(std::move(e));
    Warn(ctx) << &file
              << ": a line table is not remapped to the code that "
                 "the link resized";
    return false;
  }
  uint8_t header[sizeof(LineFragmentHeader)];
  uint32_t relocIndex = nextRelocIndex;
  debugChunk->writeAndRelocateSubsection(debugChunk->getContents(),
                                         subsec.take_front(sizeof(header)),
                                         relocIndex, header);
  auto *h = reinterpret_cast<const LineFragmentHeader *>(header);
  auto lines =
      std::make_shared<DebugLinesSubsection>(*newChecksums, linker.pdbStrTab);
  uint32_t size = base.map(h->CodeSize);
  lines->setRelocationAddress(h->RelocSegment, h->RelocOffset);
  lines->setFlags(LineFlags(uint16_t(h->Flags)));
  lines->setCodeSize(size);

  // An entry covers nothing if a later one that the object has elsewhere is
  // at its offset in the image, or if it is at its function's end there.
  SmallVector<std::pair<uint32_t, uint32_t>, 0> offsets;
  for (const LineColumnEntry &block : ref)
    for (const LineNumberEntry &l : block.LineNumbers)
      offsets.push_back({l.Offset, base.map(l.Offset)});
  for (size_t i = offsets.size(), last = 0; i-- != 0;) {
    auto &[in, out] = offsets[i];
    if (i + 1 != offsets.size() && offsets[i + 1].second == out)
      in = in == last ? in : ~0u;
    else
      last = in;
    if (out >= size && in < h->CodeSize)
      in = ~0u;
  }
  size_t i = 0;
  for (const LineColumnEntry &block : ref) {
    bool created = false;
    auto col = block.Columns.begin();
    for (const LineNumberEntry &l : block.LineNumbers) {
      if (offsets[i].first != ~0u) {
        if (!created)
          lines->createBlock(uint32_t(block.NameIndex));
        created = true;
        if (ref.hasColumnInfo())
          lines->addLineAndColumnInfo(offsets[i].second, LineInfo(l.Flags),
                                      col->StartColumn, col->EndColumn);
        else
          lines->addLineInfo(offsets[i].second, LineInfo(l.Flags));
      }
      ++i;
      if (ref.hasColumnInfo())
        ++col;
    }
  }
  file.moduleDBI->addDebugSubsection(lines);
  return true;
}

void DebugSHandler::addFrameDataSubsection(SectionChunk *debugChunk,
                                           const DebugSubsectionRecord &ss) {
  // We need to re-write string table indices here, so save off all
  // frame data subsections until we've processed the entire list of
  // subsections so that we can be sure we have the string table.
  ArrayRef<uint8_t> subsec;
  BinaryStreamRef sr = ss.getRecordData();
  cantFail(sr.readBytes(0, sr.getLength(), subsec));
  advanceRelocIndex(debugChunk, subsec);
  uint32_t va = subsec.data() - debugChunk->getContents().data();
  if (linker.resizedCode &&
      getCodeBase(*debugChunk, nextRelocIndex, va, va + 4))
    Warn(ctx) << &file
              << ": frame data is not remapped to the code that the "
                 "link resized";
  frameDataSubsecs.push_back({debugChunk, subsec, nextRelocIndex});
}

static Expected<StringRef>
getFileName(const DebugStringTableSubsectionRef &strings,
            const DebugChecksumsSubsectionRef &checksums, uint32_t fileID) {
  auto iter = checksums.getArray().at(fileID);
  if (iter == checksums.getArray().end())
    return make_error<CodeViewError>(cv_error_code::no_records);
  uint32_t offset = iter->FileNameOffset;
  return strings.getString(offset);
}

void DebugSHandler::finish() {
  pdb::DbiStreamBuilder &dbiBuilder = linker.builder.getDbiBuilder();

  // If we found any symbol records for the module symbol stream, defer them.
  if (moduleStreamSize > kSymbolStreamMagicSize)
    file.moduleDBI->addUnmergedSymbols(&file, moduleStreamSize -
                                                  kSymbolStreamMagicSize);

  // We should have seen all debug subsections across the entire object file now
  // which means that if a StringTable subsection and Checksums subsection were
  // present, now is the time to handle them.
  if (!cvStrTab.valid()) {
    if (checksums.valid())
      fatal(".debug$S sections with a checksums subsection must also contain a "
            "string table subsection");

    if (!stringTableFixups.empty())
      Warn(ctx)
          << "No StringTable subsection was encountered, but there are string "
             "table references";
    return;
  }

  ExitOnError exitOnErr;

  // Handle FPO data. Each subsection begins with a single image base
  // relocation, which is then added to the RvaStart of each frame data record
  // when it is added to the PDB. The string table indices for the FPO program
  // must also be rewritten to use the PDB string table.
  for (const UnrelocatedFpoData &subsec : frameDataSubsecs) {
    // Relocate the first four bytes of the subection and reinterpret them as a
    // 32 bit little-endian integer.
    SectionChunk *debugChunk = subsec.debugChunk;
    ArrayRef<uint8_t> subsecData = subsec.subsecData;
    uint32_t relocIndex = subsec.relocIndex;
    auto unrelocatedRvaStart = subsecData.take_front(sizeof(uint32_t));
    uint8_t relocatedRvaStart[sizeof(uint32_t)];
    debugChunk->writeAndRelocateSubsection(debugChunk->getContents(),
                                           unrelocatedRvaStart, relocIndex,
                                           &relocatedRvaStart[0]);
    // Use of memcpy here avoids violating type-based aliasing rules.
    support::ulittle32_t rvaStart;
    memcpy(&rvaStart, &relocatedRvaStart[0], sizeof(support::ulittle32_t));

    // Copy each frame data record, add in rvaStart, translate string table
    // indices, and add the record to the PDB.
    DebugFrameDataSubsectionRef fds;
    BinaryStreamReader reader(subsecData, llvm::endianness::little);
    exitOnErr(fds.initialize(reader));
    for (codeview::FrameData fd : fds) {
      fd.RvaStart += rvaStart;
      fd.FrameFunc = translateStringTableIndex(ctx, fd.FrameFunc, cvStrTab,
                                               linker.pdbStrTab);
      dbiBuilder.addNewFpoData(fd);
    }
  }

  // Translate the fixups and pass them off to the module builder so they will
  // be applied during writing.
  for (StringTableFixup &ref : stringTableFixups) {
    ref.StrTabOffset = translateStringTableIndex(ctx, ref.StrTabOffset,
                                                 cvStrTab, linker.pdbStrTab);
  }
  file.moduleDBI->setStringTableFixups(std::move(stringTableFixups));

  // Make a new file checksum table that refers to offsets in the PDB-wide
  // string table. Generally the string table subsection appears after the
  // checksum table, so we have to do this after looping over all the
  // subsections. The new checksum table must have the exact same layout and
  // size as the original. Otherwise, the file references in the line and
  // inlinee line tables will be incorrect.
  for (const FileChecksumEntry &fc : checksums) {
    SmallString<128> filename =
        exitOnErr(cvStrTab.getString(fc.FileNameOffset));
    linker.pdbMakeAbsolute(filename);
    exitOnErr(dbiBuilder.addModuleSourceFile(*file.moduleDBI, filename));
    newChecksums->addChecksum(filename, fc.Kind, fc.Checksum);
  }
  assert(checksums.getArray().getUnderlyingStream().getLength() ==
             newChecksums->calculateSerializedSize() &&
         "file checksum table must have same layout");

  file.moduleDBI->addDebugSubsection(std::move(newChecksums));
}

static void warnUnusable(InputFile *f, Error e, bool shouldWarn) {
  if (!shouldWarn) {
    consumeError(std::move(e));
    return;
  }
  auto diag = Warn(f->symtab.ctx);
  diag << "Cannot use debug info for '" << f << "' [LNK4099]";
  if (e)
    diag << "\n>>> failed to load reference " << std::move(e);
}

// Allocate memory for a .debug$S / .debug$F section and relocate it.
static ArrayRef<uint8_t> relocateDebugChunk(SectionChunk &debugChunk) {
  uint8_t *buffer = bAlloc().Allocate<uint8_t>(debugChunk.getSize());
  assert(debugChunk.getOutputSectionIdx() == 0 &&
         "debug sections should not be in output sections");
  debugChunk.writeTo(buffer);
  return ArrayRef(buffer, debugChunk.getSize());
}

void PDBLinker::addDebugSymbols(TpiSource *source) {
  // If this TpiSource doesn't have an object file, it must be from a type
  // server PDB. Type server PDBs do not contain symbols, so stop here.
  if (!source->file)
    return;

  llvm::TimeTraceScope timeScope("Merge symbols");
  ScopedTimer t(ctx.symbolMergingTimer);
  ExitOnError exitOnErr;
  pdb::DbiStreamBuilder &dbiBuilder = builder.getDbiBuilder();
  DebugSHandler dsh(ctx, *this, *source->file);
  // Now do all live .debug$S and .debug$F sections.
  for (SectionChunk *debugChunk : source->file->getDebugChunks()) {
    if (!debugChunk->live || debugChunk->getSize() == 0)
      continue;

    bool isDebugS = debugChunk->getSectionName() == ".debug$S";
    bool isDebugF = debugChunk->getSectionName() == ".debug$F";
    if (!isDebugS && !isDebugF)
      continue;

    if (isDebugS) {
      dsh.handleDebugS(debugChunk);
    } else if (isDebugF) {
      // Handle old FPO data .debug$F sections. These are relatively rare.
      ArrayRef<uint8_t> relocatedDebugContents =
          relocateDebugChunk(*debugChunk);
      FixedStreamArray<object::FpoData> fpoRecords;
      BinaryStreamReader reader(relocatedDebugContents,
                                llvm::endianness::little);
      uint32_t count = relocatedDebugContents.size() / sizeof(object::FpoData);
      exitOnErr(reader.readArray(fpoRecords, count));

      // These are already relocated and don't refer to the string table, so we
      // can just copy it.
      for (const object::FpoData &fd : fpoRecords)
        dbiBuilder.addOldFpoData(fd);
    }
  }

  // Do any post-processing now that all .debug$S sections have been processed.
  dsh.finish();
}

// Add a module descriptor for every object file. We need to put an absolute
// path to the object into the PDB. If this is a plain object, we make its
// path absolute. If it's an object in an archive, we make the archive path
// absolute.
void PDBLinker::createModuleDBI(ObjFile *file) {
  pdb::DbiStreamBuilder &dbiBuilder = builder.getDbiBuilder();
  SmallString<128> objName;
  ExitOnError exitOnErr;

  bool inArchive = !file->parentName.empty();
  objName = inArchive ? file->parentName : file->getName();
  pdbMakeAbsolute(objName);
  StringRef modName = inArchive ? file->getName() : objName.str();

  file->moduleDBI = &exitOnErr(dbiBuilder.addModuleInfo(modName));
  file->moduleDBI->setObjFileName(objName);
  file->moduleDBI->setMergeSymbolsCallback(this, &commitSymbolsForObject);

  ArrayRef<Chunk *> chunks = file->getChunks();
  uint32_t modi = file->moduleDBI->getModuleIndex();

  for (Chunk *c : chunks) {
    auto *secChunk = dyn_cast<SectionChunk>(c);
    if (!secChunk || !secChunk->live)
      continue;
    pdb::SectionContrib sc = createSectionContrib(ctx, secChunk, modi);
    file->moduleDBI->setFirstSectionContrib(sc);
    break;
  }
}

void PDBLinker::addDebug(TpiSource *source) {
  // Before we can process symbol substreams from .debug$S, we need to process
  // type information, file checksums, and the string table. Add type info to
  // the PDB first, so that we can get the map from object file type and item
  // indices to PDB type and item indices.  If we are using ghashes, types have
  // already been merged.
  if (!ctx.config.debugGHashes) {
    llvm::TimeTraceScope timeScope("Merge types (Non-GHASH)");
    ScopedTimer t(ctx.typeMergingTimer);
    if (Error e = source->mergeDebugT(&tMerger)) {
      // If type merging failed, ignore the symbols.
      warnUnusable(source->file, std::move(e),
                   ctx.config.warnDebugInfoUnusable);
      return;
    }
  }

  // If type merging failed, ignore the symbols.
  Error typeError = std::move(source->typeMergingError);
  if (typeError) {
    warnUnusable(source->file, std::move(typeError),
                 ctx.config.warnDebugInfoUnusable);
    return;
  }

  addDebugSymbols(source);
}

static pdb::BulkPublic createPublic(COFFLinkerContext &ctx, Defined *def) {
  pdb::BulkPublic pub;
  pub.Name = def->getName().data();
  pub.NameLen = def->getName().size();

  PublicSymFlags flags = PublicSymFlags::None;
  if (auto *d = dyn_cast<DefinedCOFF>(def)) {
    if (d->getCOFFSymbol().isFunctionDefinition())
      flags = PublicSymFlags::Function;
  } else if (isa<DefinedImportThunk>(def) || FunctionOverrideThunk::get(def)) {
    flags = PublicSymFlags::Function;
  }
  pub.setFlags(flags);

  OutputSection *os = ctx.getOutputSection(def->getChunk());
  assert((os || !def->getChunk()->getSize()) &&
         "all publics should be in final image");
  if (os) {
    pub.Offset = def->getRVA() - os->getRVA();
    pub.Segment = os->sectionIndex;
  }
  return pub;
}

// Add all object files to the PDB. Merge .debug$T sections into IpiData and
// TpiData.
void PDBLinker::addObjectsToPDB() {
  {
    llvm::TimeTraceScope timeScope("Add objects to PDB");
    ScopedTimer t1(ctx.addObjectsTimer);

    // Create module descriptors
    for (ObjFile *obj : ctx.objFileInstances)
      createModuleDBI(obj);

    // Reorder dependency type sources to come first.
    tMerger.sortDependencies();

    // Merge type information from input files using global type hashing.
    if (ctx.config.debugGHashes)
      tMerger.mergeTypesWithGHash();

    // Merge dependencies and then regular objects.
    {
      llvm::TimeTraceScope timeScope("Merge debug info (dependencies)");
      for (TpiSource *source : tMerger.dependencySources)
        addDebug(source);
    }
    {
      llvm::TimeTraceScope timeScope("Merge debug info (objects)");
      for (TpiSource *source : tMerger.objectSources)
        addDebug(source);
    }

    builder.getStringTableBuilder().setStrings(pdbStrTab);
  }

  // Construct TPI and IPI stream contents.
  {
    llvm::TimeTraceScope timeScope("TPI/IPI stream layout");
    ScopedTimer t2(ctx.tpiStreamLayoutTimer);

    // Collect all the merged types.
    if (ctx.config.debugGHashes) {
      addGHashTypeInfo(ctx, builder);
    } else {
      addTypeInfo(builder.getTpiBuilder(), tMerger.getTypeTable());
      addTypeInfo(builder.getIpiBuilder(), tMerger.getIDTable());
    }
  }

  if (ctx.pdbStats.has_value()) {
    for (TpiSource *source : ctx.tpiSourceList) {
      ctx.pdbStats->nbTypeRecords += source->nbTypeRecords;
      ctx.pdbStats->nbTypeRecordsBytes += source->nbTypeRecordsBytes;
    }
  }
}

void PDBLinker::addPublicsToPDB() {
  llvm::TimeTraceScope timeScope("Publics layout");
  ScopedTimer t3(ctx.publicsLayoutTimer);
  // Compute the public symbols.
  auto &gsiBuilder = builder.getGsiBuilder();
  std::vector<pdb::BulkPublic> publics;
  ctx.symtab.forEachSymbol([&publics, this](Symbol *s) {
    // Only emit external, defined, live symbols that have a chunk. Static,
    // non-external symbols do not appear in the symbol table.
    auto *def = dyn_cast<Defined>(s);
    if (def && def->isLive() && def->getChunk()) {
      // A section /coffsecttopdbstrm moves out of the image has no publics.
      if (auto *sc = dyn_cast<SectionChunk>(def->getChunk()))
        if (ctx.config.sectionStreams.contains(sc->getSectionName()))
          return;
      // Don't emit a public symbol for coverage data symbols. LLVM code
      // coverage (and PGO) create a __profd_ and __profc_ symbol for every
      // function. C++ mangled names are long, and tend to dominate symbol size.
      // Including these names triples the size of the public stream, which
      // results in bloated PDB files. These symbols generally are not helpful
      // for debugging, so suppress them.
      StringRef name = def->getName();
      if (name.data()[0] == '_' && name.data()[1] == '_') {
        // Drop the '_' prefix for x86.
        if (ctx.config.machine == I386)
          name = name.drop_front(1);
        if (name.starts_with("__profd_") || name.starts_with("__profc_") ||
            name.starts_with("__covrec_")) {
          return;
        }
      }
      publics.push_back(createPublic(ctx, def));
    }
  });

  if (ctx.pdbStats.has_value())
    ctx.pdbStats->publicSymbols = publics.size();

  if (!publics.empty())
    gsiBuilder.addPublicSymbols(std::move(publics));
}

void PDBLinker::collectStats() {
  if (!ctx.config.showSummary)
    return;

  ctx.pdbStats->nbTPIrecords = builder.getTpiBuilder().getRecordCount();
  ctx.pdbStats->nbIPIrecords = builder.getIpiBuilder().getRecordCount();
  ctx.pdbStats->strTabSize = pdbStrTab.size();

  SmallString<256> buffer;
  raw_svector_ostream stream(buffer);

  auto printLargeInputTypeRecs = [&](StringRef name,
                                     ArrayRef<uint32_t> recCounts,
                                     TypeCollection &records) {
    // Figure out which type indices were responsible for the most duplicate
    // bytes in the input files. These should be frequently emitted LF_CLASS and
    // LF_FIELDLIST records.
    struct TypeSizeInfo {
      uint32_t typeSize;
      uint32_t dupCount;
      TypeIndex typeIndex;
      uint64_t totalInputSize() const { return uint64_t(dupCount) * typeSize; }
      bool operator<(const TypeSizeInfo &rhs) const {
        if (totalInputSize() == rhs.totalInputSize())
          return typeIndex < rhs.typeIndex;
        return totalInputSize() < rhs.totalInputSize();
      }
    };
    SmallVector<TypeSizeInfo, 0> tsis;
    for (auto e : enumerate(recCounts)) {
      TypeIndex typeIndex = TypeIndex::fromArrayIndex(e.index());
      uint32_t typeSize = records.getType(typeIndex).length();
      uint32_t dupCount = e.value();
      tsis.push_back({typeSize, dupCount, typeIndex});
    }

    if (!tsis.empty()) {
      stream << "\nTop 10 types responsible for the most " << name
             << " input:\n";
      stream << "       index     total bytes   count     size\n";
      llvm::sort(tsis);
      unsigned i = 0;
      for (const auto &tsi : reverse(tsis)) {
        stream << formatv("  {0,10:X}: {1,14:N} = {2,5:N} * {3,6:N}\n",
                          tsi.typeIndex.getIndex(), tsi.totalInputSize(),
                          tsi.dupCount, tsi.typeSize);
        if (++i >= 10)
          break;
      }
      stream
          << "Run llvm-pdbutil to print details about a particular record:\n";
      stream << formatv("llvm-pdbutil dump -{0}s -{0}-index {1:X} {2}\n",
                        (name == "TPI" ? "type" : "id"),
                        tsis.back().typeIndex.getIndex(), ctx.config.pdbPath);
    }
  };

  if (!ctx.config.debugGHashes) {
    // FIXME: Reimplement for ghash.
    printLargeInputTypeRecs("TPI", tMerger.tpiCounts, tMerger.getTypeTable());
    printLargeInputTypeRecs("IPI", tMerger.ipiCounts, tMerger.getIDTable());

    ctx.pdbStats->largeInputTypeRecs = buffer.str();
  }
}

void PDBLinker::addNatvisFiles() {
  llvm::TimeTraceScope timeScope("Natvis files");
  for (StringRef file : ctx.config.natvisFiles) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> dataOrErr =
        MemoryBuffer::getFile(file);
    if (!dataOrErr) {
      Warn(ctx) << "Cannot open input file: " << file;
      continue;
    }
    std::unique_ptr<MemoryBuffer> data = std::move(*dataOrErr);

    // Can't use takeBuffer() here since addInjectedSource() takes ownership.
    if (ctx.driver.tar)
      ctx.driver.tar->append(relativeToRoot(data->getBufferIdentifier()),
                             data->getBuffer());

    builder.addInjectedSource(file, std::move(data));
  }
}

void PDBLinker::addNamedStreams() {
  llvm::TimeTraceScope timeScope("Named streams");
  ExitOnError exitOnErr;
  for (const auto &streamFile : ctx.config.namedStreams) {
    const StringRef stream = streamFile.getKey(), file = streamFile.getValue();
    ErrorOr<std::unique_ptr<MemoryBuffer>> dataOrErr =
        MemoryBuffer::getFile(file);
    if (!dataOrErr) {
      Warn(ctx) << "Cannot open input file: " << file;
      continue;
    }
    std::unique_ptr<MemoryBuffer> data = std::move(*dataOrErr);
    exitOnErr(builder.addNamedStream(stream, data->getBuffer()));
    ctx.driver.takeBuffer(std::move(data));
  }

  // The sections /coffsecttopdbstrm moves out of the image, file by file in
  // reverse, each file's in order, as link.exe writes them.
  MapVector<StringRef, std::string> sectionStreams;
  for (ObjFile *file : llvm::reverse(ctx.objFileInstances))
    for (Chunk *c : file->getChunks())
      if (auto *sc = dyn_cast<SectionChunk>(c))
        if (auto it = ctx.config.sectionStreams.find(sc->getSectionName());
            it != ctx.config.sectionStreams.end())
          sectionStreams[it->second] += toStringRef(sc->getContents());
  for (auto &[stream, data] : sectionStreams)
    exitOnErr(builder.addNamedStream(stream, data));
}

static codeview::CPUType toCodeViewMachine(COFF::MachineTypes machine) {
  switch (machine) {
  case COFF::IMAGE_FILE_MACHINE_AMD64:
    return codeview::CPUType::X64;
  case COFF::IMAGE_FILE_MACHINE_ARM:
    return codeview::CPUType::ARM7;
  case COFF::IMAGE_FILE_MACHINE_ARM64:
    return codeview::CPUType::ARM64;
  case COFF::IMAGE_FILE_MACHINE_ARM64EC:
    return codeview::CPUType::ARM64EC;
  case COFF::IMAGE_FILE_MACHINE_ARM64X:
    return codeview::CPUType::ARM64X;
  case COFF::IMAGE_FILE_MACHINE_ARMNT:
    return codeview::CPUType::ARMNT;
  case COFF::IMAGE_FILE_MACHINE_I386:
    return codeview::CPUType::Intel80386;
  default:
    llvm_unreachable("Unsupported CPU Type");
  }
}

// Mimic MSVC which surrounds arguments containing whitespace with quotes.
// Double double-quotes are handled, so that the resulting string can be
// executed again on the cmd-line.
static std::string quote(ArrayRef<StringRef> args) {
  std::string r;
  r.reserve(256);
  for (StringRef a : args) {
    if (!r.empty())
      r.push_back(' ');
    bool hasWS = a.contains(' ');
    bool hasQ = a.contains('"');
    if (hasWS || hasQ)
      r.push_back('"');
    if (hasQ) {
      SmallVector<StringRef, 4> s;
      a.split(s, '"');
      r.append(join(s, "\"\""));
    } else {
      r.append(std::string(a));
    }
    if (hasWS || hasQ)
      r.push_back('"');
  }
  return r;
}

static void fillLinkerVerRecord(Compile3Sym &cs, MachineTypes machine) {
  cs.Machine = toCodeViewMachine(machine);
  // Interestingly, if we set the string to 0.0.0.0, then when trying to view
  // local variables WinDbg emits an error that private symbols are not present.
  // By setting this to a valid MSVC linker version string, local variables are
  // displayed properly.   As such, even though it is not representative of
  // LLVM's version information, we need this for compatibility.
  cs.Flags = CompileSym3Flags::None;
  cs.VersionBackendBuild = 25019;
  cs.VersionBackendMajor = 14;
  cs.VersionBackendMinor = 10;
  cs.VersionBackendQFE = 0;

  // MSVC also sets the frontend to 0.0.0.0 since this is specifically for the
  // linker module (which is by definition a backend), so we don't need to do
  // anything here.  Also, it seems we can use "LLVM Linker" for the linker name
  // without any problems.  Only the backend version has to be hardcoded to a
  // magic number.
  cs.VersionFrontendBuild = 0;
  cs.VersionFrontendMajor = 0;
  cs.VersionFrontendMinor = 0;
  cs.VersionFrontendQFE = 0;
  cs.Version = "LLVM Linker";
  cs.setLanguage(SourceLanguage::Link);
}

void PDBLinker::addCommonLinkerModuleSymbols(
    StringRef path, pdb::DbiModuleDescriptorBuilder &mod) {
  ObjNameSym ons(SymbolRecordKind::ObjNameSym);
  EnvBlockSym ebs(SymbolRecordKind::EnvBlockSym);
  Compile3Sym cs(SymbolRecordKind::Compile3Sym);

  MachineTypes machine = ctx.config.machine;
  // MSVC uses the ARM64X machine type for ARM64EC targets in the common linker
  // module record.
  if (isArm64EC(machine))
    machine = ARM64X;
  fillLinkerVerRecord(cs, machine);

  ons.Name = "* Linker *";
  ons.Signature = 0;

  ArrayRef<StringRef> args = ArrayRef(ctx.config.argv).drop_front();
  std::string argStr = quote(args);
  ebs.Fields.push_back("cwd");
  SmallString<64> cwd;
  if (ctx.config.pdbSourcePath.empty())
    sys::fs::current_path(cwd);
  else
    cwd = ctx.config.pdbSourcePath;
  ebs.Fields.push_back(cwd);
  ebs.Fields.push_back("exe");
  SmallString<64> exe = ctx.config.argv[0];
  pdbMakeAbsolute(exe);
  ebs.Fields.push_back(exe);
  ebs.Fields.push_back("pdb");
  ebs.Fields.push_back(path);
  ebs.Fields.push_back("cmd");
  ebs.Fields.push_back(argStr);
  llvm::BumpPtrAllocator &bAlloc = lld::bAlloc();
  mod.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
      ons, bAlloc, CodeViewContainer::Pdb));
  mod.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
      cs, bAlloc, CodeViewContainer::Pdb));
  mod.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
      ebs, bAlloc, CodeViewContainer::Pdb));
}

static void addLinkerModuleCoffGroup(PartialSection *sec,
                                     pdb::DbiModuleDescriptorBuilder &mod,
                                     OutputSection &os) {
  // If there's a section, there's at least one chunk
  assert(!sec->chunks.empty());
  const Chunk *firstChunk = *sec->chunks.begin();
  const Chunk *lastChunk = *sec->chunks.rbegin();

  // Emit COFF group
  CoffGroupSym cgs(SymbolRecordKind::CoffGroupSym);
  cgs.Name = sec->name;
  cgs.Segment = os.sectionIndex;
  cgs.Offset = firstChunk->getRVA() + firstChunk->getLeftOut() - os.getRVA();
  cgs.Size =
      lastChunk->getRVA() + lastChunk->getSize() - os.getRVA() - cgs.Offset;
  cgs.Characteristics = sec->characteristics;

  // Somehow .idata sections & sections groups in the debug symbol stream have
  // the "write" flag set. However the section header for the corresponding
  // .idata section doesn't have it.
  if (cgs.Name.starts_with(".idata"))
    cgs.Characteristics |= llvm::COFF::IMAGE_SCN_MEM_WRITE;

  mod.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
      cgs, bAlloc(), CodeViewContainer::Pdb));
}

static void addLinkerModuleSectionSymbol(pdb::DbiModuleDescriptorBuilder &mod,
                                         OutputSection &os, bool isMinGW) {
  SectionSym sym(SymbolRecordKind::SectionSym);
  sym.Alignment = 12; // 2^12 = 4KB
  sym.Characteristics = os.header.Characteristics;
  sym.Length = os.getVirtualSize();
  sym.Name = os.name;
  sym.Rva = os.getRVA();
  sym.SectionNumber = os.sectionIndex;
  mod.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
      sym, bAlloc(), CodeViewContainer::Pdb));

  // Skip COFF groups in MinGW because it adds a significant footprint to the
  // PDB, due to each function being in its own section
  if (isMinGW)
    return;

  // Output COFF groups for individual chunks of this section.
  for (PartialSection *sec : os.contribSections) {
    addLinkerModuleCoffGroup(sec, mod, os);
  }
}

// Adds an S_THUNK32 for the thunk in chunk c that starts at offset in it.
static void addThunkSymbol(COFFLinkerContext &ctx,
                           pdb::DbiModuleDescriptorBuilder &mod, StringRef name,
                           Chunk *c, uint32_t offset) {
  OutputSection *os = ctx.getOutputSection(c);
  Thunk32Sym ts(SymbolRecordKind::Thunk32Sym);
  ScopeEndSym es(SymbolRecordKind::ScopeEndSym);
  ts.Name = name;
  ts.Parent = 0;
  ts.End = 0;
  ts.Next = 0;
  ts.Thunk = ThunkOrdinal::Standard;
  ts.Length = c->getSize() - offset;
  ts.Segment = os->sectionIndex;
  ts.Offset = c->getRVA() + offset - os->getRVA();

  CVSymbol newSym = codeview::SymbolSerializer::writeOneSymbol(
      ts, bAlloc(), CodeViewContainer::Pdb);
  // Write ptrEnd for the S_THUNK32.
  ScopeRecord *thunkSymScope =
      getSymbolScopeFields(const_cast<uint8_t *>(newSym.data().data()));
  mod.addSymbol(newSym);
  newSym = codeview::SymbolSerializer::writeOneSymbol(es, bAlloc(),
                                                      CodeViewContainer::Pdb);
  thunkSymScope->ptrEnd = mod.getNextSymbolOffset();
  mod.addSymbol(newSym);
}

// Add all import files as modules to the PDB.
void PDBLinker::addImportFilesToPDB() {
  if (ctx.importFileInstances.empty())
    return;

  llvm::TimeTraceScope timeScope("Import files");
  ExitOnError exitOnErr;
  std::map<std::string, llvm::pdb::DbiModuleDescriptorBuilder *> dllToModuleDbi;

  for (ImportFile *file : ctx.importFileInstances) {
    if (!file->live)
      continue;

    if (!file->thunkSym)
      continue;

    if (!file->thunkSym->isLive())
      continue;

    std::string dll = StringRef(file->dllName).lower();
    llvm::pdb::DbiModuleDescriptorBuilder *&mod = dllToModuleDbi[dll];
    if (!mod) {
      pdb::DbiStreamBuilder &dbiBuilder = builder.getDbiBuilder();
      SmallString<128> libPath = file->parentName;
      pdbMakeAbsolute(libPath);
      sys::path::native(libPath);

      // Name modules similar to MSVC's link.exe.
      // The first module is the simple dll filename
      llvm::pdb::DbiModuleDescriptorBuilder &firstMod =
          exitOnErr(dbiBuilder.addModuleInfo(file->dllName));
      firstMod.setObjFileName(libPath);
      pdb::SectionContrib sc =
          createSectionContrib(ctx, nullptr, llvm::pdb::kInvalidStreamIndex);
      firstMod.setFirstSectionContrib(sc);

      // The second module is where the import stream goes.
      mod = &exitOnErr(dbiBuilder.addModuleInfo("Import:" + file->dllName));
      mod->setObjFileName(libPath);
    }

    DefinedImportThunk *thunk = cast<DefinedImportThunk>(file->thunkSym);

    ObjNameSym ons(SymbolRecordKind::ObjNameSym);
    Compile3Sym cs(SymbolRecordKind::Compile3Sym);

    ons.Name = file->dllName;
    ons.Signature = 0;

    fillLinkerVerRecord(cs, ctx.config.machine);

    llvm::BumpPtrAllocator &bAlloc = lld::bAlloc();
    mod->addSymbol(codeview::SymbolSerializer::writeOneSymbol(
        ons, bAlloc, CodeViewContainer::Pdb));
    mod->addSymbol(codeview::SymbolSerializer::writeOneSymbol(
        cs, bAlloc, CodeViewContainer::Pdb));
    addThunkSymbol(ctx, *mod, thunk->getName(), thunk->getChunk(), 0);

    pdb::SectionContrib sc =
        createSectionContrib(ctx, thunk->getChunk(), mod->getModuleIndex());
    mod->setFirstSectionContrib(sc);
  }
}

// Creates a PDB file.
void lld::coff::createPDB(COFFLinkerContext &ctx,
                          ArrayRef<uint8_t> sectionTable,
                          ArrayRef<uint8_t> fixups,
                          llvm::codeview::DebugInfo *buildId) {
  llvm::TimeTraceScope timeScope("PDB file");
  ScopedTimer t1(ctx.totalPdbLinkTimer);
  {
    PDBLinker pdb(ctx);

    if (ctx.config.showSummary)
      ctx.pdbStats.emplace();

    pdb.initialize(buildId);
    pdb.addObjectsToPDB();
    pdb.addImportFilesToPDB();
    pdb.addSections(sectionTable, fixups);
    pdb.addNatvisFiles();
    pdb.addNamedStreams();
    pdb.addPublicsToPDB();

    {
      llvm::TimeTraceScope timeScope("Commit PDB file to disk");
      ScopedTimer t2(ctx.diskCommitTimer);
      codeview::GUID guid;
      pdb.commit(&guid);
      memcpy(&buildId->PDB70.Signature, &guid, 16);
    }

    pdb.collectStats();
    t1.stop();

    // Manually start this profile point to measure ~PDBLinker().
    if (getTimeTraceProfilerInstance() != nullptr)
      timeTraceProfilerBegin("PDBLinker destructor", StringRef(""));
  }
  // Manually end this profile point to measure ~PDBLinker().
  if (getTimeTraceProfilerInstance() != nullptr)
    timeTraceProfilerEnd();
}

void PDBLinker::initialize(llvm::codeview::DebugInfo *buildId) {
  ExitOnError exitOnErr;
  exitOnErr(builder.initialize(ctx.config.pdbPageSize));

  buildId->Signature.CVSignature = OMF::Signature::PDB70;
  // Signature is set to a hash of the PDB contents when the PDB is done.
  memset(buildId->PDB70.Signature, 0, 16);
  buildId->PDB70.Age = 1;

  // Create streams in MSF for predefined streams, namely
  // PDB, TPI, DBI and IPI.
  for (int i = 0; i < (int)pdb::kSpecialStreamCount; ++i)
    exitOnErr(builder.getMsfBuilder().addStream(0));

  // Add an Info stream.
  auto &infoBuilder = builder.getInfoBuilder();
  infoBuilder.setVersion(pdb::PdbRaw_ImplVer::PdbImplVC70);
  infoBuilder.setHashPDBContentsToGUID(true);

  // Add an empty DBI stream.
  pdb::DbiStreamBuilder &dbiBuilder = builder.getDbiBuilder();
  dbiBuilder.setAge(buildId->PDB70.Age);
  dbiBuilder.setVersionHeader(pdb::PdbDbiV70);
  dbiBuilder.setMachineType(ctx.config.machine);
  // Technically we are not link.exe 14.11, but there are known cases where
  // debugging tools on Windows expect Microsoft-specific version numbers or
  // they fail to work at all.  Since we know we produce PDBs that are
  // compatible with LINK 14.11, we set that version number here.
  dbiBuilder.setBuildNumber(14, 11);
}

void PDBLinker::addSections(ArrayRef<uint8_t> sectionTable,
                            ArrayRef<uint8_t> fixups) {
  llvm::TimeTraceScope timeScope("PDB output sections");
  ExitOnError exitOnErr;
  // It's not entirely clear what this is, but the * Linker * module uses it.
  pdb::DbiStreamBuilder &dbiBuilder = builder.getDbiBuilder();
  nativePath = ctx.config.pdbPath;
  pdbMakeAbsolute(nativePath);
  uint32_t pdbFilePathNI = dbiBuilder.addECName(nativePath);
  auto &linkerModule = exitOnErr(dbiBuilder.addModuleInfo("* Linker *"));
  linkerModule.setPdbFilePathNI(pdbFilePathNI);
  addCommonLinkerModuleSymbols(nativePath, linkerModule);

  // Add section contributions. They must be ordered by ascending RVA.
  for (OutputSection *os : ctx.outputSections) {
    addLinkerModuleSectionSymbol(linkerModule, *os, ctx.config.mingw);
    for (Chunk *c : os->chunks) {
      pdb::SectionContrib sc =
          createSectionContrib(ctx, c, linkerModule.getModuleIndex());
      builder.getDbiBuilder().addSectionContrib(sc);
    }
  }
  if (ctx.dynamicRelocs)
    for (FunctionOverride *fo : ctx.dynamicRelocs->functionOverrides)
      if (fo->stub)
        addThunkSymbol(ctx, linkerModule, fo->body->getName(), fo->stub,
                       fo->stub->getEntry());

  // The * Linker * first section contrib is only used along with /INCREMENTAL,
  // to provide trampolines thunks for incremental function patching. Set this
  // as "unused" because LLD doesn't support /INCREMENTAL link.
  pdb::SectionContrib sc =
      createSectionContrib(ctx, nullptr, llvm::pdb::kInvalidStreamIndex);
  linkerModule.setFirstSectionContrib(sc);

  // Add Section Map stream.
  ArrayRef<object::coff_section> sections = {
      (const object::coff_section *)sectionTable.data(),
      sectionTable.size() / sizeof(object::coff_section)};
  dbiBuilder.createSectionMap(sections);

  // Add COFF section header stream.
  exitOnErr(
      dbiBuilder.addDbgStream(pdb::DbgHeaderType::SectionHdr, sectionTable));

  // Add the Fixup Data stream.
  if (!fixups.empty())
    exitOnErr(dbiBuilder.addDbgStream(pdb::DbgHeaderType::Fixup, fixups));
}

void PDBLinker::commit(codeview::GUID *guid) {
  // Print an error and continue if PDB writing fails. This is done mainly so
  // the user can see the output of /time and /summary, which is very helpful
  // when trying to figure out why a PDB file is too large.
  if (Error e = builder.commit(ctx.config.pdbPath, guid)) {
    e = handleErrors(std::move(e), [&](const llvm::msf::MSFError &me) {
      Err(ctx) << me.message();
      if (me.isPageOverflow())
        Err(ctx) << "try setting a larger /pdbpagesize";
    });
    checkError(std::move(e));
    Err(ctx) << "failed to write PDB file " << Twine(ctx.config.pdbPath);
  }
}

static uint32_t getSecrelReloc(Triple::ArchType arch) {
  switch (arch) {
  case Triple::x86_64:
    return COFF::IMAGE_REL_AMD64_SECREL;
  case Triple::x86:
    return COFF::IMAGE_REL_I386_SECREL;
  case Triple::thumb:
    return COFF::IMAGE_REL_ARM_SECREL;
  case Triple::aarch64:
    return COFF::IMAGE_REL_ARM64_SECREL;
  default:
    llvm_unreachable("unknown machine type");
  }
}

// Try to find a line table for the given offset Addr into the given chunk C.
// If a line table was found, the line table, the string and checksum tables
// that are used to interpret the line table, and the offset of Addr in the line
// table are stored in the output arguments. Returns whether a line table was
// found.
static bool findLineTable(const SectionChunk *c, uint32_t addr,
                          DebugStringTableSubsectionRef &cvStrTab,
                          DebugChecksumsSubsectionRef &checksums,
                          DebugLinesSubsectionRef &lines,
                          uint32_t &offsetInLinetable) {
  ExitOnError exitOnErr;
  const uint32_t secrelReloc = getSecrelReloc(c->getArch());

  for (SectionChunk *dbgC : c->file->getDebugChunks()) {
    if (dbgC->getSectionName() != ".debug$S")
      continue;

    // Build a mapping of SECREL relocations in dbgC that refer to `c`.
    DenseMap<uint32_t, uint32_t> secrels;
    for (const coff_relocation &r : dbgC->getRelocs()) {
      if (r.Type != secrelReloc)
        continue;

      if (auto *s = dyn_cast_or_null<DefinedRegular>(
              c->file->getSymbols()[r.SymbolTableIndex]))
        if (s->getChunk() == c)
          secrels[r.VirtualAddress] = s->getValue();
    }

    ArrayRef<uint8_t> contents =
        SectionChunk::consumeDebugMagic(dbgC->getContents(), ".debug$S");
    DebugSubsectionArray subsections;
    BinaryStreamReader reader(contents, llvm::endianness::little);
    exitOnErr(reader.readArray(subsections, contents.size()));

    for (const DebugSubsectionRecord &ss : subsections) {
      switch (ss.kind()) {
      case DebugSubsectionKind::StringTable: {
        assert(!cvStrTab.valid() &&
               "Encountered multiple string table subsections!");
        exitOnErr(cvStrTab.initialize(ss.getRecordData()));
        break;
      }
      case DebugSubsectionKind::FileChecksums:
        assert(!checksums.valid() &&
               "Encountered multiple checksum subsections!");
        exitOnErr(checksums.initialize(ss.getRecordData()));
        break;
      case DebugSubsectionKind::Lines: {
        ArrayRef<uint8_t> bytes;
        auto ref = ss.getRecordData();
        exitOnErr(ref.readLongestContiguousChunk(0, bytes));
        size_t offsetInDbgC = bytes.data() - dbgC->getContents().data();

        // Check whether this line table refers to C.
        auto i = secrels.find(offsetInDbgC);
        if (i == secrels.end())
          break;

        // Check whether this line table covers Addr in C.
        DebugLinesSubsectionRef linesTmp;
        exitOnErr(linesTmp.initialize(BinaryStreamReader(ref)));
        uint32_t offsetInC = i->second + linesTmp.header()->RelocOffset;
        if (addr < offsetInC || addr >= offsetInC + linesTmp.header()->CodeSize)
          break;

        assert(!lines.header() &&
               "Encountered multiple line tables for function!");
        exitOnErr(lines.initialize(BinaryStreamReader(ref)));
        offsetInLinetable = addr - offsetInC;
        break;
      }
      default:
        break;
      }

      if (cvStrTab.valid() && checksums.valid() && lines.header())
        return true;
    }
  }

  return false;
}

// Use CodeView line tables to resolve a file and line number for the given
// offset into the given chunk and return them, or std::nullopt if a line table
// was not found.
std::optional<std::pair<StringRef, uint32_t>>
lld::coff::getFileLineCodeView(const SectionChunk *c, uint32_t addr) {
  ExitOnError exitOnErr;

  DebugStringTableSubsectionRef cvStrTab;
  DebugChecksumsSubsectionRef checksums;
  DebugLinesSubsectionRef lines;
  uint32_t offsetInLinetable;

  if (!findLineTable(c, addr, cvStrTab, checksums, lines, offsetInLinetable))
    return std::nullopt;

  std::optional<uint32_t> nameIndex;
  std::optional<uint32_t> lineNumber;
  for (const LineColumnEntry &entry : lines) {
    for (const LineNumberEntry &ln : entry.LineNumbers) {
      LineInfo li(ln.Flags);
      if (ln.Offset > offsetInLinetable) {
        if (!nameIndex) {
          nameIndex = entry.NameIndex;
          lineNumber = li.getStartLine();
        }
        StringRef filename =
            exitOnErr(getFileName(cvStrTab, checksums, *nameIndex));
        return std::make_pair(filename, *lineNumber);
      }
      nameIndex = entry.NameIndex;
      lineNumber = li.getStartLine();
    }
  }
  if (!nameIndex)
    return std::nullopt;
  StringRef filename = exitOnErr(getFileName(cvStrTab, checksums, *nameIndex));
  return std::make_pair(filename, *lineNumber);
}
