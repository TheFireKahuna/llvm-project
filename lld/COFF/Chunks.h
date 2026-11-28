//===- Chunks.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_CHUNKS_H
#define LLD_COFF_CHUNKS_H

#include "Config.h"
#include "InputFiles.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/iterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/WindowsMachineFlag.h"
#include <atomic>
#include <utility>
#include <vector>

namespace lld::coff {

using llvm::COFF::ImportDirectoryTableEntry;
using llvm::object::chpe_range_type;
using llvm::object::coff_relocation;
using llvm::object::coff_section;
using llvm::object::COFFSymbolRef;
using llvm::object::SectionRef;

class Baserel;
class Defined;
class DefinedImportData;
class DefinedRegular;
class ObjFile;
class OutputSection;
class RuntimePseudoReloc;
struct Relocation;
struct ScannedSection;
class Symbol;

// Mask for permissions (discardable, writable, readable, executable, etc).
const uint32_t permMask = 0xFE000000;

// Mask for section types (code, data, bss).
const uint32_t typeMask = 0x000000E0;

// The log base 2 of the largest section alignment, which is log2(8192), or 13.
enum : unsigned { Log2MaxSectionAlignment = 13 };

// A Chunk represents a chunk of data that will occupy space in the
// output (if the resolver chose that). It may or may not be backed by
// a section of an input file. It could be linker-created data, or
// doesn't even have actual data (if common or bss).
class Chunk {
public:
  enum Kind : uint8_t {
    SectionKind,
    SectionECKind,
    OtherKind,
    ImportThunkKind,
    ECExportThunkKind,
    FunctionOverrideKind,
    TlsRecordKind
  };
  Kind kind() const { return chunkKind; }

  // Returns the size of this chunk (even if this is a common or BSS.)
  size_t getSize() const;

  // Returns chunk alignment in power of two form. Value values are powers of
  // two from 1 to 8192.
  uint32_t getAlignment() const { return 1U << p2Align; }

  // Update the chunk section alignment measured in bytes. Internally alignment
  // is stored in log2.
  void setAlignment(uint32_t align) {
    // Treat zero byte alignment as 1 byte alignment.
    align = align ? align : 1;
    assert(llvm::isPowerOf2_32(align) && "alignment is not a power of 2");
    assert(llvm::Log2_32(align) <= Log2MaxSectionAlignment &&
           "impossible requested alignment");
    p2Align = llvm::Log2_32(align);
  }

  // Write this chunk to a mmap'ed file, assuming Buf is pointing to
  // beginning of the file. Because this function may use RVA values
  // of other chunks for relocations, you need to set them properly
  // before calling this function.
  void writeTo(uint8_t *buf) const;

  // The writer sets and uses the addresses. In practice, PE images cannot be
  // larger than 2GB. Chunks are always laid as part of the image, so Chunk RVAs
  // can be stored with 32 bits.
  uint32_t getRVA() const { return rva; }
  void setRVA(uint64_t v) {
    // This may truncate. The writer checks for overflow later.
    rva = (uint32_t)v;
  }

  // Returns readable/writable/executable bits.
  uint32_t getOutputCharacteristics() const;

  // Returns the section name if this is a section chunk.
  // It is illegal to call this function on non-section chunks.
  StringRef getSectionName() const;

  // An output section has pointers to chunks in the section, and each
  // chunk has a back pointer to an output section.
  void setOutputSectionIdx(uint16_t o) { osidx = o; }
  uint16_t getOutputSectionIdx() const { return osidx; }

  // Windows-specific.
  // Collect all locations that contain absolute addresses for base relocations.
  void getBaserels(std::vector<Baserel> *res);

  // Returns a human-readable name of this chunk. Chunks are unnamed chunks of
  // bytes, so this is used only for logging or debugging.
  StringRef getDebugName() const;

  // Return true if this file has the hotpatch flag set to true in the
  // S_COMPILE3 record in codeview debug info. Also returns true for some thunks
  // synthesized by the linker.
  bool isHotPatchable() const;

  MachineTypes getMachine() const;
  llvm::Triple::ArchType getArch() const;
  std::optional<chpe_range_type> getArm64ECRangeType() const;

  // ARM64EC entry thunk associated with the chunk.
  Defined *getEntryThunk() const;
  void setEntryThunk(Defined *entryThunk);

protected:
  Chunk(Kind k = OtherKind)
      : chunkKind(k), hasData(true), p2Align(0), leftOut(0) {}

  const Kind chunkKind;

public:
  // Returns true if this has non-zero data. BSS chunks return
  // false. If false is returned, the space occupied by this chunk
  // will be filled with zeros. Corresponds to the
  // IMAGE_SCN_CNT_UNINITIALIZED_DATA section characteristic bit.
  uint8_t hasData : 1;

public:
  // The alignment of this chunk, stored in log2 form. The writer uses the
  // value.
  uint8_t p2Align : 4;

  // The 4-byte words at the start of this chunk that the image leaves out:
  // the padding before a function's type prefix, which aligns the entry. The
  // chunk's address counts them, so they overlap the end of what precedes it,
  // and every offset into the chunk stands.
  uint8_t leftOut : 3;
  uint32_t getLeftOut() const { return leftOut * 4; }

  // The output section index for this chunk. The first valid section number is
  // one.
  uint16_t osidx = 0;

  // The RVA of this chunk in the output. The writer sets a value.
  uint32_t rva = 0;
};

class NonSectionChunk : public Chunk {
public:
  virtual ~NonSectionChunk() = default;

  // Returns the size of this chunk (even if this is a common or BSS.)
  virtual size_t getSize() const = 0;

  virtual uint32_t getOutputCharacteristics() const { return 0; }

  // Write this chunk to a mmap'ed file, assuming Buf is pointing to
  // beginning of the file. Because this function may use RVA values
  // of other chunks for relocations, you need to set them properly
  // before calling this function.
  virtual void writeTo(uint8_t *buf) const {}

  // Returns the section name if this is a section chunk.
  // It is illegal to call this function on non-section chunks.
  virtual StringRef getSectionName() const {
    llvm_unreachable("unimplemented getSectionName");
  }

