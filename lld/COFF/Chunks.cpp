//===- Chunks.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "COFFLinkerContext.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <iterator>

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support;
using namespace llvm::support::endian;
using namespace llvm::COFF;
using llvm::support::ulittle32_t;

namespace lld::coff {

SectionChunk::SectionChunk(ObjFile *f, const coff_section *h, Kind k)
    : Chunk(k), file(f), header(h), repl(this) {
  // Initialize relocs.
  if (file)
    setRelocs(file->getCOFFObj()->getRelocations(header));

  // Initialize sectionName.
  StringRef sectionName;
  if (file) {
    if (Expected<StringRef> e = file->getCOFFObj()->getSectionName(header))
      sectionName = *e;
  }
  sectionNameData = sectionName.data();
  sectionNameSize = sectionName.size();

  setAlignment(header->getAlignment());

  hasData = !(header->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA);

  // If linker GC is disabled, every chunk starts out alive.  If linker GC is
  // enabled, treat non-comdat sections as roots. Generally optimized object
  // files will be built with -ffunction-sections or /Gy, so most things worth
  // stripping will be in a comdat.
  if (file)
    live = !file->symtab.ctx.config.doGC || !isCOMDAT();
  else
    live = true;
}

MachineTypes SectionChunk::getMachine() const {
  MachineTypes machine = file->getMachineType();
  // On ARM64EC, the IMAGE_SCN_GPREL flag is repurposed to indicate that section
  // code is x86_64. This enables embedding x86_64 code within ARM64EC object
  // files. MSVC uses this for export thunks in .exp files.
  if (isArm64EC(machine) && (header->Characteristics & IMAGE_SCN_GPREL))
    machine = AMD64;
  return machine;
}

// SectionChunk is one of the most frequently allocated classes, so it is
// important to keep it as compact as possible. As of this writing, the number
// below is the size of this class on x64 platforms.
static_assert(sizeof(SectionChunk) <= 88, "SectionChunk grew unexpectedly");

static void add16(uint8_t *p, int16_t v) { write16le(p, read16le(p) + v); }
static void add32(uint8_t *p, int32_t v) { write32le(p, read32le(p) + v); }
static void add64(uint8_t *p, int64_t v) { write64le(p, read64le(p) + v); }
static void or16(uint8_t *p, uint16_t v) { write16le(p, read16le(p) | v); }
static void or32(uint8_t *p, uint32_t v) { write32le(p, read32le(p) | v); }

// Verify that given sections are appropriate targets for SECREL
// relocations. This check is relaxed because unfortunately debug
// sections have section-relative relocations against absolute symbols.
static bool checkSecRel(const SectionChunk *sec, OutputSection *os) {
  if (os)
    return true;
  if (sec->isCodeView())
    return false;
  error("SECREL relocation cannot be applied to absolute symbols");
  return false;
}

static void applySecRel(const SectionChunk *sec, uint8_t *off,
                        OutputSection *os, uint64_t s) {
  if (!checkSecRel(sec, os))
    return;
  uint64_t secRel = s - os->getRVA();
  if (secRel > UINT32_MAX) {
    error("overflow in SECREL relocation in section: " + sec->getSectionName());
    return;
  }
  add32(off, secRel);
}

static void applySecIdx(uint8_t *off, OutputSection *os,
                        unsigned numOutputSections) {
  // numOutputSections is the largest valid section index. Make sure that
  // it fits in 16 bits.
  assert(numOutputSections <= 0xffff && "size of outputSections is too big");

  // Absolute symbol doesn't have section index, but section index relocation
  // against absolute symbol should be resolved to one plus the last output
  // section index. This is required for compatibility with MSVC.
  if (os)
    add16(off, os->sectionIndex);
  else
    add16(off, numOutputSections + 1);
}

void SectionChunk::applyRelX64(uint8_t *off, uint16_t type, OutputSection *os,
                               uint64_t s, uint64_t p,
                               uint64_t imageBase) const {
  switch (type) {
  case IMAGE_REL_AMD64_ADDR32:
    add32(off, s + imageBase);
    break;
  case IMAGE_REL_AMD64_ADDR64:
    add64(off, s + imageBase);
    break;
  case IMAGE_REL_AMD64_ADDR32NB: add32(off, s); break;
  case IMAGE_REL_AMD64_REL32:    add32(off, s - p - 4); break;
  case IMAGE_REL_AMD64_REL32_1:  add32(off, s - p - 5); break;
  case IMAGE_REL_AMD64_REL32_2:  add32(off, s - p - 6); break;
  case IMAGE_REL_AMD64_REL32_3:  add32(off, s - p - 7); break;
  case IMAGE_REL_AMD64_REL32_4:  add32(off, s - p - 8); break;
  case IMAGE_REL_AMD64_REL32_5:  add32(off, s - p - 9); break;
  case IMAGE_REL_AMD64_SECTION:
    applySecIdx(off, os, file->symtab.ctx.outputSections.size());
    break;
  case IMAGE_REL_AMD64_SECREL:   applySecRel(this, off, os, s); break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
}

void SectionChunk::applyRelX86(uint8_t *off, uint16_t type, OutputSection *os,
                               uint64_t s, uint64_t p,
                               uint64_t imageBase) const {
  switch (type) {
  case IMAGE_REL_I386_ABSOLUTE: break;
  case IMAGE_REL_I386_DIR32:
    add32(off, s + imageBase);
    break;
  case IMAGE_REL_I386_DIR32NB:  add32(off, s); break;
  case IMAGE_REL_I386_REL32:    add32(off, s - p - 4); break;
  case IMAGE_REL_I386_SECTION:
    applySecIdx(off, os, file->symtab.ctx.outputSections.size());
    break;
  case IMAGE_REL_I386_SECREL:   applySecRel(this, off, os, s); break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
}

static void applyMOV(uint8_t *off, uint16_t v) {
  write16le(off, (read16le(off) & 0xfbf0) | ((v & 0x800) >> 1) | ((v >> 12) & 0xf));
  write16le(off + 2, (read16le(off + 2) & 0x8f00) | ((v & 0x700) << 4) | (v & 0xff));
}

static uint16_t readMOV(uint8_t *off, bool movt) {
  uint16_t op1 = read16le(off);
  if ((op1 & 0xfbf0) != (movt ? 0xf2c0 : 0xf240))
    error("unexpected instruction in " + Twine(movt ? "MOVT" : "MOVW") +
          " instruction in MOV32T relocation");
  uint16_t op2 = read16le(off + 2);
  if ((op2 & 0x8000) != 0)
    error("unexpected instruction in " + Twine(movt ? "MOVT" : "MOVW") +
          " instruction in MOV32T relocation");
  return (op2 & 0x00ff) | ((op2 >> 4) & 0x0700) | ((op1 << 1) & 0x0800) |
         ((op1 & 0x000f) << 12);
}

void applyMOV32T(uint8_t *off, uint32_t v) {
  uint16_t immW = readMOV(off, false);    // read MOVW operand
  uint16_t immT = readMOV(off + 4, true); // read MOVT operand
  uint32_t imm = immW | (immT << 16);
  v += imm;                         // add the immediate offset
  applyMOV(off, v);           // set MOVW operand
  applyMOV(off + 4, v >> 16); // set MOVT operand
}

static void applyBranch20T(uint8_t *off, int32_t v) {
  if (!isInt<21>(v))
    error("relocation out of range");
  uint32_t s = v < 0 ? 1 : 0;
  uint32_t j1 = (v >> 19) & 1;
  uint32_t j2 = (v >> 18) & 1;
  or16(off, (s << 10) | ((v >> 12) & 0x3f));
  or16(off + 2, (j1 << 13) | (j2 << 11) | ((v >> 1) & 0x7ff));
}

void applyBranch24T(uint8_t *off, int32_t v) {
  if (!isInt<25>(v))
    error("relocation out of range");
  uint32_t s = v < 0 ? 1 : 0;
  uint32_t j1 = ((~v >> 23) & 1) ^ s;
  uint32_t j2 = ((~v >> 22) & 1) ^ s;
  or16(off, (s << 10) | ((v >> 12) & 0x3ff));
  // Clear out the J1 and J2 bits which may be set.
  write16le(off + 2, (read16le(off + 2) & 0xd000) | (j1 << 13) | (j2 << 11) | ((v >> 1) & 0x7ff));
}

void SectionChunk::applyRelARM(uint8_t *off, uint16_t type, OutputSection *os,
                               uint64_t s, uint64_t p,
                               uint64_t imageBase) const {
  // Pointer to thumb code must have the LSB set.
  uint64_t sx = s;
  if (os && (os->header.Characteristics & IMAGE_SCN_MEM_EXECUTE))
    sx |= 1;
  switch (type) {
  case IMAGE_REL_ARM_ADDR32:
    add32(off, sx + imageBase);
    break;
  case IMAGE_REL_ARM_ADDR32NB:  add32(off, sx); break;
  case IMAGE_REL_ARM_MOV32T:
    applyMOV32T(off, sx + imageBase);
    break;
  case IMAGE_REL_ARM_BRANCH20T: applyBranch20T(off, sx - p - 4); break;
  case IMAGE_REL_ARM_BRANCH24T: applyBranch24T(off, sx - p - 4); break;
  case IMAGE_REL_ARM_BLX23T:    applyBranch24T(off, sx - p - 4); break;
  case IMAGE_REL_ARM_SECTION:
    applySecIdx(off, os, file->symtab.ctx.outputSections.size());
    break;
  case IMAGE_REL_ARM_SECREL:    applySecRel(this, off, os, s); break;
  case IMAGE_REL_ARM_REL32:     add32(off, sx - p - 4); break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
}

// Interpret the existing immediate value as a byte offset to the
// target symbol, then update the instruction with the immediate as
// the page offset from the current instruction to the target.
void applyArm64Addr(uint8_t *off, uint64_t s, uint64_t p, int shift) {
  uint32_t orig = read32le(off);
  int64_t imm =
      SignExtend64<21>(((orig >> 29) & 0x3) | ((orig >> 3) & 0x1FFFFC));
  s += imm;
  imm = (s >> shift) - (p >> shift);
  uint32_t immLo = (imm & 0x3) << 29;
  uint32_t immHi = (imm & 0x1FFFFC) << 3;
  uint64_t mask = (0x3 << 29) | (0x1FFFFC << 3);
  write32le(off, (orig & ~mask) | immLo | immHi);
}

// Update the immediate field in a AARCH64 ldr, str, and add instruction.
// Optionally limit the range of the written immediate by one or more bits
// (rangeLimit).
void applyArm64Imm(uint8_t *off, uint64_t imm, uint32_t rangeLimit) {
  uint32_t orig = read32le(off);
  imm += (orig >> 10) & 0xFFF;
  orig &= ~(0xFFF << 10);
  write32le(off, orig | ((imm & (0xFFF >> rangeLimit)) << 10));
}

// Add the 12 bit page offset to the existing immediate.
// Ldr/str instructions store the opcode immediate scaled
// by the load/store size (giving a larger range for larger
// loads/stores). The immediate is always (both before and after
// fixing up the relocation) stored scaled similarly.
// Even if larger loads/stores have a larger range, limit the
// effective offset to 12 bit, since it is intended to be a
// page offset.
static void applyArm64Ldr(uint8_t *off, uint64_t imm) {
  uint32_t orig = read32le(off);
  uint32_t size = orig >> 30;
  // 0x04000000 indicates SIMD/FP registers
  // 0x00800000 indicates 128 bit
  if ((orig & 0x4800000) == 0x4800000)
    size += 4;
  if ((imm & ((1 << size) - 1)) != 0)
    error("misaligned ldr/str offset");
  applyArm64Imm(off, imm >> size, size);
}

static void applySecRelLow12A(const SectionChunk *sec, uint8_t *off,
                              OutputSection *os, uint64_t s) {
  if (checkSecRel(sec, os))
    applyArm64Imm(off, (s - os->getRVA()) & 0xfff, 0);
}

static void applySecRelHigh12A(const SectionChunk *sec, uint8_t *off,
                               OutputSection *os, uint64_t s) {
  if (!checkSecRel(sec, os))
    return;
  uint32_t orig = read32le(off);
  uint64_t imm = (orig >> 10) & 0xFFF;
  orig &= ~(0xFFF << 10);
  imm = (s + imm - os->getRVA()) >> 12;
  if (0xfff < imm) {
    error("overflow in SECREL_HIGH12A relocation in section: " +
          sec->getSectionName());
    return;
  }
  write32le(off, orig | (imm << 10));
}

static void applySecRelLdr(const SectionChunk *sec, uint8_t *off,
                           OutputSection *os, uint64_t s) {
  if (checkSecRel(sec, os))
    applyArm64Ldr(off, (s - os->getRVA()) & 0xfff);
}

void applyArm64Branch26(uint8_t *off, int64_t v) {
  if (!isInt<28>(v))
    error("relocation out of range");
  or32(off, (v & 0x0FFFFFFC) >> 2);
}

static void applyArm64Branch19(uint8_t *off, int64_t v) {
  if (!isInt<21>(v))
    error("relocation out of range");
  or32(off, (v & 0x001FFFFC) << 3);
}

static void applyArm64Branch14(uint8_t *off, int64_t v) {
  if (!isInt<16>(v))
    error("relocation out of range");
  or32(off, (v & 0x0000FFFC) << 3);
}

void SectionChunk::applyRelARM64(uint8_t *off, uint16_t type, OutputSection *os,
                                 uint64_t s, uint64_t p,
                                 uint64_t imageBase) const {
  switch (type) {
  case IMAGE_REL_ARM64_PAGEBASE_REL21: applyArm64Addr(off, s, p, 12); break;
  case IMAGE_REL_ARM64_REL21:          applyArm64Addr(off, s, p, 0); break;
  case IMAGE_REL_ARM64_PAGEOFFSET_12A: applyArm64Imm(off, s & 0xfff, 0); break;
  case IMAGE_REL_ARM64_PAGEOFFSET_12L: applyArm64Ldr(off, s & 0xfff); break;
  case IMAGE_REL_ARM64_BRANCH26:       applyArm64Branch26(off, s - p); break;
  case IMAGE_REL_ARM64_BRANCH19:       applyArm64Branch19(off, s - p); break;
  case IMAGE_REL_ARM64_BRANCH14:       applyArm64Branch14(off, s - p); break;
  case IMAGE_REL_ARM64_ADDR32:
    add32(off, s + imageBase);
    break;
  case IMAGE_REL_ARM64_ADDR32NB:       add32(off, s); break;
  case IMAGE_REL_ARM64_ADDR64:
    add64(off, s + imageBase);
    break;
  case IMAGE_REL_ARM64_SECREL:         applySecRel(this, off, os, s); break;
  case IMAGE_REL_ARM64_SECREL_LOW12A:  applySecRelLow12A(this, off, os, s); break;
  case IMAGE_REL_ARM64_SECREL_HIGH12A: applySecRelHigh12A(this, off, os, s); break;
  case IMAGE_REL_ARM64_SECREL_LOW12L:  applySecRelLdr(this, off, os, s); break;
  case IMAGE_REL_ARM64_SECTION:
    applySecIdx(off, os, file->symtab.ctx.outputSections.size());
    break;
  case IMAGE_REL_ARM64_REL32:          add32(off, s - p - 4); break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
}

static void applyMipsBranch(uint8_t *off, int64_t v) {
  if (v & 3)
    error("misaligned jmp offset");
  add32(off, (v >> 2) & 0x03FFFFFC);
}

void SectionChunk::applyRelMIPS(uint8_t *off, uint16_t type, OutputSection *os,
                                uint64_t s, uint64_t p,
                                uint64_t imageBase) const {
  switch (type) {
  case IMAGE_REL_MIPS_REFWORD:
    add32(off, s + imageBase);
    break;
  case IMAGE_REL_MIPS_JMPADDR:
    applyMipsBranch(off, s + imageBase);
    break;
  case IMAGE_REL_MIPS_REFHI:
    add16(off, (s + imageBase) >> 16);
    break;
  case IMAGE_REL_MIPS_REFLO:
    add16(off, s + imageBase);
    break;
  case IMAGE_REL_MIPS_PAIR:
    // Nothing to do
    break;
  case IMAGE_REL_MIPS_REFWORDNB:
    add32(off, s);
    break;
  case IMAGE_REL_MIPS_SECTION:
    applySecIdx(off, os, file->symtab.ctx.outputSections.size());
    break;
  case IMAGE_REL_MIPS_SECREL:
    applySecRel(this, off, os, s);
    break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
}

static void maybeReportRelocationToDiscarded(const SectionChunk *fromChunk,
                                             Defined *sym,
                                             const coff_relocation &rel,
                                             bool isMinGW) {
  // Don't report these errors when the relocation comes from a debug info
  // section or in mingw mode. MinGW mode object files (built by GCC) can
  // have leftover sections with relocations against discarded comdat
  // sections. Such sections are left as is, with relocations untouched.
  if (fromChunk->isCodeView() || fromChunk->isDWARF() || isMinGW)
    return;

  // Get the name of the symbol. If it's null, it was discarded early, so we
  // have to go back to the object file.
  ObjFile *file = fromChunk->file;
  std::string name;
  if (sym) {
    name = toString(file->symtab.ctx, *sym);
  } else {
    COFFSymbolRef coffSym =
        check(file->getCOFFObj()->getSymbol(rel.SymbolTableIndex));
    name = maybeDemangleSymbol(
        file->symtab.ctx, check(file->getCOFFObj()->getSymbolName(coffSym)));
  }

  std::vector<std::string> symbolLocations =
      getSymbolLocations(file, rel.SymbolTableIndex);

  std::string out;
  llvm::raw_string_ostream os(out);
  os << "relocation against symbol in discarded section: " + name;
  for (const std::string &s : symbolLocations)
    os << s;
  error(out);
}

void SectionChunk::writeTo(uint8_t *buf) const {
  if (!hasData)
    return;
  // Copy section contents from source object file to output file.
  ArrayRef<uint8_t> a = getContents();
  if (!a.empty())
    memcpy(buf, a.data(), a.size());

  // Apply relocations.
  size_t inputSize = getSize();
  for (const coff_relocation &rel : getRelocs()) {
    // Check for an invalid relocation offset. This check isn't perfect, because
    // we don't have the relocation size, which is only known after checking the
    // machine and relocation type. As a result, a relocation may overwrite the
    // beginning of the following input section.
    if (rel.VirtualAddress >= inputSize) {
      error("relocation points beyond the end of its parent section");
      continue;
    }

    applyRelocation(buf + rel.VirtualAddress, rel);
  }
}

// Whether the bytes around a REL32 field at off are those of the form an
// object describes there. They verify the description, which is the only
// means of finding the instruction.
static bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off,
                       LinkSiteForm form) {
  // A displacement that names the pointer itself has no addend.
  if (off < 2 || off + 4 > data.size() || read32le(&data[off]) != 0)
    return false;
  uint8_t opcode = data[off - 2], modrm = data[off - 1];
  uint8_t prefix = off >= 3 ? data[off - 3] : 0;
  switch (form) {
  case LinkSiteCall:
    return opcode == 0xFF && modrm == 0x15;
  case LinkSiteJump:
    return opcode == 0xFF && modrm == 0x25;
  case LinkSiteJumpOnePrefix:
    // A REX prefix, or a legacy prefix.
    return off >= 3 && opcode == 0xFF && modrm == 0x25 &&
           ((prefix & 0xF0) == 0x40 ||
            is_contained({0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF2,
                          0xF3},
                         prefix));
  case LinkSiteLoad:
  case LinkSiteAddress:
    // mov r64, [rip+d] or lea r64, [rip+d], with REX.W, or with REX2 and its
    // W bit.
    return off >= 3 && opcode == (form == LinkSiteLoad ? 0x8B : 0x8D) &&
           (modrm & 0xC7) == 0x05 &&
           ((prefix & 0xF8) == 0x48 ||
            (off >= 4 && data[off - 4] == 0xD5 && (prefix & 0x88) == 0x08));
  default:
    return false;
  }
}

// Whether a local import pointer holds zero, the default of an absent weak
// reference, which an instruction can materialise instead of loading.
static bool isZeroPointer(DefinedLocalImport *li) {
  auto *abs = dyn_cast<DefinedAbsolute>(li->getTarget());
  return abs && abs->getVA() == 0;
}

// Whether a reference to a local import pointer can reach the pointer's
// symbol directly instead. An absolute symbol is not at an address relative to
// the image, but zero can be materialised.
static bool canBypass(ObjFile *file, const coff_relocation &rel) {
  auto *li = dyn_cast_or_null<DefinedLocalImport>(
      file->getSymbol(rel.SymbolTableIndex));
  return li && (!isa<DefinedAbsolute>(li->getTarget()) || isZeroPointer(li));
}

std::optional<LinkSiteForm>
SectionChunk::getLocalImportRewrite(const coff_relocation &rel,
                                    bool *mismatch) const {
  if (!file->describesSites || rel.Type != IMAGE_REL_AMD64_REL32 ||
      !canBypass(file, rel))
    return std::nullopt;
  std::optional<LinkSiteForm> form =
      file->getLinkSiteForm(this, rel.VirtualAddress);
  if (!form || *form == LinkSiteOther || *form == LinkSiteAddress)
    return std::nullopt;
  if (!isSiteForm(getContents(), rel.VirtualAddress, *form)) {
    if (mismatch)
      *mismatch = true;
    return std::nullopt;
  }
  // Zero is materialised only into a register named by a REX prefix, and for
  // a call. A jump through it keeps reading the pointer, since the unwinder
  // takes an epilogue to end only in a jump to an immediate or through memory.
  if (isZeroPointer(cast<DefinedLocalImport>(
          file->getSymbol(rel.SymbolTableIndex))) &&
      !(*form == LinkSiteCall ||
        (*form == LinkSiteLoad &&
         (getContents()[rel.VirtualAddress - 3] & 0xF0) == 0x40)))
    return std::nullopt;
  return form;
}

bool SectionChunk::isSectionRelative(const coff_relocation &rel) const {
  switch (getArch()) {
  case Triple::x86_64:
    return rel.Type == IMAGE_REL_AMD64_SECREL ||
           rel.Type == IMAGE_REL_AMD64_SECREL7 ||
           rel.Type == IMAGE_REL_AMD64_SECTION;
  case Triple::aarch64:
    return rel.Type == IMAGE_REL_ARM64_SECREL ||
           rel.Type == IMAGE_REL_ARM64_SECREL_LOW12A ||
           rel.Type == IMAGE_REL_ARM64_SECREL_HIGH12A ||
           rel.Type == IMAGE_REL_ARM64_SECREL_LOW12L ||
           rel.Type == IMAGE_REL_ARM64_SECTION;
  default:
    return false;
  }
}

bool SectionChunk::isDescribedSite(const coff_relocation &rel,
                                   LinkSiteForm form) const {
  return file->describesSites && rel.Type == IMAGE_REL_AMD64_REL32 &&
         file->getLinkSiteForm(this, rel.VirtualAddress) == form &&
         isSiteForm(getContents(), rel.VirtualAddress, form);
}

bool SectionChunk::isArm64AddressPair(const coff_relocation &adrp,
                                      const coff_relocation &add) const {
  ArrayRef<uint8_t> data = getContents();
  if (adrp.Type != IMAGE_REL_ARM64_PAGEBASE_REL21 ||
      add.Type != IMAGE_REL_ARM64_PAGEOFFSET_12A ||
      adrp.SymbolTableIndex != add.SymbolTableIndex ||
      uint64_t(adrp.VirtualAddress) + 4 != add.VirtualAddress ||
      uint64_t(add.VirtualAddress) + 4 > data.size())
    return false;
  uint32_t adrpInsn = read32le(&data[adrp.VirtualAddress]);
  uint32_t addInsn = read32le(&data[add.VirtualAddress]);
  // adrp xd with no addend, then add xd, xd, #0.
  return (adrpInsn & 0x9F000000) == 0x90000000 &&
         (adrpInsn & 0x60FFFFE0) == 0 && (addInsn & 0xFFFFFC00) == 0x91000000 &&
         (addInsn & 0x1F) == (adrpInsn & 0x1F) &&
         ((addInsn >> 5) & 0x1F) == (adrpInsn & 0x1F);
}

bool SectionChunk::isArm64PointerLoad(const coff_relocation &rel) const {
  ArrayRef<uint8_t> data = getContents();
  if (uint64_t(rel.VirtualAddress) + 4 > data.size())
    return false;
  uint32_t insn = read32le(&data[rel.VirtualAddress]);
  switch (rel.Type) {
  case IMAGE_REL_ARM64_PAGEBASE_REL21:
    // adrp with no addend.
    return (insn & 0x9F000000) == 0x90000000 && (insn & 0x60FFFFE0) == 0;
  case IMAGE_REL_ARM64_PAGEOFFSET_12L:
    // ldr x, [x, #0], with an unsigned offset.
    return (insn & 0xFFFFFC00) == 0xF9400000;
  default:
    return false;
  }
}

Defined *SectionChunk::getImportSiteTarget(const coff_relocation &rel) const {
  const auto &sites = file->symtab.ctx.importSites;
  if (sites.empty() || !sites.contains({this, rel.VirtualAddress}))
    return nullptr;
  Symbol *s = file->getSymbol(rel.SymbolTableIndex);
  if (auto *thunk = dyn_cast<DefinedImportThunk>(s))
    return thunk->wrappedSym;
  return cast<Defined>(cast<DefinedImportData>(s)->file->thunkSym);
}

bool SectionChunk::isArm64LocalImportPageRef(const coff_relocation &rel) const {
  ArrayRef<uint8_t> data = getContents();
  if (rel.VirtualAddress + 4 > data.size() || !canBypass(file, rel))
    return false;
  uint32_t insn = read32le(&data[rel.VirtualAddress]);
  switch (rel.Type) {
  case IMAGE_REL_ARM64_PAGEBASE_REL21:
    // adrp with no addend.
    return (insn & 0x9F000000) == 0x90000000 && (insn & 0x60FFFFE0) == 0;
  case IMAGE_REL_ARM64_PAGEOFFSET_12L:
    // ldr x, [x, #0], with an unsigned offset.
    return (insn & 0xFFFFFC00) == 0xF9400000;
  default:
    return false;
  }
}

// Rewrites the described load whose REL32 field is at off, `mov r64,
// [rip+d]` with a REX prefix, to materialise zero instead, in the same length:
// `mov r/m64, imm32` with the register moved from ModRM.reg to ModRM.rm.
static void rewriteZeroLoad(uint8_t *off) {
  uint8_t rex = off[-3], reg = (off[-1] >> 3) & 7;
  off[-3] = 0x48 | ((rex >> 2) & 1);
  off[-2] = 0xC7;
  off[-1] = 0xC0 | reg;
  write32le(off, 0);
}

// Rewrites the described call whose REL32 field is at off, `call [rip+d]`
// through a pointer that holds zero, to call zero in the same length:
// `xor r11d, r11d; call r11`. The call pushes the same return address and
// faults at the same address. R11 carries no argument to a callee that does
// not exist, and is the linker's scratch register at a call.
static void rewriteZeroCall(uint8_t *off) {
  static const uint8_t call[] = {0x45, 0x31, 0xDB, 0x41, 0xFF, 0xD3};
  memcpy(off - 2, call, sizeof(call));
}

// Rewrites the described instruction whose REL32 field is at off, a reference
// through the import pointer of a symbol in the image at s, to reach the
// symbol directly, keeping the instruction's length and the address of the
// instruction after it. A load becomes `lea` and a call `addr32 call rel32`.
// A jump becomes `jmp rel32` at its first byte, followed by int3: the Windows
// unwinder recognises an epilogue only by the instruction that ends it, and
// accepts no prefix but BND before a direct jump. Returns whether the field
// is left for the relocation to fill.
static bool rewriteLocalImportSite(uint8_t *off, LinkSiteForm form,
                                   uint64_t s, uint64_t p) {
  switch (form) {
  case LinkSiteLoad:
    off[-2] = 0x8D;
    return true;
  case LinkSiteCall:
    off[-2] = 0x67;
    off[-1] = 0xE8;
    return true;
  case LinkSiteJump:
  case LinkSiteJumpOnePrefix: {
    uint8_t *start = off - (form == LinkSiteJump ? 2 : 3);
    uint64_t end = p - (off - start) + 5;
    start[0] = 0xE9;
    write32le(start + 1, s - end);
    memset(start + 5, 0xCC, off + 4 - (start + 5));
    return false;
  }
  default:
    llvm_unreachable("not a rewritten form");
  }
}

const ImportSlot *
SectionChunk::getImportSlot(const coff_relocation &rel) const {
  if (!hasImportSlots)
    return nullptr;
  const std::vector<ImportSlot> &slots =
      file->symtab.ctx.importSlots.find(this)->second;
  auto it = llvm::partition_point(slots, [&](const ImportSlot &s) {
    return s.offset < rel.VirtualAddress;
  });
  if (it == slots.end() || it->offset != rel.VirtualAddress)
    return nullptr;
  return &*it;
}

void SectionChunk::applyRelocation(uint8_t *off,
                                   const coff_relocation &rel) const {
  // The loader writes an in-place import slot; until then it holds the value
  // of its lookup table entry, which the loader requires.
  if (const ImportSlot *slot = getImportSlot(rel)) {
    slot->lookup->writeTo(off);
    return;
  }

  auto *sym = dyn_cast_or_null<Defined>(file->getSymbol(rel.SymbolTableIndex));
  uint16_t type = rel.Type;

  // The address of an imported function that code takes becomes a load of
  // its import address table entry, and a load of a delay-loaded import's
  // entry the address of its thunk, so that code agrees with static data.
  // Each site was verified when imports were bound.
  if (Defined *target = getImportSiteTarget(rel)) {
    bool load = isa<DefinedImportData>(target);
    if (getArch() == Triple::aarch64) {
      uint32_t regs = read32le(off) & 0x3FF;
      if (type == IMAGE_REL_ARM64_PAGEOFFSET_12A) {
        write32le(off, 0xF9400000 | regs);
        type = IMAGE_REL_ARM64_PAGEOFFSET_12L;
      } else if (type == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
        write32le(off, 0x91000000 | regs);
        type = IMAGE_REL_ARM64_PAGEOFFSET_12A;
      }
    } else {
      off[-2] = load ? 0x8B : 0x8D;
    }
    sym = target;
  }

  // A described call, jump or pointer load through the import pointer of a
  // symbol in the image reaches the symbol directly. Its bytes were verified
  // when local imports were bound.
  // On ARM64, every adrp and ldr of a pointer that no other instruction reads
  // becomes adrp and add of its symbol; data still reads the pointer.
  std::optional<LinkSiteForm> rewrite;
  if (auto *li = dyn_cast_or_null<DefinedLocalImport>(sym)) {
    if (getArch() == Triple::aarch64) {
      if (li->getChunk()->bypassed &&
          (type == IMAGE_REL_ARM64_PAGEBASE_REL21 ||
           type == IMAGE_REL_ARM64_PAGEOFFSET_12L)) {
        // Zero is materialised by movz into the register each writes.
        if (isZeroPointer(li)) {
          write32le(off, 0xD2800000 | (read32le(off) & 0x1F));
          return;
        }
        sym = li->getTarget();
        if (type == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
          write32le(off, 0x91000000 | (read32le(off) & 0x3FF));
          type = IMAGE_REL_ARM64_PAGEOFFSET_12A;
        }
      }
    } else if ((rewrite = getLocalImportRewrite(rel))) {
      if (isZeroPointer(li)) {
        if (*rewrite == LinkSiteCall)
          rewriteZeroCall(off);
        else
          rewriteZeroLoad(off);
        return;
      }
      sym = li->getTarget();
    }
  }

  // Get the output section of the symbol for this relocation.  The output
  // section is needed to compute SECREL and SECTION relocations used in debug
  // info.
  Chunk *c = sym ? sym->getChunk() : nullptr;
  COFFLinkerContext &ctx = file->symtab.ctx;
  OutputSection *os = c ? ctx.getOutputSection(c) : nullptr;

  // Skip the relocation if it refers to a discarded section, and diagnose it
  // as an error if appropriate. If a symbol was discarded early, it may be
  // null. If it was discarded late, the output section will be null, unless
  // it was an absolute or synthetic symbol.
  if (!sym ||
      (!os && !isa<DefinedAbsolute>(sym) && !isa<DefinedSynthetic>(sym))) {
    maybeReportRelocationToDiscarded(this, sym, rel, ctx.config.mingw);
    return;
  }

  uint64_t s = sym->getRVA();

  // Compute the RVA of the relocation for relative relocations.
  uint64_t p = rva + rel.VirtualAddress;
  if (rewrite && !rewriteLocalImportSite(off, *rewrite, s, p))
    return;
  uint64_t imageBase = ctx.config.imageBase;
  switch (getArch()) {
  case Triple::x86_64:
    applyRelX64(off, rel.Type, os, s, p, imageBase);
    break;
  case Triple::x86:
    applyRelX86(off, rel.Type, os, s, p, imageBase);
    break;
  case Triple::thumb:
    applyRelARM(off, rel.Type, os, s, p, imageBase);
    break;
  case Triple::aarch64:
    applyRelARM64(off, type, os, s, p, imageBase);
    break;
  case Triple::mipsel:
    applyRelMIPS(off, rel.Type, os, s, p, imageBase);
    break;
  default:
    llvm_unreachable("unknown machine type");
  }
}

// Defend against unsorted relocations. This may be overly conservative.
void SectionChunk::sortRelocations() {
  auto cmpByVa = [](const coff_relocation &l, const coff_relocation &r) {
    return l.VirtualAddress < r.VirtualAddress;
  };
  if (llvm::is_sorted(getRelocs(), cmpByVa))
    return;
  warn("some relocations in " + file->getName() + " are not sorted");
  MutableArrayRef<coff_relocation> newRelocs(
      bAlloc().Allocate<coff_relocation>(relocsSize), relocsSize);
  memcpy(newRelocs.data(), relocsData, relocsSize * sizeof(coff_relocation));
  llvm::sort(newRelocs, cmpByVa);
  setRelocs(newRelocs);
}

// Similar to writeTo, but suitable for relocating a subsection of the overall
// section.
void SectionChunk::writeAndRelocateSubsection(ArrayRef<uint8_t> sec,
                                              ArrayRef<uint8_t> subsec,
                                              uint32_t &nextRelocIndex,
                                              uint8_t *buf) const {
  assert(!subsec.empty() && !sec.empty());
  assert(sec.begin() <= subsec.begin() && subsec.end() <= sec.end() &&
         "subsection is not part of this section");
  size_t vaBegin = std::distance(sec.begin(), subsec.begin());
  size_t vaEnd = std::distance(sec.begin(), subsec.end());
  memcpy(buf, subsec.data(), subsec.size());
  for (; nextRelocIndex < relocsSize; ++nextRelocIndex) {
    const coff_relocation &rel = relocsData[nextRelocIndex];
    // Only apply relocations that apply to this subsection. These checks
    // assume that all subsections completely contain their relocations.
    // Relocations must not straddle the beginning or end of a subsection.
    if (rel.VirtualAddress < vaBegin)
      continue;
    if (rel.VirtualAddress + 1 >= vaEnd)
      break;
    applyRelocation(&buf[rel.VirtualAddress - vaBegin], rel);
  }
}

void SectionChunk::addAssociative(SectionChunk *child) {
  // Insert the child section into the list of associated children. Keep the
  // list ordered by section name so that ICF does not depend on section order.
  assert(child->assocChildren == nullptr &&
         "associated sections cannot have their own associated children");
  SectionChunk *prev = this;
  SectionChunk *next = assocChildren;
  for (; next != nullptr; prev = next, next = next->assocChildren) {
    if (next->getSectionName() <= child->getSectionName())
      break;
  }

  // Insert child between prev and next.
  assert(prev->assocChildren == next);
  prev->assocChildren = child;
  child->assocChildren = next;
}

static uint8_t getBaserelType(const coff_relocation &rel,
                              Triple::ArchType arch) {
  switch (arch) {
  case Triple::x86_64:
    if (rel.Type == IMAGE_REL_AMD64_ADDR64)
      return IMAGE_REL_BASED_DIR64;
    if (rel.Type == IMAGE_REL_AMD64_ADDR32)
      return IMAGE_REL_BASED_HIGHLOW;
    return IMAGE_REL_BASED_ABSOLUTE;
  case Triple::x86:
    if (rel.Type == IMAGE_REL_I386_DIR32)
      return IMAGE_REL_BASED_HIGHLOW;
    return IMAGE_REL_BASED_ABSOLUTE;
  case Triple::thumb:
    if (rel.Type == IMAGE_REL_ARM_ADDR32)
      return IMAGE_REL_BASED_HIGHLOW;
    if (rel.Type == IMAGE_REL_ARM_MOV32T)
      return IMAGE_REL_BASED_ARM_MOV32T;
    return IMAGE_REL_BASED_ABSOLUTE;
  case Triple::aarch64:
    if (rel.Type == IMAGE_REL_ARM64_ADDR64)
      return IMAGE_REL_BASED_DIR64;
    return IMAGE_REL_BASED_ABSOLUTE;
  case Triple::mipsel:
    return IMAGE_REL_BASED_ABSOLUTE;
  default:
    llvm_unreachable("unknown machine type");
  }
}

bool SectionChunk::isAddressWord(const coff_relocation &rel) const {
  return getBaserelType(rel, getArch()) == (file->symtab.ctx.config.is64()
                                                ? IMAGE_REL_BASED_DIR64
                                                : IMAGE_REL_BASED_HIGHLOW);
}

// Windows-specific.
// Collect all locations that contain absolute addresses, which need to be
// fixed by the loader if load-time relocation is needed.
// Only called when base relocation is enabled.
void SectionChunk::getBaserels(std::vector<Baserel> *res) {
  for (const coff_relocation &rel : getRelocs()) {
    uint8_t ty = getBaserelType(rel, getArch());
    if (ty == IMAGE_REL_BASED_ABSOLUTE)
      continue;
    Symbol *target = file->getSymbol(rel.SymbolTableIndex);
    if (!isa_and_nonnull<Defined>(target) || isa<DefinedAbsolute>(target))
      continue;
    // The loader writes an in-place import slot as an absolute address.
    if (getImportSlot(rel))
      continue;
    res->emplace_back(rva + rel.VirtualAddress, ty);
  }

  // Insert a 64-bit relocation for CHPEMetadataPointer in the native load
  // config of a hybrid ARM64X image. Its value will be set in prepareLoadConfig
  // to match the value in the EC load config, which is expected to be
  // a relocatable pointer to the __chpe_metadata symbol.
  COFFLinkerContext &ctx = file->symtab.ctx;
  if (ctx.config.machine == ARM64X && ctx.hybridSymtab->loadConfigSym &&
      ctx.hybridSymtab->loadConfigSym->getChunk() == this &&
      ctx.symtab.loadConfigSym &&
      ctx.hybridSymtab->loadConfigSize >=
          offsetof(coff_load_configuration64, CHPEMetadataPointer) +
              sizeof(coff_load_configuration64::CHPEMetadataPointer))
    res->emplace_back(
        ctx.hybridSymtab->loadConfigSym->getRVA() +
            offsetof(coff_load_configuration64, CHPEMetadataPointer),
        IMAGE_REL_BASED_DIR64);
}

// MinGW specific.
// Check whether a static relocation of type Type can be deferred and
// handled at runtime as a pseudo relocation (for references to a module
// local variable, which turned out to actually need to be imported from
// another DLL) This returns the size the relocation is supposed to update,
// in bits, or 0 if the relocation cannot be handled as a runtime pseudo
// relocation.
static int getRuntimePseudoRelocSize(uint16_t type, Triple::ArchType arch) {
  // Relocations that either contain an absolute address, or a plain
  // relative offset, since the runtime pseudo reloc implementation
  // adds 8/16/32/64 bit values to a memory address.
  //
  // Given a pseudo relocation entry,
  //
  // typedef struct {
  //   DWORD sym;
  //   DWORD target;
  //   DWORD flags;
  // } runtime_pseudo_reloc_item_v2;
  //
  // the runtime relocation performs this adjustment:
  //     *(base + .target) += *(base + .sym) - (base + .sym)
  //
  // This works for both absolute addresses (IMAGE_REL_*_ADDR32/64,
  // IMAGE_REL_I386_DIR32, where the memory location initially contains
  // the address of the IAT slot, and for relative addresses (IMAGE_REL*_REL32),
  // where the memory location originally contains the relative offset to the
  // IAT slot.
  //
  // This requires the target address to be writable, either directly out of
  // the image, or temporarily changed at runtime with VirtualProtect.
  // Since this only operates on direct address values, it doesn't work for
  // ARM/ARM64 relocations, other than the plain ADDR32/ADDR64 relocations.
  switch (arch) {
  case Triple::x86_64:
    switch (type) {
    case IMAGE_REL_AMD64_ADDR64:
      return 64;
    case IMAGE_REL_AMD64_ADDR32:
    case IMAGE_REL_AMD64_REL32:
    case IMAGE_REL_AMD64_REL32_1:
    case IMAGE_REL_AMD64_REL32_2:
    case IMAGE_REL_AMD64_REL32_3:
    case IMAGE_REL_AMD64_REL32_4:
    case IMAGE_REL_AMD64_REL32_5:
      return 32;
    default:
      return 0;
    }
  case Triple::x86:
    switch (type) {
    case IMAGE_REL_I386_DIR32:
    case IMAGE_REL_I386_REL32:
      return 32;
    default:
      return 0;
    }
  case Triple::thumb:
    switch (type) {
    case IMAGE_REL_ARM_ADDR32:
      return 32;
    default:
      return 0;
    }
  case Triple::aarch64:
    switch (type) {
    case IMAGE_REL_ARM64_ADDR64:
      return 64;
    case IMAGE_REL_ARM64_ADDR32:
      return 32;
    default:
      return 0;
    }
  default:
    llvm_unreachable("unknown machine type");
  }
}

// MinGW specific.
// Append information to the provided vector about all relocations that
// need to be handled at runtime as runtime pseudo relocations (references
// to a module local variable, which turned out to actually need to be
// imported from another DLL).
void SectionChunk::getRuntimePseudoRelocs(
    std::vector<RuntimePseudoReloc> &res) {
  for (const coff_relocation &rel : getRelocs()) {
    auto *target =
        dyn_cast_or_null<Defined>(file->getSymbol(rel.SymbolTableIndex));
    if (!target || !target->isRuntimePseudoReloc)
      continue;
    // If the target doesn't have a chunk allocated, it may be a
    // DefinedImportData symbol which ended up unnecessary after GC.
    // Normally we wouldn't eliminate section chunks that are referenced, but
    // references within DWARF sections don't count for keeping section chunks
    // alive. Thus such dangling references in DWARF sections are expected.
    if (!target->getChunk())
      continue;
    int sizeInBits = getRuntimePseudoRelocSize(rel.Type, getArch());
    if (sizeInBits == 0) {
      error("unable to automatically import from " + target->getName() +
            " with relocation type " +
            file->getCOFFObj()->getRelocationTypeName(rel.Type) + " in " +
            toString(file));
      continue;
    }
    int addressSizeInBits = file->symtab.ctx.config.is64() ? 64 : 32;
    if (sizeInBits < addressSizeInBits) {
      warn("runtime pseudo relocation in " + toString(file) + " against " +
           "symbol " + target->getName() + " is too narrow (only " +
           Twine(sizeInBits) + " bits wide); this can fail at runtime " +
           "depending on memory layout");
    }
    // sizeInBits is used to initialize the Flags field; currently no
    // other flags are defined.
    res.emplace_back(target, this, rel.VirtualAddress, sizeInBits);
  }
}

bool SectionChunk::isCOMDAT() const {
  return header->Characteristics & IMAGE_SCN_LNK_COMDAT;
}

void SectionChunk::printDiscardedMessage() const {
  // Removed by dead-stripping. If it's removed by ICF, ICF already
  // printed out the name, so don't repeat that here.
  if (sym && this == repl)
    log("Discarded " + sym->getName());
}

StringRef SectionChunk::getDebugName() const {
  if (sym)
    return sym->getName();
  return "";
}

ArrayRef<uint8_t> SectionChunk::getContents() const {
  ArrayRef<uint8_t> a;
  cantFail(file->getCOFFObj()->getSectionContents(header, a));
  return a;
}

ArrayRef<uint8_t> SectionChunk::consumeDebugMagic() {
  assert(isCodeView());
  return consumeDebugMagic(getContents(), getSectionName());
}

ArrayRef<uint8_t> SectionChunk::consumeDebugMagic(ArrayRef<uint8_t> data,
                                                  StringRef sectionName) {
  if (data.empty())
    return {};

  // First 4 bytes are section magic.
  if (data.size() < 4)
    fatal("the section is too short: " + sectionName);

  if (!sectionName.starts_with(".debug$"))
    fatal("invalid section: " + sectionName);

  uint32_t magic = support::endian::read32le(data.data());
  uint32_t expectedMagic = sectionName == ".debug$H"
                               ? DEBUG_HASHES_SECTION_MAGIC
                               : DEBUG_SECTION_MAGIC;
  if (magic != expectedMagic) {
    warn("ignoring section " + sectionName + " with unrecognized magic 0x" +
         utohexstr(magic));
    return {};
  }
  return data.slice(4);
}

SectionChunk *SectionChunk::findByName(ArrayRef<SectionChunk *> sections,
                                       StringRef name) {
  for (SectionChunk *c : sections)
    if (c->getSectionName() == name)
      return c;
  return nullptr;
}

void SectionChunk::replace(SectionChunk *other) {
  p2Align = std::max(p2Align, other->p2Align);
  other->repl = repl;
  other->live = false;
}

uint32_t SectionChunk::getSectionNumber() const {
  DataRefImpl r;
  r.p = reinterpret_cast<uintptr_t>(header);
  SectionRef s(r, file->getCOFFObj());
  return s.getIndex() + 1;
}

CommonChunk::CommonChunk(const COFFSymbolRef s) : live(false), sym(s) {
  // The value of a common symbol is its size. Align all common symbols smaller
  // than 32 bytes naturally, i.e. round the size up to the next power of two.
  // This is what MSVC link.exe does.
  setAlignment(std::min(32U, uint32_t(PowerOf2Ceil(sym.getValue()))));
  hasData = false;
}

uint32_t CommonChunk::getOutputCharacteristics() const {
  return IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ |
         IMAGE_SCN_MEM_WRITE;
}

void StringChunk::writeTo(uint8_t *buf) const {
  memcpy(buf, str.data(), str.size());
  buf[str.size()] = '\0';
}

ImportThunkChunk::ImportThunkChunk(COFFLinkerContext &ctx, Defined *s)
    : NonSectionCodeChunk(ImportThunkKind), live(!ctx.config.doGC),
      impSymbol(s), ctx(ctx) {}

ImportThunkChunkX64::ImportThunkChunkX64(COFFLinkerContext &ctx, Defined *s)
    : ImportThunkChunk(ctx, s) {
  // Intel Optimization Manual says that all branch targets
  // should be 16-byte aligned. MSVC linker does this too.
  setAlignment(16);
}

void ImportThunkChunkX64::writeTo(uint8_t *buf) const {
  memcpy(buf, importThunkX86, sizeof(importThunkX86));
  // The first two bytes is a JMP instruction. Fill its operand.
  write32le(buf + 2, impSymbol->getRVA() - rva - getSize());
}

void ImportThunkChunkX86::getBaserels(std::vector<Baserel> *res) {
  res->emplace_back(getRVA() + 2, ctx.config.machine);
}

void ImportThunkChunkX86::writeTo(uint8_t *buf) const {
  memcpy(buf, importThunkX86, sizeof(importThunkX86));
  // The first two bytes is a JMP instruction. Fill its operand.
  write32le(buf + 2, impSymbol->getRVA() + ctx.config.imageBase);
}

void ImportThunkChunkARM::getBaserels(std::vector<Baserel> *res) {
  res->emplace_back(getRVA(), IMAGE_REL_BASED_ARM_MOV32T);
}

void ImportThunkChunkARM::writeTo(uint8_t *buf) const {
  memcpy(buf, importThunkARM, sizeof(importThunkARM));
  // Fix mov.w and mov.t operands.
  applyMOV32T(buf, impSymbol->getRVA() + ctx.config.imageBase);
}

void ImportThunkChunkARM64::writeTo(uint8_t *buf) const {
  int64_t off = impSymbol->getRVA() & 0xfff;
  memcpy(buf, importThunkARM64, sizeof(importThunkARM64));
  applyArm64Addr(buf, impSymbol->getRVA(), rva, 12);
  applyArm64Ldr(buf + 4, off);
}

// A Thumb2, PIC, non-interworking range extension thunk.
const uint8_t armThunk[] = {
    0x40, 0xf2, 0x00, 0x0c, // P:  movw ip,:lower16:S - (P + (L1-P) + 4)
    0xc0, 0xf2, 0x00, 0x0c, //     movt ip,:upper16:S - (P + (L1-P) + 4)
    0xe7, 0x44,             // L1: add  pc, ip
};

size_t RangeExtensionThunkARM::getSize() const {
  assert(ctx.config.machine == ARMNT);
  (void)&ctx;
  return sizeof(armThunk);
}

void RangeExtensionThunkARM::writeTo(uint8_t *buf) const {
  assert(ctx.config.machine == ARMNT);
  uint64_t offset = target->getRVA() - rva - 12;
  memcpy(buf, armThunk, sizeof(armThunk));
  applyMOV32T(buf, uint32_t(offset));
}

// A position independent ARM64 adrp+add thunk, with a maximum range of
// +/- 4 GB, which is enough for any PE-COFF.
const uint8_t arm64Thunk[] = {
    0x10, 0x00, 0x00, 0x90, // adrp x16, Dest
    0x10, 0x02, 0x00, 0x91, // add  x16, x16, :lo12:Dest
    0x00, 0x02, 0x1f, 0xd6, // br   x16
};

size_t RangeExtensionThunkARM64::getSize() const { return sizeof(arm64Thunk); }

void RangeExtensionThunkARM64::writeTo(uint8_t *buf) const {
  memcpy(buf, arm64Thunk, sizeof(arm64Thunk));
  applyArm64Addr(buf + 0, target->getRVA(), rva, 12);
  applyArm64Imm(buf + 4, target->getRVA() & 0xfff, 0);
}

void SameAddressThunkARM64EC::setDynamicRelocs(COFFLinkerContext &ctx) const {
  // Add ARM64X relocations replacing adrp/add instructions with a version using
  // the hybrid target.
  RangeExtensionThunkARM64 hybridView(ARM64EC, hybridTarget);
  uint8_t buf[sizeof(arm64Thunk)];
  hybridView.setRVA(rva);
  hybridView.writeTo(buf);
  uint32_t addrp = *reinterpret_cast<ulittle32_t *>(buf);
  uint32_t add = *reinterpret_cast<ulittle32_t *>(buf + sizeof(uint32_t));
  ctx.dynamicRelocs->set(this, addrp);
  ctx.dynamicRelocs->set(Arm64XRelocVal(this, sizeof(uint32_t)), add);
}

LocalImportChunk::LocalImportChunk(COFFLinkerContext &c, Defined *s)
    : sym(s), ctx(c) {
  setAlignment(ctx.config.wordsize);
}

// An absolute address does not move with the image.
void LocalImportChunk::getBaserels(std::vector<Baserel> *res) {
  if (!isa<DefinedAbsolute>(sym))
    res->emplace_back(getRVA(), ctx.config.machine);
}

size_t LocalImportChunk::getSize() const { return ctx.config.wordsize; }

KCFIOpenChunk::KCFIOpenChunk(COFFLinkerContext &ctx, Defined *list,
                             Defined *scanner, bool dynamic, Defined *outside,
                             bool check)
    : list(list), scanner(scanner), dynamic(dynamic), outside(outside),
      check(check), ctx(ctx) {
  setAlignment(ctx.config.machine == ARM64 ? 4 : 1);
}

size_t KCFIOpenChunk::getSize() const {
  if (ctx.config.machine == ARM64)
    return (outside ? 36 : 0) + (list ? 8 : 0) + 12;
  return (outside ? 32 : 0) + (list ? 7 : 0) + (dynamic ? 6 : 5);
}

MachineTypes KCFIOpenChunk::getMachine() const { return ctx.config.machine; }

ResidualFillChunk::ResidualFillChunk(COFFLinkerContext &ctx,
                                     std::vector<ResidualWord> words,
                                     Chunk *sealed, DefinedImportData *protect)
    : words(std::move(words)), sealed(sealed), protect(protect), ctx(ctx) {
  // Control Flow Guard checks the initializer table's call to it.
  setAlignment(16);
}

MachineTypes ResidualFillChunk::getMachine() const {
  return ctx.config.machine;
}

size_t ResidualFillChunk::getPrologueSize() const {
  if (!protect)
    return 0;
  return ctx.config.machine == ARM64 ? 8 : 4;
}

// The instructions that add an addend to the import's address.
static size_t getArm64AddendSize(int64_t addend) {
  if (addend > -4096 && addend < 4096)
    return 4;
  size_t n = 0;
  for (int shift = 0; shift != 64; shift += 16)
    n += (uint64_t(addend) >> shift & 0xFFFF) != 0;
  return 4 * std::max<size_t>(n, 1) + 4;
}

size_t ResidualFillChunk::getWordSize(const ResidualWord &w) const {
  if (ctx.config.machine == ARM64)
    return 8 + getArm64AddendSize(w.addend) + 12;
  return 7 + (isInt<32>(w.addend) ? 6 : 13) + 7;
}

size_t ResidualFillChunk::getSize() const {
  size_t size = getPrologueSize();
  for (const ResidualWord &w : words)
    size += getWordSize(w);
  if (ctx.config.machine == ARM64)
    return size + (protect ? 56 + 8 : 0) + 8;
  return size + (protect ? 60 + 4 : 0) + 3;
}

void ResidualFillChunk::writeTo(uint8_t *buf) const {
  const uint32_t pageReadOnly = 2;
  uint64_t sealStart = 0, sealSize = 0;
  if (protect) {
    OutputSection *os = ctx.getOutputSection(sealed);
    sealStart = os->getRVA();
    sealSize = os->getVirtualSize();
  }
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    // adrp xN, target; ldr xN, [xN, :lo12:target]
    auto load = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0xF9400000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Ldr(buf + off + 4, target & 0xfff);
      off += 8;
    };
    // adrp xN, target; add xN, xN, :lo12:target
    auto address = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0x91000000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
      off += 8;
    };
    if (protect) {
      put(0xA9BD7BFD); // stp x29, x30, [sp, #-48]!
      put(0x910003FD); // mov x29, sp
    }
    for (const ResidualWord &w : words) {
      load(16, w.imp->getRVA());
      if (w.addend > 0 && w.addend < 4096) {
        put(0x91000210 | uint32_t(w.addend) << 10); // add x16, x16, #addend
      } else if (w.addend < 0 && w.addend > -4096) {
        put(0xD1000210 | uint32_t(-w.addend) << 10); // sub x16, x16, #-addend
      } else {
        // movz x17, #h0; movk x17, #hN, lsl #16N; add x16, x16, x17
        bool first = true;
        for (uint32_t hw = 0; hw != 4; ++hw) {
          uint32_t half = uint64_t(w.addend) >> (16 * hw) & 0xFFFF;
          if (!half && !(first && hw == 3))
            continue;
          put((first ? 0xD2800011 : 0xF2800011) | hw << 21 | half << 5);
          first = false;
        }
        put(0x8B110210);
      }
      address(17, w.chunk->getRVA() + w.offset);
      put(0xF9000230); // str x16, [x17]
    }
    if (protect) {
      // NtProtectVirtualMemory(-1, &base, &size, PAGE_READONLY, &old), with
      // base, size and old at sp + 16, 24 and 32.
      address(0, sealStart);
      put(0xF9000BE0);                                    // str x0, [sp, #16]
      put(0xD2800001 | uint32_t(sealSize & 0xFFFF) << 5); // movz x1, #lo
      put(0xF2A00001 | uint32_t(sealSize >> 16 & 0xFFFF) << 5); // movk x1, #hi
      put(0xF9000FE1);                     // str x1, [sp, #24]
      put(0x92800000);                     // mov x0, #-1
      put(0x910043E1);                     // add x1, sp, #16
      put(0x910063E2);                     // add x2, sp, #24
      put(0x52800003 | pageReadOnly << 5); // mov w3, #PAGE_READONLY
      put(0x910083E4);                     // add x4, sp, #32
      load(16, protect->getRVA());
      put(0xD63F0200); // blr x16
    }
    put(0x52800000); // mov w0, #0
    if (protect) {
      put(0x910003BF); // mov sp, x29
      put(0xA8C37BFD); // ldp x29, x30, [sp], #48
    }
    put(0xD65F03C0); // ret
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  // The RIP-relative displacement to target of the instruction just put,
  // which ends with it.
  auto disp = [&](uint64_t target) {
    write32le(buf + off - 4, target - (rva + off));
  };
  if (protect)
    put({0x48, 0x83, 0xEC, 0x48}); // sub rsp, 0x48
  for (const ResidualWord &w : words) {
    put({0x48, 0x8B, 0x05, 0, 0, 0, 0}); // mov rax, [rip + imp]
    disp(w.imp->getRVA());
    if (isInt<32>(w.addend)) {
      put({0x48, 0x05, 0, 0, 0, 0}); // add rax, addend
      write32le(buf + off - 4, uint32_t(w.addend));
    } else {
      put({0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0}); // mov rcx, addend
      write64le(buf + off - 8, uint64_t(w.addend));
      put({0x48, 0x01, 0xC8}); // add rax, rcx
    }
    put({0x48, 0x89, 0x05, 0, 0, 0, 0}); // mov [rip + word], rax
    disp(w.chunk->getRVA() + w.offset);
  }
  if (protect) {
    // NtProtectVirtualMemory(-1, &base, &size, PAGE_READONLY, &old), with
    // base, size and old at rsp + 0x30, 0x38 and 0x40.
    put({0x48, 0x8D, 0x05, 0, 0, 0, 0}); // lea rax, [rip + base]
    disp(sealStart);
    put({0x48, 0x89, 0x44, 0x24, 0x30});             // mov [rsp + 0x30], rax
    put({0x48, 0xC7, 0x44, 0x24, 0x38, 0, 0, 0, 0}); // mov [rsp + 0x38], size
    write32le(buf + off - 4, uint32_t(sealSize));
    put({0x48, 0x8D, 0x44, 0x24, 0x40});               // lea rax, [rsp + 0x40]
    put({0x48, 0x89, 0x44, 0x24, 0x20});               // mov [rsp + 0x20], rax
    put({0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF});   // mov rcx, -1
    put({0x48, 0x8D, 0x54, 0x24, 0x30});               // lea rdx, [rsp + 0x30]
    put({0x4C, 0x8D, 0x44, 0x24, 0x38});               // lea r8, [rsp + 0x38]
    put({0x41, 0xB9, uint8_t(pageReadOnly), 0, 0, 0}); // mov r9d, PAGE_READONLY
    put({0xFF, 0x15, 0, 0, 0, 0}); // call [rip + NtProtectVirtualMemory]
    disp(protect->getRVA());
  }
  put({0x31, 0xC0}); // xor eax, eax
  if (protect)
    put({0x48, 0x83, 0xC4, 0x48}); // add rsp, 0x48
  put({0xC3});                     // ret
}

void ResidualFillUnwindChunk::writeTo(uint8_t *buf) const {
  if (ctx.config.machine == ARM64) {
    // One epilog at the end, which the prologue's codes describe: set_fp,
    // save_fplr_x of 48 bytes, end.
    write32le(buf, fill->getSize() / 4 | 1 << 21 | 1 << 27);
    const uint8_t codes[] = {0xE1, 0x85, 0xE4, 0xE4};
    memcpy(buf + 4, codes, sizeof(codes));
    return;
  }
  // Version 1, a prologue of 4 bytes with one code: UWOP_ALLOC_SMALL of
  // 0x48 bytes at its end.
  const uint8_t info[] = {0x01, 0x04, 0x01, 0x00, 0x04, 0x82, 0x00, 0x00};
  memcpy(buf, info, sizeof(info));
}

size_t ResidualFillPdataChunk::getSize() const {
  return ctx.config.machine == ARM64 ? 8 : 12;
}

void ResidualFillPdataChunk::writeTo(uint8_t *buf) const {
  write32le(buf, fill->getRVA());
  if (ctx.config.machine == ARM64) {
    write32le(buf + 4, unwind->getRVA());
    return;
  }
  write32le(buf + 4, fill->getRVA() + fill->getSize());
  write32le(buf + 8, unwind->getRVA());
}

void KCFIOpenChunk::writeTo(uint8_t *buf) const {
  // The scanner reads the list from its first word after the head.
  uint64_t first = list ? list->getRVA() + 8 : 0;
  // The image ends where its last section does, rounded up to the page that
  // the loader maps it in.
  uint64_t end = 0;
  if (outside) {
    for (OutputSection *sec : ctx.outputSections)
      end = std::max(end, sec->getRVA() + sec->getVirtualSize());
    end = alignTo(end, 4096);
  }
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    // Each address is reached at any distance, since the linker adds no range
    // extension thunk for a chunk of its own.
    auto setAddr = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);                // adrp
      write32le(buf + off + 4, 0x91000000 | reg << 5 | reg); // add
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
      off += 8;
    };
    auto branch = [&](uint64_t target) {
      setAddr(17, target);
      write32le(buf + off, 0xD61F0220); // br x17
      off += 4;
    };
    if (outside) {
      // adrp x17, start; cmp x15, x17; b.lo 1f
      // adrp x17, end; cmp x15, x17; b.hs 1f
      uint32_t tail = getSize() - 12;
      write32le(buf, 0x90000011);
      write32le(buf + 4, 0xEB1101FF);
      write32le(buf + 8, 0x54000003 | (tail - 8) / 4 << 5);
      write32le(buf + 12, 0x90000011);
      write32le(buf + 16, 0xEB1101FF);
      write32le(buf + 20, 0x54000002 | (tail - 20) / 4 << 5);
      applyArm64Addr(buf, 0, rva, 12);
      applyArm64Addr(buf + 12, end, rva + 12, 12);
      off = 24;
    }
    // adrp x16, first; add x16, x16, :lo12:first
    if (list)
      setAddr(16, first);
    // adrp x17, scanner; add x17, x17, :lo12:scanner; br x17
    branch(scanner->getRVA());
    // 1: adrp x17, outside; add x17, x17, :lo12:outside; br x17
    if (outside)
      branch(outside->getRVA());
    return;
  }
  if (outside) {
    static const uint8_t bound[] = {
        0x4C, 0x8D, 0x1D, 0, 0, 0, 0, // lea r11, [rip + start]
        0x4C, 0x39, 0xD8,             // cmp rax, r11
        0x0F, 0x82, 0,    0, 0, 0,    // jb outside
        0x4C, 0x8D, 0x1D, 0, 0, 0, 0, // lea r11, [rip + end]
        0x4C, 0x39, 0xD8,             // cmp rax, r11
        0x0F, 0x83, 0,    0, 0, 0,    // jae outside
    };
    memcpy(buf, bound, sizeof(bound));
    if (check)
      buf[9] = buf[25] = 0xD9; // cmp rcx, r11
    write32le(buf + 3, -(rva + 7));
    write32le(buf + 12, outside->getRVA() - (rva + 16));
    write32le(buf + 19, end - (rva + 23));
    write32le(buf + 28, outside->getRVA() - (rva + 32));
    off = sizeof(bound);
  }
  // lea r10, [rip + first]; jmp scanner; int3
  if (list) {
    static const uint8_t lea[] = {0x4C, 0x8D, 0x15, 0, 0, 0, 0};
    memcpy(buf + off, lea, sizeof(lea));
    write32le(buf + off + 3, first - (rva + off + 7));
    off += 7;
  }
  buf[off] = 0xE9;
  write32le(buf + off + 1, scanner->getRVA() - (rva + off + 5));
  if (dynamic)
    buf[off + 5] = 0xCC;
}