  // Windows-specific.
  // Collect all locations that contain absolute addresses for base relocations.
  virtual void getBaserels(std::vector<Baserel> *res) {}

  virtual MachineTypes getMachine() const { return IMAGE_FILE_MACHINE_UNKNOWN; }

  // Returns a human-readable name of this chunk. Chunks are unnamed chunks of
  // bytes, so this is used only for logging or debugging.
  virtual StringRef getDebugName() const { return ""; }

  // Verify that chunk relocations are within their ranges.
  virtual bool verifyRanges() { return true; };

  // If needed, extend the chunk to ensure all relocations are within the
  // allowed ranges. Return the additional space required for the extension.
  virtual uint32_t extendRanges() { return 0; };

  virtual Defined *getEntryThunk() const { return nullptr; };

  static bool classof(const Chunk *c) { return c->kind() >= OtherKind; }

protected:
  NonSectionChunk(Kind k = OtherKind) : Chunk(k) {}
};

class NonSectionCodeChunk : public NonSectionChunk {
public:
  virtual uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_MEM_READ | llvm::COFF::IMAGE_SCN_MEM_EXECUTE;
  }

protected:
  NonSectionCodeChunk(Kind k = OtherKind) : NonSectionChunk(k) {}
};

// MinGW specific; information about one individual location in the image
// that needs to be fixed up at runtime after loading. This represents
// one individual element in the PseudoRelocTableChunk table.
class RuntimePseudoReloc {
public:
  RuntimePseudoReloc(Defined *sym, SectionChunk *target, uint32_t targetOffset,
                     int flags)
      : sym(sym), target(target), targetOffset(targetOffset), flags(flags) {}

  Defined *sym;
  SectionChunk *target;
  uint32_t targetOffset;
  // The Flags field contains the size of the relocation, in bits. No other
  // flags are currently defined.
  int flags;
};

// A chunk corresponding a section of an input file.
class SectionChunk : public Chunk {
  // Identical COMDAT Folding feature accesses section internal data.
  friend class ICF;

public:
  class symbol_iterator : public llvm::iterator_adaptor_base<
                              symbol_iterator, const coff_relocation *,
                              std::random_access_iterator_tag, Symbol *> {
    friend SectionChunk;

    const SectionChunk *chunk;

    symbol_iterator(const SectionChunk *chunk, const coff_relocation *i)
        : symbol_iterator::iterator_adaptor_base(i), chunk(chunk) {}

  public:
    symbol_iterator() = default;

    Symbol *operator*() const { return chunk->getRelocTarget(*I); }
  };

  SectionChunk(ObjFile *file, const coff_section *header, Kind k = SectionKind);
  static bool classof(const Chunk *c) { return c->kind() <= SectionECKind; }
  size_t getSize() const {
    return resizable ? getResizedSize() : header->SizeOfRawData;
  }
  size_t getResizedSize() const;
  ArrayRef<uint8_t> getContents() const;
  void writeTo(uint8_t *buf) const;
  MachineTypes getMachine() const;

  // Defend against unsorted relocations. This may be overly conservative.
  void sortRelocations();

  // Write and relocate a portion of the section. This is intended to be called
  // in a loop. Relocations must be sorted first.
  void writeAndRelocateSubsection(ArrayRef<uint8_t> sec,
                                  ArrayRef<uint8_t> subsec,
                                  uint32_t &nextRelocIndex, uint8_t *buf) const;

  uint32_t getOutputCharacteristics() const {
    return header->Characteristics & (permMask | typeMask);
  }
  StringRef getSectionName() const {
    return StringRef(sectionNameData, sectionNameSize);
  }
  void getBaserels(std::vector<Baserel> *res);
  bool isCOMDAT() const;
  // Applies rel, whose place is at offset in the output and whose scanned
  // relocation, holding its target, is r, or null if it has none.
  void applyRelocation(uint8_t *off, const coff_relocation &rel,
                       uint32_t offset, const Relocation *r) const;
  bool isPair(uint16_t type) const;
  const coff_relocation *getPair(const coff_relocation &rel) const;
  bool applyRelX64(uint8_t *off, uint16_t type, OutputSection *os, uint64_t s,
                   uint64_t p, uint64_t imageBase) const;
  void applyRelX86(uint8_t *off, uint16_t type, OutputSection *os, uint64_t s,
                   uint64_t p, uint64_t imageBase) const;
  void applyRelARM(uint8_t *off, uint16_t type, OutputSection *os, uint64_t s,
                   uint64_t p, uint64_t imageBase) const;
  bool applyRelARM64(uint8_t *off, uint16_t type, OutputSection *os, uint64_t s,
                     uint64_t p, uint64_t imageBase) const;
  void applyRelMIPS(uint8_t *off, uint16_t type, OutputSection *os, uint64_t s,
                    uint64_t p, uint64_t imageBase) const;

  void getRuntimePseudoRelocs(std::vector<RuntimePseudoReloc> &res);

  // Called if the garbage collector decides to not include this chunk
  // in a final output. It's supposed to print out a log message to stdout.
  void printDiscardedMessage() const;

  // Adds COMDAT associative sections to this COMDAT section. A chunk
  // and its children are treated as a group by the garbage collector.
  void addAssociative(SectionChunk *child);

  StringRef getDebugName() const;

  // True if this is a codeview debug info chunk. These will not be laid out in
  // the image. Instead they will end up in the PDB, if one is requested.
  bool isCodeView() const {
    return getSectionName() == ".debug" || getSectionName().starts_with(".debug$");
  }

  // True if this is a DWARF debug info or exception handling chunk.
  bool isDWARF() const {
    return getSectionName().starts_with(".debug_") || getSectionName() == ".eh_frame";
  }

  // Allow iteration over the bodies of this chunk's relocated symbols.
  llvm::iterator_range<symbol_iterator> symbols() const {
    return llvm::make_range(symbol_iterator(this, relocsData),
                            symbol_iterator(this, relocsData + relocsSize));
  }

  // Returns the symbol a relocation refers to, or null for one that refers to
  // nothing: a type that names no symbol, and a PAIR outside a fork object,
  // whose symbol then keeps nothing live and is ignored by ICF, as in link.exe.
  Symbol *getRelocTarget(const coff_relocation &rel) const;

  // The relocations the scan kept for this section, or null.
  ScannedSection *getScanned() const;

  // The target of relocation i, s being getScanned(): where the scan kept a
  // section's relocations, they hold the targets, which a later pass may
  // change.
  Symbol *getTarget(size_t i, const ScannedSection *s) const;

  // Where the byte at an input offset is in the output. A stored input offset
  // into a section the link may resize goes through this. With end, the offset
  // is where an instruction ends, before any padding that follows it.
  uint32_t getOutputOffset(uint32_t offset, bool end = false) const;

  ArrayRef<coff_relocation> getRelocs() const {
    return llvm::ArrayRef(relocsData, relocsSize);
  }

  // Reloc setter used by ARM range extension thunk insertion.
  void setRelocs(ArrayRef<coff_relocation> newRelocs) {
    relocsData = newRelocs.data();
    relocsSize = newRelocs.size();
    assert(relocsSize == newRelocs.size() && "reloc size truncation");
  }

  // Single linked list iterator for associated comdat children.
  class AssociatedIterator
      : public llvm::iterator_facade_base<
            AssociatedIterator, std::forward_iterator_tag, SectionChunk> {
  public:
    AssociatedIterator() = default;
    AssociatedIterator(SectionChunk *head) : cur(head) {}
    bool operator==(const AssociatedIterator &r) const { return cur == r.cur; }
    // FIXME: Wrong const-ness, but it makes filter ranges work.
    SectionChunk &operator*() const { return *cur; }
    SectionChunk &operator*() { return *cur; }
    AssociatedIterator &operator++() {
      cur = cur->assocChildren;
      return *this;
    }

  private:
    SectionChunk *cur = nullptr;
  };

  // Allow iteration over the associated child chunks for this section.
  llvm::iterator_range<AssociatedIterator> children() const {
    // Associated sections do not have children. The assocChildren field is
    // part of the parent's list of children.
    bool isAssoc = selection == llvm::COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE;
    return llvm::make_range(
        AssociatedIterator(isAssoc ? nullptr : assocChildren),
        AssociatedIterator(nullptr));
  }

  // The section ID this chunk belongs to in its Obj.
  uint32_t getSectionNumber() const;

  ArrayRef<uint8_t> consumeDebugMagic();

  static ArrayRef<uint8_t> consumeDebugMagic(ArrayRef<uint8_t> data,
                                             StringRef sectionName);

  static SectionChunk *findByName(ArrayRef<SectionChunk *> sections,
                                  StringRef name);

  // The file that this chunk was created from.
  ObjFile *file;

  // Pointer to the COFF section header in the input file.
  const coff_section *header;

  // The COMDAT leader symbol if this is a COMDAT chunk.
  DefinedRegular *sym = nullptr;

  // The CRC of the contents as described in the COFF spec 4.5.5.
  // Auxiliary Format 5: Section Definitions. Used for ICF.
  uint32_t checksum = 0;

  // Used by the garbage collector.
  bool live;

  // Whether this section needs to be kept distinct from other sections during
  // ICF. This is set by the driver using address-significance tables.
  bool keepUnique : 1;

  // Whether the object states that the linker may change the size of this
  // section: every value that depends on its layout is then a relocation or
  // CodeView.
  bool resizable : 1;

  // Whether the link moves the code of this resizable section freely: it is
  // no x86-64 section, or has a relocation that COFF::relocationMovesCode
  // names. Any other is compacted by the spans it states.
  bool movable : 1;

  // Whether the object states as spans the values that depend on this
  // resizable section's layout: the link moves its code only by deleting
  // bytes, those of a load before the call through its register that became
  // a direct call among them, and by redoing padding.
  bool compacts : 1;

  // Whether ICF must never fold this section: a program enumerates it through
  // __start_ and __stop_ symbols, or it is a function override's body, whose
  // branches stay sites of its own record.
  bool keepDistinct : 1;

  // Whether /guardsym suppresses a function here, so that it folds only with
  // another such section.
  bool guardSuppressed : 1;

  // Of the places 0, 16, 32 and 48 bytes into a 64-byte line, as bits 0 to
  // 3, those where this chunk may not begin: a loop of it that a line is to
  // keep would cross one there. Never all four. The object states some, and
  // the pass over a resized section those of the loops it lays out.
  mutable uint8_t linePlacesBarred = 0;

  // The COMDAT selection if this is a COMDAT chunk.
  llvm::COFF::COMDATType selection = (llvm::COFF::COMDATType)0;

  // A pointer pointing to a replacement for this chunk.
  // Initially it points to "this" object. If this chunk is merged
  // with other chunk by ICF, it points to another chunk,
  // and this chunk is considered as dead.
  SectionChunk *repl;

  void replace(SectionChunk *other);

private:
  SectionChunk *assocChildren = nullptr;

  // Used for ICF (Identical COMDAT Folding)
  uint32_t eqClass[2] = {0, 0};

  // Relocations for this section. Size is stored below.
  const coff_relocation *relocsData;

  // Section name string. Size is stored below.
  const char *sectionNameData;

  uint32_t relocsSize = 0;
  uint32_t sectionNameSize = 0;
};

// A section chunk corresponding a section of an EC input file.
class SectionChunkEC final : public SectionChunk {
public:
  static bool classof(const Chunk *c) { return c->kind() == SectionECKind; }

  SectionChunkEC(ObjFile *file, const coff_section *header)
      : SectionChunk(file, header, SectionECKind) {}
  Defined *entryThunk = nullptr;
};

// Inline methods to implement faux-virtual dispatch for SectionChunk.

inline size_t Chunk::getSize() const {
  if (isa<SectionChunk>(this))
    return static_cast<const SectionChunk *>(this)->getSize();
  return static_cast<const NonSectionChunk *>(this)->getSize();
}

inline uint32_t Chunk::getOutputCharacteristics() const {
  if (isa<SectionChunk>(this))
    return static_cast<const SectionChunk *>(this)->getOutputCharacteristics();
  return static_cast<const NonSectionChunk *>(this)->getOutputCharacteristics();
}