KCFIThunkChunk::KCFIThunkChunk(COFFLinkerContext &ctx, bool check,
                               ArrayRef<uint8_t> compare,
                               ArrayRef<uint8_t> pageTest, Symbol *mismatch,
                               Symbol *guard, Defined *codeStart,
                               Defined *codeEnd, ArrayRef<Range> imported)
    : check(check), compare(compare), pageTest(pageTest), mismatch(mismatch),
      guard(guard), codeStart(codeStart), codeEnd(codeEnd), imported(imported),
      ctx(ctx) {
  setAlignment(16);
}

MachineTypes KCFIThunkChunk::getMachine() const { return ctx.config.machine; }

// With the image's own range tested, the thunk is laid out as: the range test,
// the type check and jump that a target inside a range takes, the tests of the
// imported ranges, and the path of a target outside every range. Without it,
// the type check and jump follow that path instead, so that no branch skips
// them. On ARM64 the branch to the mismatch routine, which may be far, ends
// the thunk.
KCFIThunkChunk::Layout KCFIThunkChunk::getLayout() const {
  bool isARM64 = ctx.config.machine == ARM64;
  bool own = codeStart;
  bool hits = own || !imported.empty();
  uint32_t ownSize = own ? (isARM64 ? 28 : 22) : 0;
  uint32_t hitSize = 0;
  if (hits)
    hitSize = compare.size() + (isARM64 ? 8 : 6 + (check ? 1 : 2));
  uint32_t importedSize = imported.size() * (isARM64 ? 28 : 18);
  uint32_t outsideSize =
      pageTest.size() + compare.size() + (isARM64 ? 4 + 4 + 12 : 6 + 6 + 6);
  Layout l;
  l.own = 0;
  if (own) {
    l.hit = ownSize;
    l.imported = l.hit + hitSize;
    l.outside = l.imported + importedSize;
    l.mismatch = l.outside + outsideSize;
  } else {
    l.imported = 0;
    l.outside = importedSize;
    l.hit = l.outside + outsideSize;
    l.mismatch = l.hit + hitSize;
  }
  l.size = l.mismatch + (isARM64 ? 12 : 0);
  return l;
}