inline void Chunk::writeTo(uint8_t *buf) const {
  if (isa<SectionChunk>(this))
    static_cast<const SectionChunk *>(this)->writeTo(buf);
  else
    static_cast<const NonSectionChunk *>(this)->writeTo(buf);
}

inline StringRef Chunk::getSectionName() const {
  if (isa<SectionChunk>(this))
    return static_cast<const SectionChunk *>(this)->getSectionName();
  return static_cast<const NonSectionChunk *>(this)->getSectionName();
}

inline void Chunk::getBaserels(std::vector<Baserel> *res) {
  if (isa<SectionChunk>(this))
    static_cast<SectionChunk *>(this)->getBaserels(res);
  else
    static_cast<NonSectionChunk *>(this)->getBaserels(res);
}

inline StringRef Chunk::getDebugName() const {
  if (isa<SectionChunk>(this))
    return static_cast<const SectionChunk *>(this)->getDebugName();
  return static_cast<const NonSectionChunk *>(this)->getDebugName();
}

inline MachineTypes Chunk::getMachine() const {
  if (isa<SectionChunk>(this))
    return static_cast<const SectionChunk *>(this)->getMachine();
  return static_cast<const NonSectionChunk *>(this)->getMachine();
}

inline llvm::Triple::ArchType Chunk::getArch() const {
  return llvm::getMachineArchType(getMachine());
}

inline std::optional<chpe_range_type> Chunk::getArm64ECRangeType() const {
  // Data sections don't need codemap entries.
  if (!(getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
    return std::nullopt;

  switch (getMachine()) {
  case AMD64:
    return chpe_range_type::Amd64;
  case ARM64EC:
    return chpe_range_type::Arm64EC;
  default:
    return chpe_range_type::Arm64;
  }
}

// This class is used to implement an lld-specific feature (not implemented in
// MSVC) that minimizes the output size by finding string literals sharing tail
// parts and merging them.
//
// If string tail merging is enabled and a section is identified as containing a
// string literal, it is added to a MergeChunk with an appropriate alignment.
// The MergeChunk then tail merges the strings using the StringTableBuilder
// class and assigns RVAs and section offsets to each of the member chunks based
// on the offsets assigned by the StringTableBuilder.
class MergeChunk : public NonSectionChunk {
public:
  MergeChunk(uint32_t alignment);
  static void addSection(COFFLinkerContext &ctx, SectionChunk *c);
  void finalizeContents();
  void assignSubsectionRVAs();

  uint32_t getOutputCharacteristics() const override;
  StringRef getSectionName() const override { return ".rdata"; }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;

  std::vector<SectionChunk *> sections;

private:
  llvm::StringTableBuilder builder;
  bool finalized = false;
};

// A chunk for common symbols. Common chunks don't have actual data.
class CommonChunk : public NonSectionChunk {
public:
  CommonChunk(const COFFSymbolRef sym);
  size_t getSize() const override { return sym.getValue(); }
  uint32_t getOutputCharacteristics() const override;
  StringRef getSectionName() const override { return ".bss"; }

  bool live;

private:
  const COFFSymbolRef sym;
};

// A chunk for linker-created strings.
class StringChunk : public NonSectionChunk {
public:
  explicit StringChunk(StringRef s) : str(s) {}
  size_t getSize() const override { return str.size() + 1; }
  void writeTo(uint8_t *buf) const override;

private:
  StringRef str;
};

static const uint8_t importThunkX86[] = {
    0xff, 0x25, 0x00, 0x00, 0x00, 0x00, // JMP *0x0
};

static const uint8_t importThunkARM[] = {
    0x40, 0xf2, 0x00, 0x0c, // mov.w ip, #0
    0xc0, 0xf2, 0x00, 0x0c, // mov.t ip, #0
    0xdc, 0xf8, 0x00, 0xf0, // ldr.w pc, [ip]
};

static const uint8_t importThunkARM64[] = {
    0x10, 0x00, 0x00, 0x90, // adrp x16, #0
    0x10, 0x02, 0x40, 0xf9, // ldr  x16, [x16]
    0x00, 0x02, 0x1f, 0xd6, // br   x16
};

static const uint8_t importThunkARM64EC[] = {
    0x0b, 0x00, 0x00, 0x90, // adrp x11, 0x0
    0x6b, 0x01, 0x40, 0xf9, // ldr  x11, [x11]
    0x0a, 0x00, 0x00, 0x90, // adrp x10, 0x0
    0x4a, 0x01, 0x00, 0x91, // add  x10, x10, #0x0
    0x00, 0x00, 0x00, 0x14  // b    0x0
};

// Windows-specific.
// A chunk for DLL import jump table entry. In a final output, its
// contents will be a JMP instruction to some __imp_ symbol.
class ImportThunkChunk : public NonSectionCodeChunk {
public:
  ImportThunkChunk(COFFLinkerContext &ctx, Defined *s);
  static bool classof(const Chunk *c) { return c->kind() == ImportThunkKind; }

  // We track the usage of the thunk symbol separately from the import file
  // to avoid generating unnecessary thunks.
  bool live;

protected:
  Defined *impSymbol;
  COFFLinkerContext &ctx;
};

class ImportThunkChunkX64 : public ImportThunkChunk {
public:
  explicit ImportThunkChunkX64(COFFLinkerContext &ctx, Defined *s);
  size_t getSize() const override { return sizeof(importThunkX86); }
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return AMD64; }
};

class ImportThunkChunkX86 : public ImportThunkChunk {
public:
  explicit ImportThunkChunkX86(COFFLinkerContext &ctx, Defined *s)
      : ImportThunkChunk(ctx, s) {}
  size_t getSize() const override { return sizeof(importThunkX86); }
  void getBaserels(std::vector<Baserel> *res) override;
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return I386; }
};

class ImportThunkChunkARM : public ImportThunkChunk {
public:
  explicit ImportThunkChunkARM(COFFLinkerContext &ctx, Defined *s)
      : ImportThunkChunk(ctx, s) {
    setAlignment(2);
  }
  size_t getSize() const override { return sizeof(importThunkARM); }
  void getBaserels(std::vector<Baserel> *res) override;
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return ARMNT; }
};

class ImportThunkChunkARM64 : public ImportThunkChunk {
public:
  explicit ImportThunkChunkARM64(COFFLinkerContext &ctx, Defined *s,
                                 MachineTypes machine)
      : ImportThunkChunk(ctx, s), machine(machine) {
    setAlignment(4);
  }
  size_t getSize() const override { return near() ? 8 : 12; }
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return machine; }
  bool verifyRanges() override;
  uint32_t extendRanges() override;

private:
  // In an image with a fork object, the thunk reads the pointer with one ldr
  // (literal) where that reaches it.
  bool near() const;

  MachineTypes machine;
  bool far = false;
};

// ARM64EC __impchk_* thunk implementation.
// Performs an indirect call to an imported function pointer
// using the __icall_helper_arm64ec helper function.
class ImportThunkChunkARM64EC : public ImportThunkChunk {
public:
  explicit ImportThunkChunkARM64EC(ImportFile *file);
  size_t getSize() const override;
  MachineTypes getMachine() const override { return ARM64EC; }
  void writeTo(uint8_t *buf) const override;
  bool verifyRanges() override;
  uint32_t extendRanges() override;

  Defined *exitThunk = nullptr;
  Defined *sym = nullptr;
  bool extended = false;

private:
  ImportFile *file;
};

class RangeExtensionThunkARM : public NonSectionCodeChunk {
public:
  explicit RangeExtensionThunkARM(COFFLinkerContext &ctx, Defined *t)
      : target(t), ctx(ctx) {
    setAlignment(2);
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return ARMNT; }

  Defined *target;

private:
  COFFLinkerContext &ctx;
};

// A ragnge extension thunk used for both ARM64EC and ARM64 machine types.
class RangeExtensionThunkARM64 : public NonSectionCodeChunk {
public:
  explicit RangeExtensionThunkARM64(MachineTypes machine, Defined *t)
      : target(t), machine(machine) {
    setAlignment(4);
    assert(llvm::COFF::isAnyArm64(machine));
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return machine; }

  Defined *target;

private:
  MachineTypes machine;
};

// A chunk used to guarantee the same address for a function in both views of
// a hybrid image. Similar to RangeExtensionThunkARM64 chunks, it calls the
// target symbol using a BR instruction. It also contains an entry thunk for EC
// compatibility and additional ARM64X relocations that swap targets between
// views.
class SameAddressThunkARM64EC : public RangeExtensionThunkARM64 {
public:
  explicit SameAddressThunkARM64EC(Defined *t, Defined *hybridTarget,
                                   Defined *entryThunk)
      : RangeExtensionThunkARM64(ARM64EC, t), hybridTarget(hybridTarget),
        entryThunk(entryThunk) {}

  Defined *getEntryThunk() const override { return entryThunk; }
  void setDynamicRelocs(COFFLinkerContext &ctx) const;

private:
  Defined *hybridTarget;
  Defined *entryThunk;
};

// Windows-specific.
// See comments for DefinedLocalImport class.
class LocalImportChunk : public NonSectionChunk {
public:
  explicit LocalImportChunk(COFFLinkerContext &ctx, Defined *s);
  size_t getSize() const override;
  void getBaserels(std::vector<Baserel> *res) override;
  void writeTo(uint8_t *buf) const override;
  Defined *getTarget() const { return sym; }
  void setTarget(Defined *s) { sym = s; }

  // How roots and live sections use the pointer, which the scan sets from
  // several threads: a Load reaches the target where the scan rewrites it.
  // The pointer is left out unless a use reads or keeps it.
  enum Use : uint8_t { Keep = 1, Load = 2, Read = 4 };
  std::atomic<uint8_t> uses = 0;
  bool isBypassed() const { return (uses & (Load | Read)) == Load; }
  bool isLive() const { return uses & (Keep | Read); }
  // The compiler's .refptr cell that this pointer binds, if any. The cell
  // stays the pointer, so that it keeps one address, and this one is never
  // written.
  SectionChunk *cell = nullptr;
  // Whether the target is the default of a weak reference to X, and so no
  // definition of X.
  bool weakDefault = false;
  // Whether the image's fill may write the pointer, which code then reads
  // where it is.
  bool filled = false;

private:
  Defined *sym;
  COFFLinkerContext &ctx;
};

// Duplicate RVAs are not allowed in RVA tables, so unique symbols by chunk and
// offset into the chunk. Order does not matter as the RVA table will be sorted
// later.
struct ChunkAndOffset {
  Chunk *inputChunk;
  uint32_t offset;

  struct DenseMapInfo {
    static unsigned getHashValue(const ChunkAndOffset &co) {
      return llvm::DenseMapInfo<std::pair<Chunk *, uint32_t>>::getHashValue(
          {co.inputChunk, co.offset});
    }
    static bool isEqual(const ChunkAndOffset &lhs, const ChunkAndOffset &rhs) {
      return lhs.inputChunk == rhs.inputChunk && lhs.offset == rhs.offset;
    }
  };
};

using SymbolRVASet = llvm::DenseSet<ChunkAndOffset>;

// Table which contains symbol RVAs. Used for /safeseh and /guard:cf.
class RVATableChunk : public NonSectionChunk {
public:
  explicit RVATableChunk(SymbolRVASet s) : syms(std::move(s)) {}
  size_t getSize() const override { return syms.size() * 4; }
  void writeTo(uint8_t *buf) const override { write(buf); }
  // Writes the table and returns the number of its entries. A table holds an
  // address once, so it has fewer entries than symbols, and unused bytes after
  // them, where resizing brought two of its symbols to one address.
  virtual size_t write(uint8_t *buf) const;

protected:
  SymbolRVASet syms;
};

// Table which contains symbol RVAs with flags. Used for /guard:cf.
class RVAFlagTableChunk : public RVATableChunk {
public:
  RVAFlagTableChunk(SymbolRVASet s, llvm::DenseMap<ChunkAndOffset, uint8_t> f)
      : RVATableChunk(std::move(s)), flags(std::move(f)) {}
  size_t getSize() const override { return syms.size() * 5; }
  size_t write(uint8_t *buf) const override;

private:
  llvm::DenseMap<ChunkAndOffset, uint8_t> flags;
};