size_t KCFIThunkChunk::getSize() const { return getLayout().size; }

void KCFIThunkChunk::writeTo(uint8_t *buf) const {
  Layout l = getLayout();
  bool hits = codeStart || !imported.empty();
  uint64_t mismatchRVA = cast<Defined>(mismatch)->getRVA();
  uint64_t guardRVA = cast<Defined>(guard)->getRVA();
  uint64_t codeSize = codeStart ? codeEnd->getRVA() - codeStart->getRVA() : 0;
  // Both forms compare the size as a sign-extended 32-bit immediate.
  if (codeSize > INT32_MAX)
    Err(ctx) << "the code range of " << ctx.config.outputFile
             << " is too large for its KCFI thunks";

  if (ctx.config.machine == ARM64) {
    // The target is in X15; X16 and X17 are free, as for the guard function.
    auto insn = [&](uint32_t off, uint32_t v) { write32le(buf + off, v); };
    auto branch19 = [&](uint32_t off, uint32_t cond, uint32_t to) {
      int32_t delta = (int32_t(to) - int32_t(off)) / 4;
      insn(off, 0x54000000 | cond | (delta & 0x7FFFF) << 5);
    };
    auto adrpAdd = [&](uint32_t off, uint32_t reg, uint64_t target) {
      insn(off, 0x90000000 | reg);                // adrp reg, target
      insn(off + 4, 0x91000000 | reg << 5 | reg); // add reg, reg, :lo12:target
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
    };
    if (codeStart) {
      // adrp x16, start; add x16, x16, :lo12:start; sub x16, x15, x16
      // mov x17, #size; movk x17, #size, lsl #16; cmp x16, x17; b.hs outside
      adrpAdd(l.own, 16, codeStart->getRVA());
      insn(l.own + 8, 0xCB1001F0);
      insn(l.own + 12, 0xD2800011 | (codeSize & 0xFFFF) << 5);
      insn(l.own + 16, 0xF2A00011 | (codeSize >> 16) << 5);
      insn(l.own + 20, 0xEB11021F);
      branch19(l.own + 24, 2, l.imported);
    }
    if (hits) {
      // The type check; b.ne mismatch; ret
      memcpy(buf + l.hit, compare.data(), compare.size());
      uint32_t off = l.hit + compare.size();
      branch19(off, 1, l.mismatch);
      insn(off + 4, 0xD65F03C0);
    }
    for (size_t i = 0; i != imported.size(); ++i) {
      // adrp x16, start; add x16, x16, :lo12:start; ldp x16, x17, [x16]
      // cmp x15, x16; b.lo 1f; cmp x15, x17; b.lo hit; 1:
      uint32_t off = l.imported + i * 28;
      adrpAdd(off, 16, imported[i].start->getRVA());
      insn(off + 8, 0xA9404610);
      insn(off + 12, 0xEB1001FF);
      branch19(off + 16, 3, off + 28);
      insn(off + 20, 0xEB1101FF);
      branch19(off + 24, 3, l.hit);
    }
    // The page test; b.eq mismatch; the type check; b.ne mismatch
    // adrp x16, guard; ldr x16, [x16, :lo12:guard]; br x16
    uint32_t off = l.outside;
    memcpy(buf + off, pageTest.data(), pageTest.size());
    off += pageTest.size();
    branch19(off, 0, l.mismatch);
    off += 4;
    memcpy(buf + off, compare.data(), compare.size());
    off += compare.size();
    branch19(off, 1, l.mismatch);
    insn(off + 4, 0x90000010);
    insn(off + 8, 0xF9400210);
    insn(off + 12, 0xD61F0200);
    applyArm64Addr(buf + off + 4, guardRVA, rva + off + 4, 12);
    applyArm64Ldr(buf + off + 8, guardRVA & 0xfff);
    // adrp x17, mismatch; add x17, x17, :lo12:mismatch; br x17
    adrpAdd(l.mismatch, 17, mismatchRVA);
    insn(l.mismatch + 8, 0xD61F0220);
    return;
  }

  // The target is in RAX for a dispatch thunk and in RCX for a check thunk;
  // R10 and R11 are free, as for the guard function.
  uint8_t modrm = check ? 0x0D : 0x05;
  auto rel32 = [&](uint32_t off, uint64_t target) {
    write32le(buf + off, target - (rva + off + 4));
  };
  auto rel8 = [&](uint32_t off, uint32_t target) {
    buf[off] = target - (off + 1);
  };
  if (codeStart) {
    static const uint8_t ownTest[] = {
        0x4C, 0x8D, 0x15, 0, 0, 0, 0, // lea r10, [rip + start]
        0x49, 0x89, 0xC3,             // mov r11, rax
        0x4D, 0x29, 0xD3,             // sub r11, r10
        0x49, 0x81, 0xFB, 0, 0, 0, 0, // cmp r11, size
        0x73, 0,                      // jae imported
    };
    memcpy(buf + l.own, ownTest, sizeof(ownTest));
    rel32(l.own + 3, codeStart->getRVA());
    if (check)
      buf[l.own + 9] = 0xCB; // mov r11, rcx
    write32le(buf + l.own + 16, codeSize);
    rel8(l.own + 21, l.imported);
  }
  if (hits) {
    // The type check; jne mismatch; jmp rax or ret
    uint32_t off = l.hit;
    memcpy(buf + off, compare.data(), compare.size());
    off += compare.size();
    buf[off] = 0x0F;
    buf[off + 1] = 0x85;
    rel32(off + 2, mismatchRVA);
    if (check) {
      buf[off + 6] = 0xC3;
    } else {
      buf[off + 6] = 0xFF;
      buf[off + 7] = 0xE0;
    }
  }
  for (size_t i = 0; i != imported.size(); ++i) {
    // cmp rax, [rip + start]; jb 1f; cmp rax, [rip + end]; jb hit; 1:
    uint32_t off = l.imported + i * 18;
    const uint8_t test[] = {0x48, 0x3B, modrm, 0, 0, 0, 0, 0x72, 0x09,
                            0x48, 0x3B, modrm, 0, 0, 0, 0, 0x72, 0};
    memcpy(buf + off, test, sizeof(test));
    rel32(off + 3, imported[i].start->getRVA());
    rel32(off + 12, imported[i].end->getRVA());
    rel8(off + 17, l.hit);
  }
  // The page test; je mismatch; the type check; jne mismatch; jmp [rip + guard]
  uint32_t off = l.outside;
  memcpy(buf + off, pageTest.data(), pageTest.size());
  off += pageTest.size();
  buf[off] = 0x0F;
  buf[off + 1] = 0x84;
  rel32(off + 2, mismatchRVA);
  off += 6;
  memcpy(buf + off, compare.data(), compare.size());
  off += compare.size();
  buf[off] = 0x0F;
  buf[off + 1] = 0x85;
  rel32(off + 2, mismatchRVA);
  buf[off + 6] = 0xFF;
  buf[off + 7] = 0x25;
  rel32(off + 8, guardRVA);
}