// Windows-specific.
// This class represents a block in .reloc section.
// See the PE/COFF spec 5.6 for details.
class BaserelChunk : public NonSectionChunk {
public:
  BaserelChunk(uint32_t page, Baserel *begin, Baserel *end);
  size_t getSize() const override { return data.size(); }
  void writeTo(uint8_t *buf) const override;

private:
  std::vector<uint8_t> data;
};

class Baserel {
public:
  Baserel(uint32_t v, uint8_t ty) : rva(v), type(ty) {}
  explicit Baserel(uint32_t v, llvm::COFF::MachineTypes machine)
      : Baserel(v, getDefaultType(machine)) {}
  static uint8_t getDefaultType(llvm::COFF::MachineTypes machine);

  uint32_t rva;
  uint8_t type;
};

// This is a placeholder Chunk, to allow attaching a DefinedSynthetic to a
// specific place in a section, without any data. This is used for the MinGW
// specific symbol __RUNTIME_PSEUDO_RELOC_LIST_END__, even though the concept
// of an empty chunk isn't MinGW specific. With a size, it is a gap of zeros.
class EmptyChunk : public NonSectionChunk {
public:
  explicit EmptyChunk(size_t size = 0) : size(size) {}
  size_t getSize() const override { return size; }
  void writeTo(uint8_t *buf) const override { memset(buf, 0, size); }

private:
  size_t size;
};

class ECCodeMapEntry {
public:
  ECCodeMapEntry(Chunk *first, Chunk *last, chpe_range_type type)
      : first(first), last(last), type(type) {}
  Chunk *first;
  Chunk *last;
  chpe_range_type type;
};

// This is a chunk containing CHPE code map on EC targets. It's a table
// of address ranges and their types.
class ECCodeMapChunk : public NonSectionChunk {
public:
  ECCodeMapChunk(std::vector<ECCodeMapEntry> &map) : map(map) {}
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;

private:
  std::vector<ECCodeMapEntry> &map;
};

class CHPECodeRangesChunk : public NonSectionChunk {
public:
  CHPECodeRangesChunk(std::vector<std::pair<Chunk *, Defined *>> &exportThunks)
      : exportThunks(exportThunks) {}
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;

private:
  std::vector<std::pair<Chunk *, Defined *>> &exportThunks;
};

class CHPERedirectionChunk : public NonSectionChunk {
public:
  CHPERedirectionChunk(std::vector<std::pair<Chunk *, Defined *>> &exportThunks)
      : exportThunks(exportThunks) {}
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;

private:
  std::vector<std::pair<Chunk *, Defined *>> &exportThunks;
};

static const uint8_t ECExportThunkCode[] = {
    0x48, 0x8b, 0xc4,          // movq    %rsp, %rax
    0x48, 0x89, 0x58, 0x20,    // movq    %rbx, 0x20(%rax)
    0x55,                      // pushq   %rbp
    0x5d,                      // popq    %rbp
    0xe9, 0,    0,    0,    0, // jmp *0x0
    0xcc,                      // int3
    0xcc                       // int3
};

class ECExportThunkChunk : public NonSectionCodeChunk {
public:
  explicit ECExportThunkChunk(Defined *targetSym)
      : NonSectionCodeChunk(ECExportThunkKind), target(targetSym) {}
  static bool classof(const Chunk *c) { return c->kind() == ECExportThunkKind; }

  size_t getSize() const override { return sizeof(ECExportThunkCode); };
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return AMD64; }

  Defined *target;
};

// ARM64X relocation value, potentially relative to a symbol.
class Arm64XRelocVal {
public:
  Arm64XRelocVal(uint64_t value = 0) : value(value) {}
  Arm64XRelocVal(Defined *sym, int32_t offset = 0) : sym(sym), value(offset) {}
  Arm64XRelocVal(const Chunk *chunk, int32_t offset = 0)
      : chunk(chunk), value(offset) {}
  uint64_t get() const;

private:
  Defined *sym = nullptr;
  const Chunk *chunk = nullptr;
  uint64_t value;
};

// ARM64X entry for dynamic relocations.
class Arm64XDynamicRelocEntry {
public:
  Arm64XDynamicRelocEntry(llvm::COFF::Arm64XFixupType type, uint8_t size,
                          Arm64XRelocVal offset, Arm64XRelocVal value)
      : offset(offset), value(value), type(type), size(size) {}

  size_t getSize() const;
  void writeTo(uint8_t *buf) const;

  Arm64XRelocVal offset;
  Arm64XRelocVal value;

private:
  llvm::COFF::Arm64XFixupType type;
  uint8_t size;
};

class FunctionOverrideThunk;

// A function whose body Windows selects as it maps the image: its default
// body, the binary decision diagram that selects one of the candidates, the
// stub that stands for the function, and the sites, branches to the default
// body that Windows rewrites to the selected one. The function of an object
// that LLVM made for Windows Itanium or NT-POSIX has the compiler's stub and
// none of the link's, and its symbol is its default body.
struct FunctionOverride {
  // Returns the function override that s is the default body of.
  static FunctionOverride *get(const Symbol *s);

  DefinedRegular *body;
  ArrayRef<uint8_t> diagram;
  std::vector<Defined *> candidates;
  FunctionOverrideThunk *stub = nullptr;
  bool live = false;
  // The RVA of what Windows rewrites at a site: a section's is its
  // relocation's index there, and the stub's an offset.
  uint32_t getSiteRVA(std::pair<const Chunk *, uint32_t> site) const;
  std::vector<std::pair<const Chunk *, uint32_t>> sites;
};

// A function override's stub: a copy of the body's prefix [__cfi_X, X), so
// that a KCFI check of the stub passes, then a branch to the default body.
// The stub is a Control Flow Guard target, so the copy ends on a 16-byte
// boundary; the bytes before it are not written, and overlap what precedes
// the stub.
class FunctionOverrideThunk : public NonSectionCodeChunk {
public:
  FunctionOverrideThunk(FunctionOverride &fo, ArrayRef<uint8_t> prefix,
                        MachineTypes machine, bool live)
      : NonSectionCodeChunk(FunctionOverrideKind), fo(fo), prefix(prefix),
        machine(machine), live(live) {
    setAlignment(16);
    leftOut = (getEntry() - prefix.size()) / 4;
  }
  static bool classof(const Chunk *c) {
    return c->kind() == FunctionOverrideKind;
  }
  // Returns the stub that s stands for, if s is a function override.
  static FunctionOverrideThunk *get(const Symbol *s);

  // The offset of the stub's branch, its entry.
  uint32_t getEntry() const { return llvm::alignTo(prefix.size(), 16); }
  size_t getSize() const override {
    return getEntry() + (machine == AMD64 ? 5 : 4);
  }
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override { return machine; }

  FunctionOverride &fo;
  ArrayRef<uint8_t> prefix;
  MachineTypes machine;
  bool live;
};

// What the override table names for an overridable default that forwards to
// a replacement across a dropped argument: the default's type prefix, which
// the link writes from the image's key and the default's type ID, so that
// the default's check of what it transfers to passes, then a jump to the
// replacement, after the arguments that follow a dropped middle one move
// down. A jump to a DLL's offer is through the executable's pointer to it.
// ARM64 jumps by an address in a register where the replacement is out of a
// branch's reach. The prefix ends on a 16-byte boundary, as a function
// override's stub's does.
class OverrideThunk : public NonSectionCodeChunk {
public:
  OverrideThunk(COFFLinkerContext &ctx, StringRef key,
                std::optional<uint32_t> typeId, Defined *target, bool moves);
  uint32_t getEntry() const { return llvm::alignTo(prefix.size(), 16); }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override;

private:
  COFFLinkerContext &ctx;
  SmallVector<uint8_t, 0> prefix;
  Defined *target;
  bool moves;
};

// The anchor of an executable's override table, __ovr_anchor: the table's
// header with the masks of the slots that a definition replaces and of those
// whose entry is the executable's pointer to a DLL's offer, then for each
// slot the address of what stands for its default, which is null where the
// default itself does, and last for each slot the slot whose entry a call of
// its default reaches. An image that reads the table checks each slot against
// it, and it makes every such target one of Control Flow Guard's.
class OverrideAnchorChunk : public NonSectionChunk {
public:
  explicit OverrideAnchorChunk(COFFLinkerContext &ctx) : ctx(ctx) {
    setAlignment(8);
  }
  size_t getSize() const override {
    return llvm::COFF::OverrideAnchorHeaderSize + count * 8 +
           llvm::alignTo(count, 8);
  }
  void getBaserels(std::vector<Baserel> *res) override;
  void writeTo(uint8_t *buf) const override;
  // Writes the table in the header page that begins at page.
  void writeTable(uint8_t *page) const;
  // The offset of the table's slot i in the header page.
  static uint32_t getSlotOffset(unsigned i) {
    return llvm::COFF::OverrideTableOffset + 8 * i +
           (i < 8 ? 0 : llvm::COFF::OverrideTableHeaderSize);
  }

  // A replacement, a thunk's entry or the executable's import of a DLL's
  // offer.
  Defined *slots[llvm::COFF::LinkFactMaxOverrideSlot + 1] = {};
  uint8_t calls[llvm::COFF::LinkFactMaxOverrideSlot + 1] = {};
  // The number of slots that the table has.
  unsigned count = 8;
  uint32_t mask = 0;

private:
  void writeHeader(uint8_t *buf) const;
  COFFLinkerContext &ctx;
};

// The image's offer table (llvm::COFF::OfferTableMagic): each name of a vague
// entity that the image keeps, at its instance or at the cell through which
// it reaches it, in a DLL each that it imports from a DLL that keeps it
// preemptible, at the import's entry, and in an executable each that it takes
// from the owner that its link picks, at the import's entry. A guard's entry
// states its variable's size, so that a fill binds the two together.
class OfferTableChunk : public NonSectionChunk {
public:
  OfferTableChunk() { setAlignment(8); }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  // The key of a name, by which a fill finds an entry.
  static uint64_t getKey(StringRef name);
  // The identity of an image's entry, which a fill compares as its size: a
  // digest of what the record of image, of the images closure, states that
  // a fill relies on, what the image keeps and what it imports from.
  static uint32_t getIdentity(ArrayRef<llvm::object::COFFLinkImage> closure,
                              const llvm::object::COFFLinkImage &image);
  // Orders the entries by key, reporting two names with one, and names each
  // by a string the image holds: its import's hint, its export or the
  // table's own.
  void finalizeContents(COFFLinkerContext &ctx);
  // The RVA of the entry that names at, and the entry's key.
  std::pair<uint32_t, uint64_t> getEntry(const Defined *at) const;
  // The RVA of the entry that names the image name.
  uint32_t getImageEntry(StringRef name) const;

  struct Entry {
    StringRef name;
    Defined *at;
    uint16_t flags;
    uint8_t log2Align;
    uint64_t size;
    uint64_t key = 0;
    Chunk *nameChunk = nullptr;
    uint32_t nameOffset = 0;
  };
  std::vector<Entry> entries;
  // The identity of a DLL's record, which its own entry states.
  uint32_t identity = 0;

private:
  uint32_t getNamesOffset() const;
  std::string names;
  llvm::DenseMap<const Defined *, uint32_t> indices;
};

// The record of a thread-local variable X that a link fact states, the symbol
// X$tls: the address of the image's TLS index and the offsets of X and of
// its guard from the first TLS group. It is left out, and takes no room,
// unless something still names it once the relocations are scanned.
class TlsRecordChunk : public NonSectionChunk {
public:
  TlsRecordChunk(SymbolTable &symtab, Symbol *sym, Symbol *var,
                 InputFile *file);
  static bool classof(const Chunk *c) { return c->kind() == TlsRecordKind; }
  // Returns the record that s is the symbol of, if any.
  static TlsRecordChunk *get(const Symbol *s);
  size_t getSize() const override { return named ? 16 : 0; }
  void getBaserels(std::vector<Baserel> *res) override;
  void writeTo(uint8_t *buf) const override;