KCFIListChunk::KCFIListChunk(COFFLinkerContext &ctx, StringRef sectionName,
                             Defined *sym, uint64_t value)
    : sectionName(sectionName), sym(sym), value(value), ctx(ctx) {
  setAlignment(8);
}

void KCFIListChunk::getBaserels(std::vector<Baserel> *res) {
  if (sym && sym->isLive())
    res->emplace_back(getRVA(), ctx.config.machine);
}

void KCFIListChunk::writeTo(uint8_t *buf) const {
  uint64_t v = value;
  if (sym)
    v = sym->isLive() ? sym->getRVA() + ctx.config.imageBase : 0;
  write64le(buf, v);
}

void LocalImportChunk::writeTo(uint8_t *buf) const {
  if (ctx.config.is64()) {
    write64le(buf, sym->getRVA() + ctx.config.imageBase);
  } else {
    uint32_t bit = 0;
    // Pointer to thumb code must have the LSB set, so adjust it. Only code is
    // adjusted: dllimport of a locally defined variable is valid, and the
    // pointer to such a variable must stay unmodified.
    if (ctx.config.machine == ARMNT && sym->getChunk() &&
        (sym->getChunk()->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
      bit = 1;
    write32le(buf, (sym->getRVA() + ctx.config.imageBase) | bit);
  }
}

void RVATableChunk::writeTo(uint8_t *buf) const {
  ulittle32_t *begin = reinterpret_cast<ulittle32_t *>(buf);
  size_t cnt = 0;
  for (const ChunkAndOffset &co : syms)
    begin[cnt++] = co.inputChunk->getRVA() + co.offset;
  llvm::sort(begin, begin + cnt);
  assert(std::unique(begin, begin + cnt) == begin + cnt &&
         "RVA tables should be de-duplicated");
}

void RVAFlagTableChunk::writeTo(uint8_t *buf) const {
  struct RVAFlag {
    ulittle32_t rva;
    uint8_t flag;
  };
  auto flags =
      MutableArrayRef(reinterpret_cast<RVAFlag *>(buf), syms.size());
  for (auto t : zip(syms, flags)) {
    const auto &sym = std::get<0>(t);
    auto &flag = std::get<1>(t);
    flag.rva = sym.inputChunk->getRVA() + sym.offset;
    flag.flag = exportSuppressed.contains(sym)
                    ? uint8_t(GuardTableEntryFlags::EXPORT_SUPPRESSED)
                    : 0;
  }
  llvm::sort(flags,
             [](const RVAFlag &a, const RVAFlag &b) { return a.rva < b.rva; });
  assert(llvm::unique(flags, [](const RVAFlag &a,
                                const RVAFlag &b) { return a.rva == b.rva; }) ==
             flags.end() &&
         "RVA tables should be de-duplicated");
}

size_t ECCodeMapChunk::getSize() const {
  return map.size() * sizeof(chpe_range_entry);
}

void ECCodeMapChunk::writeTo(uint8_t *buf) const {
  auto table = reinterpret_cast<chpe_range_entry *>(buf);
  for (uint32_t i = 0; i < map.size(); i++) {
    const ECCodeMapEntry &entry = map[i];
    uint32_t start = entry.first->getRVA() & ~0xfff;
    table[i].StartOffset = start | entry.type;
    table[i].Length = entry.last->getRVA() + entry.last->getSize() - start;
  }
}

// MinGW specific, for the "automatic import of variables from DLLs" feature.
size_t PseudoRelocTableChunk::getSize() const {
  if (relocs.empty())
    return 0;
  return 12 + 12 * relocs.size();
}

// MinGW specific.
void PseudoRelocTableChunk::writeTo(uint8_t *buf) const {
  if (relocs.empty())
    return;

  ulittle32_t *table = reinterpret_cast<ulittle32_t *>(buf);
  // This is the list header, to signal the runtime pseudo relocation v2
  // format.
  table[0] = 0;
  table[1] = 0;
  table[2] = 1;

  size_t idx = 3;
  for (const RuntimePseudoReloc &rpr : relocs) {
    table[idx + 0] = rpr.sym->getRVA();
    table[idx + 1] = rpr.target->getRVA() + rpr.targetOffset;
    table[idx + 2] = rpr.flags;
    idx += 3;
  }
}

// Windows-specific. This class represents a block in .reloc section.
// The format is described here.
//
// On Windows, each DLL is linked against a fixed base address and
// usually loaded to that address. However, if there's already another
// DLL that overlaps, the loader has to relocate it. To do that, DLLs
// contain .reloc sections which contain offsets that need to be fixed
// up at runtime. If the loader finds that a DLL cannot be loaded to its
// desired base address, it loads it to somewhere else, and add <actual
// base address> - <desired base address> to each offset that is
// specified by the .reloc section. In ELF terms, .reloc sections
// contain relative relocations in REL format (as opposed to RELA.)
//
// This already significantly reduces the size of relocations compared
// to ELF .rel.dyn, but Windows does more to reduce it (probably because
// it was invented for PCs in the late '80s or early '90s.)  Offsets in
// .reloc are grouped by page where the page size is 12 bits, and
// offsets sharing the same page address are stored consecutively to
// represent them with less space. This is very similar to the page
// table which is grouped by (multiple stages of) pages.
//
// For example, let's say we have 0x00030, 0x00500, 0x00700, 0x00A00,
// 0x20004, and 0x20008 in a .reloc section for x64. The uppermost 4
// bits have a type IMAGE_REL_BASED_DIR64 or 0xA. In the section, they
// are represented like this:
//
//   0x00000  -- page address (4 bytes)
//   16       -- size of this block (4 bytes)
//     0xA030 -- entries (2 bytes each)
//     0xA500
//     0xA700
//     0xAA00
//   0x20000  -- page address (4 bytes)
//   12       -- size of this block (4 bytes)
//     0xA004 -- entries (2 bytes each)
//     0xA008
//
// Usually we have a lot of relocations for each page, so the number of
// bytes for one .reloc entry is close to 2 bytes on average.
BaserelChunk::BaserelChunk(uint32_t page, Baserel *begin, Baserel *end) {
  // Block header consists of 4 byte page RVA and 4 byte block size.
  // Each entry is 2 byte. Last entry may be padding.
  data.resize(alignTo((end - begin) * 2 + 8, 4));
  uint8_t *p = data.data();
  write32le(p, page);
  write32le(p + 4, data.size());
  p += 8;
  for (Baserel *i = begin; i != end; ++i) {
    write16le(p, (i->type << 12) | (i->rva - page));
    p += 2;
  }
}

void BaserelChunk::writeTo(uint8_t *buf) const {
  memcpy(buf, data.data(), data.size());
}

uint8_t Baserel::getDefaultType(llvm::COFF::MachineTypes machine) {
  return is64Bit(machine) ? IMAGE_REL_BASED_DIR64 : IMAGE_REL_BASED_HIGHLOW;
}

MergeChunk::MergeChunk(uint32_t alignment)
    : builder(StringTableBuilder::RAW, llvm::Align(alignment)) {
  setAlignment(alignment);
}

void MergeChunk::addSection(COFFLinkerContext &ctx, SectionChunk *c) {
  assert(isPowerOf2_32(c->getAlignment()));
  uint8_t p2Align = llvm::Log2_32(c->getAlignment());
  assert(p2Align < std::size(ctx.mergeChunkInstances));
  auto *&mc = ctx.mergeChunkInstances[p2Align];
  if (!mc)
    mc = make<MergeChunk>(c->getAlignment());
  mc->sections.push_back(c);
}

void MergeChunk::finalizeContents() {
  assert(!finalized && "should only finalize once");
  for (SectionChunk *c : sections)
    if (c->live)
      builder.add(toStringRef(c->getContents()));
  builder.finalize();
  finalized = true;
}

void MergeChunk::assignSubsectionRVAs() {
  for (SectionChunk *c : sections) {
    if (!c->live)
      continue;
    size_t off = builder.getOffset(toStringRef(c->getContents()));
    c->setRVA(rva + off);
  }
}

uint32_t MergeChunk::getOutputCharacteristics() const {
  return IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;
}

size_t MergeChunk::getSize() const {
  return builder.getSize();
}

void MergeChunk::writeTo(uint8_t *buf) const {
  builder.write(buf);
}

// MinGW specific.
size_t AbsolutePointerChunk::getSize() const {
  return symtab.ctx.config.wordsize;
}

void AbsolutePointerChunk::writeTo(uint8_t *buf) const {
  if (symtab.ctx.config.is64()) {
    write64le(buf, value);
  } else {
    write32le(buf, value);
  }
}

MachineTypes AbsolutePointerChunk::getMachine() const { return symtab.machine; }

void ECExportThunkChunk::writeTo(uint8_t *buf) const {
  memcpy(buf, ECExportThunkCode, sizeof(ECExportThunkCode));
  write32le(buf + 10, target->getRVA() - rva - 14);
}

size_t CHPECodeRangesChunk::getSize() const {
  return exportThunks.size() * sizeof(chpe_code_range_entry);
}

void CHPECodeRangesChunk::writeTo(uint8_t *buf) const {
  auto ranges = reinterpret_cast<chpe_code_range_entry *>(buf);

  for (uint32_t i = 0; i < exportThunks.size(); i++) {
    Chunk *thunk = exportThunks[i].first;
    uint32_t start = thunk->getRVA();
    ranges[i].StartRva = start;
    ranges[i].EndRva = start + thunk->getSize();
    ranges[i].EntryPoint = start;
  }
}

size_t CHPERedirectionChunk::getSize() const {
  // Add an extra +1 for a terminator entry.
  return (exportThunks.size() + 1) * sizeof(chpe_redirection_entry);
}

void CHPERedirectionChunk::writeTo(uint8_t *buf) const {
  auto entries = reinterpret_cast<chpe_redirection_entry *>(buf);

  for (uint32_t i = 0; i < exportThunks.size(); i++) {
    entries[i].Source = exportThunks[i].first->getRVA();
    entries[i].Destination = exportThunks[i].second->getRVA();
  }
}

ImportThunkChunkARM64EC::ImportThunkChunkARM64EC(ImportFile *file)
    : ImportThunkChunk(file->symtab.ctx, file->impSym), file(file) {}

size_t ImportThunkChunkARM64EC::getSize() const {
  if (!extended)
    return sizeof(importThunkARM64EC);
  // The last instruction is replaced with an inline range extension thunk.
  return sizeof(importThunkARM64EC) + sizeof(arm64Thunk) - sizeof(uint32_t);
}

void ImportThunkChunkARM64EC::writeTo(uint8_t *buf) const {
  memcpy(buf, importThunkARM64EC, sizeof(importThunkARM64EC));
  applyArm64Addr(buf, file->impSym->getRVA(), rva, 12);
  applyArm64Ldr(buf + 4, file->impSym->getRVA() & 0xfff);

  // The exit thunk may be missing. This can happen if the application only
  // references a function by its address (in which case the thunk is never
  // actually used, but is still required to fill the auxiliary IAT), or in
  // cases of hand-written assembly calling an imported ARM64EC function (where
  // the exit thunk is ignored by __icall_helper_arm64ec). In such cases, MSVC
  // link.exe uses 0 as the RVA.
  uint32_t exitThunkRVA = exitThunk ? exitThunk->getRVA() : 0;
  applyArm64Addr(buf + 8, exitThunkRVA, rva + 8, 12);
  applyArm64Imm(buf + 12, exitThunkRVA & 0xfff, 0);

  Defined *helper = cast<Defined>(file->symtab.ctx.config.arm64ECIcallHelper);
  if (extended) {
    // Replace last instruction with an inline range extension thunk.
    memcpy(buf + 16, arm64Thunk, sizeof(arm64Thunk));
    applyArm64Addr(buf + 16, helper->getRVA(), rva + 16, 12);
    applyArm64Imm(buf + 20, helper->getRVA() & 0xfff, 0);
  } else {
    applyArm64Branch26(buf + 16, helper->getRVA() - rva - 16);
  }
}

bool ImportThunkChunkARM64EC::verifyRanges() {
  if (extended)
    return true;
  auto helper = cast<Defined>(file->symtab.ctx.config.arm64ECIcallHelper);
  return isInt<28>(helper->getRVA() - rva - 16);
}

uint32_t ImportThunkChunkARM64EC::extendRanges() {
  if (extended || verifyRanges())
    return 0;
  extended = true;
  // The last instruction is replaced with an inline range extension thunk.
  return sizeof(arm64Thunk) - sizeof(uint32_t);
}

uint64_t Arm64XRelocVal::get() const {
  return (sym ? sym->getRVA() : 0) + (chunk ? chunk->getRVA() : 0) + value;
}

size_t Arm64XDynamicRelocEntry::getSize() const {
  switch (type) {
  case IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL:
    return sizeof(uint16_t); // Just a header.
  case IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE:
    return sizeof(uint16_t) + size; // A header and a payload.
  case IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA:
    return 2 * sizeof(uint16_t); // A header and a delta.
  }
  llvm_unreachable("invalid type");
}

void Arm64XDynamicRelocEntry::writeTo(uint8_t *buf) const {
  auto out = reinterpret_cast<ulittle16_t *>(buf);
  *out = (offset.get() & 0xfff) | (type << 12);

  switch (type) {
  case IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL:
    *out |= ((bit_width(size) - 1) << 14); // Encode the size.
    break;
  case IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE:
    *out |= ((bit_width(size) - 1) << 14); // Encode the size.
    switch (size) {
    case 2:
      out[1] = value.get();
      break;
    case 4:
      *reinterpret_cast<ulittle32_t *>(out + 1) = value.get();
      break;
    case 8:
      *reinterpret_cast<ulittle64_t *>(out + 1) = value.get();
      break;
    default:
      llvm_unreachable("invalid size");
    }
    break;
  case IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA:
    int delta = value.get();
    // Negative offsets use a sign bit in the header.
    if (delta < 0) {
      *out |= 1 << 14;
      delta = -delta;
    }
    // Depending on the value, the delta is encoded with a shift of 2 or 3 bits.
    if (delta & 7) {
      assert(!(delta & 3));
      delta >>= 2;
    } else {
      *out |= (1 << 15);
      delta >>= 3;
    }
    out[1] = delta;
    assert(!(delta & ~0xffff));
    break;
  }
}

void DynamicRelocsChunk::finalize() {
  llvm::stable_sort(arm64xRelocs, [=](const Arm64XDynamicRelocEntry &a,
                                      const Arm64XDynamicRelocEntry &b) {
    return a.offset.get() < b.offset.get();
  });

  size = sizeof(coff_dynamic_reloc_table) + sizeof(coff_dynamic_relocation64);
  uint32_t prevPage = 0xfff;

  for (const Arm64XDynamicRelocEntry &entry : arm64xRelocs) {
    uint32_t page = entry.offset.get() & ~0xfff;
    if (page != prevPage) {
      size = alignTo(size, sizeof(uint32_t)) +
             sizeof(coff_base_reloc_block_header);
      prevPage = page;
    }
    size += entry.getSize();
  }

  size = alignTo(size, sizeof(uint32_t));
}

// Set the reloc value. The reloc entry must be allocated beforehand.
void DynamicRelocsChunk::set(Arm64XRelocVal offset, Arm64XRelocVal value) {
  uint32_t rva = offset.get();
  auto entry =
      llvm::find_if(arm64xRelocs, [rva](const Arm64XDynamicRelocEntry &e) {
        return e.offset.get() == rva;
      });
  assert(entry != arm64xRelocs.end());
  assert(!entry->value.get());
  entry->value = value;
}

void DynamicRelocsChunk::writeTo(uint8_t *buf) const {
  auto table = reinterpret_cast<coff_dynamic_reloc_table *>(buf);
  table->Version = 1;
  table->Size = sizeof(coff_dynamic_relocation64);
  buf += sizeof(*table);

  auto header = reinterpret_cast<coff_dynamic_relocation64 *>(buf);
  header->Symbol = IMAGE_DYNAMIC_RELOCATION_ARM64X;
  buf += sizeof(*header);

  coff_base_reloc_block_header *pageHeader = nullptr;
  size_t relocSize = 0;
  for (const Arm64XDynamicRelocEntry &entry : arm64xRelocs) {
    uint32_t page = entry.offset.get() & ~0xfff;
    if (!pageHeader || page != pageHeader->PageRVA) {
      relocSize = alignTo(relocSize, sizeof(uint32_t));
      if (pageHeader)
        pageHeader->BlockSize =
            buf + relocSize - reinterpret_cast<uint8_t *>(pageHeader);
      pageHeader =
          reinterpret_cast<coff_base_reloc_block_header *>(buf + relocSize);
      pageHeader->PageRVA = page;
      relocSize += sizeof(*pageHeader);
    }

    entry.writeTo(buf + relocSize);
    relocSize += entry.getSize();
  }
  relocSize = alignTo(relocSize, sizeof(uint32_t));
  pageHeader->BlockSize =
      buf + relocSize - reinterpret_cast<uint8_t *>(pageHeader);

  header->BaseRelocSize = relocSize;
  table->Size += relocSize;
  assert(size == sizeof(*table) + sizeof(*header) + relocSize);
}

} // namespace lld::coff