  // Whether an input names the record or reads its pointer once the
  // relocations are scanned, or before LTO whether one may.
  bool isNamed(bool beforeLTO = false) const;
  // Names the record or not, which gives it its room.
  void setNamed(bool is) {
    named = is;
    setAlignment(is ? 8 : 1);
  }
  // Whether a rewrite of the scan reaches the record, which the scan sets
  // from several threads, or after it whether the image holds the record.
  std::atomic<bool> named = false;

  Symbol *sym;
  Symbol *var;
  // The first object that states the record.
  InputFile *file;
  // The thread-local byte that is not zero once a thread has initialized
  // the variable, which an object's fact states.
  Symbol *guard = nullptr;
  // Whether a root or a live section reaches the record.
  bool live;
  SymbolTable &symtab;
};

// An image's place word, which its records name and
// __imp___tp_register$init is: {place, form, key}, the link-time place
// where the image has one, else unfilled until its fill registers it.
class TlsPlaceChunk : public NonSectionChunk {
public:
  explicit TlsPlaceChunk(COFFLinkerContext &ctx) : ctx(ctx) { setAlignment(8); }
  size_t getSize() const override { return TP_PLACE_WORD_SIZE; }
  void writeTo(uint8_t *buf) const override;

  // Whether the image's fill registers the image and writes the place.
  bool registers = false;

private:
  COFFLinkerContext &ctx;
};

// The dynamic value relocation table: the ARM64X relocations of a hybrid
// image, then the function-override record.
class DynamicRelocsChunk : public NonSectionChunk {
public:
  DynamicRelocsChunk() {}
  size_t getSize() const override { return size; }
  void writeTo(uint8_t *buf) const override;
  void finalize();

  void add(llvm::COFF::Arm64XFixupType type, uint8_t size,
           Arm64XRelocVal offset, Arm64XRelocVal value = Arm64XRelocVal()) {
    arm64xRelocs.emplace_back(type, size, offset, value);
  }

  void set(Arm64XRelocVal offset, Arm64XRelocVal value);

  std::vector<FunctionOverride *> functionOverrides;

private:
  std::vector<Arm64XDynamicRelocEntry> arm64xRelocs;
  // The function-override record, built once the sites have RVAs.
  std::string functionOverrideRecord;
  size_t size;
};

// MinGW specific, for the "automatic import of variables from DLLs" feature.
// This provides the table of runtime pseudo relocations, for variable
// references that turned out to need to be imported from a DLL even though
// the reference didn't use the dllimport attribute. The MinGW runtime will
// process this table after loading, before handling control over to user
// code.
class PseudoRelocTableChunk : public NonSectionChunk {
public:
  PseudoRelocTableChunk(std::vector<RuntimePseudoReloc> &relocs)
      : relocs(std::move(relocs)) {
    setAlignment(4);
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;

private:
  std::vector<RuntimePseudoReloc> relocs;
};

// The offset of a thread-local in the loader's template, which link.exe
// exports as __tls_offset_X for /export:X,THREAD_DATA.
class TlsOffsetChunk : public NonSectionChunk {
public:
  TlsOffsetChunk(COFFLinkerContext &ctx, Symbol *var) : ctx(ctx), var(var) {
    setAlignment(4);
  }
  size_t getSize() const override { return 4; }
  void writeTo(uint8_t *buf) const override;

private:
  COFFLinkerContext &ctx;
  Symbol *var;
};

// MinGW specific. A Chunk that contains one pointer-sized absolute value.
class AbsolutePointerChunk : public NonSectionChunk {
public:
  AbsolutePointerChunk(SymbolTable &symtab, uint64_t value)
      : value(value), symtab(symtab) {
    setAlignment(getSize());
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  MachineTypes getMachine() const override;

private:
  uint64_t value;
  SymbolTable &symtab;
};

// Return true if this file has the hotpatch flag set to true in the S_COMPILE3
// record in codeview debug info. Also returns true for some thunks synthesized
// by the linker.
inline bool Chunk::isHotPatchable() const {
  if (auto *sc = dyn_cast<SectionChunk>(this))
    return sc->file->hotPatchable;
  else if (isa<ImportThunkChunk>(this))
    return true;
  return false;
}

inline Defined *Chunk::getEntryThunk() const {
  if (auto *c = dyn_cast<const SectionChunkEC>(this))
    return c->entryThunk;
  if (auto *c = dyn_cast<const NonSectionChunk>(this))
    return c->getEntryThunk();
  return nullptr;
}

inline void Chunk::setEntryThunk(Defined *entryThunk) {
  if (auto c = dyn_cast<SectionChunkEC>(this))
    c->entryThunk = entryThunk;
}

void applyMOV32T(uint8_t *off, uint32_t v);
void applyBranch24T(uint8_t *off, int32_t v);

void applyArm64Addr(uint8_t *off, uint64_t s, uint64_t p, int shift);
void applyArm64Imm(uint8_t *off, uint64_t imm, uint32_t rangeLimit);
void applyArm64Ldr(uint8_t *off, uint64_t imm);
void applyArm64Branch26(uint8_t *off, int64_t v);
void applyArm64Ldr(uint8_t *off, uint64_t imm);
// The log2 of the bytes an ARM64 load or store with an unsigned offset moves.
uint32_t getArm64LdrScale(uint32_t insn);
// Whether the relocation type counts from a section's or a template's start.
bool isSecRel(llvm::Triple::ArchType arch, uint16_t type);

// Convenience class for initializing a coff_section with specific flags.
class FakeSection {
public:
  FakeSection(int c) { section.Characteristics = c; }

  coff_section section;
};

// Convenience class for initializing a SectionChunk with specific flags.
class FakeSectionChunk {
public:
  FakeSectionChunk(const coff_section *section) : chunk(nullptr, section) {
    // Comdats from LTO files can't be fully treated as regular comdats
    // at this point; we don't know what size or contents they are going to
    // have, so we can't do proper checking of such aspects of them.
    chunk.selection = llvm::COFF::IMAGE_COMDAT_SELECT_ANY;
  }

  SectionChunk chunk;
};

} // namespace lld::coff

namespace llvm {
template <>
struct DenseMapInfo<lld::coff::ChunkAndOffset>
    : lld::coff::ChunkAndOffset::DenseMapInfo {};
}

#endif
