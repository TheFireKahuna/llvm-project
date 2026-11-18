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
#include "lld/Common/AArch64Instructions.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/AArch64LogicalImmediate.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/LEB128.h"
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
    : Chunk(k), file(f), header(h), resizable(false), keepDistinct(false),
      guardSuppressed(false), repl(this) {
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

// Adds v to a 32-bit PC-relative field; returns false if the sum overflows.
static bool addRel32(uint8_t *p, int64_t v) {
  v += SignExtend64<32>(read32le(p));
  write32le(p, v);
  return isInt<32>(v);
}

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

// A thread-local's offset is from the first TLS group, whatever output section
// holds it; any other target's is from its output section.
static uint64_t getSecRel(const SectionChunk *sec, OutputSection *os,
                          uint64_t s) {
  return sec->file->symtab.ctx.tlsOffset(s).value_or(s - os->getRVA());
}

static void applySecRel(const SectionChunk *sec, uint8_t *off,
                        OutputSection *os, uint64_t s) {
  if (!checkSecRel(sec, os))
    return;
  uint64_t secRel = getSecRel(sec, os, s);
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

// Returns false if the value does not fit the relocation's field.
bool SectionChunk::applyRelX64(uint8_t *off, uint16_t type, OutputSection *os,
                               uint64_t s, uint64_t p,
                               uint64_t imageBase) const {
  switch (type) {
  case IMAGE_REL_AMD64_ADDR32:
    add32(off, s + imageBase);
    break;
  case IMAGE_REL_AMD64_ADDR64:
    add64(off, s + imageBase);
    break;
  case IMAGE_REL_LLVM_ADDR32NB_CALL:
  case IMAGE_REL_AMD64_ADDR32NB: add32(off, s); break;
  case IMAGE_REL_LLVM_REL32_CALL:
  case IMAGE_REL_LLVM_BRANCH_TAIL:
  case IMAGE_REL_LLVM_BRANCH32:
  case IMAGE_REL_LLVM_JUMP32:
  case IMAGE_REL_LLVM_MEM_CALL:
  case IMAGE_REL_LLVM_MEM_JUMP:
  case IMAGE_REL_LLVM_MEM_JUMP_P1:
  case IMAGE_REL_LLVM_MEM_JUMP_TAIL:
  case IMAGE_REL_LLVM_MEM_LOAD:
  case IMAGE_REL_LLVM_MEM_LOAD_REX2:
  case IMAGE_REL_LLVM_LEA:
  case IMAGE_REL_LLVM_LEA_OFFSET:
  case IMAGE_REL_AMD64_REL32:    return addRel32(off, s - p - 4);
  case IMAGE_REL_AMD64_REL32_1:  return addRel32(off, s - p - 5);
  case IMAGE_REL_AMD64_REL32_2:  return addRel32(off, s - p - 6);
  case IMAGE_REL_AMD64_REL32_3:  return addRel32(off, s - p - 7);
  case IMAGE_REL_AMD64_REL32_4:  return addRel32(off, s - p - 8);
  case IMAGE_REL_AMD64_REL32_5:  return addRel32(off, s - p - 9);
  case IMAGE_REL_AMD64_SECTION:
    applySecIdx(off, os, file->symtab.ctx.outputSections.size());
    break;
  case IMAGE_REL_AMD64_SECREL:   applySecRel(this, off, os, s); break;
  case IMAGE_REL_LLVM_JUMP8: {
    int64_t v = s + int8_t(*off) - p - 1;
    if (!isInt<8>(v))
      return false;
    *off = v;
    break;
  }
  // A fused read's field holds how its load is disposed of, not an addend.
  case IMAGE_REL_LLVM_FUSED_READ:
  case IMAGE_REL_LLVM_FUSED_READ_REX2:
    write32le(off, s - p - 4);
    break;
  // s is the offset from the thread pointer, which a displacement holds.
  case IMAGE_REL_LLVM_TPOFF32:
    if (s > INT32_MAX)
      return false;
    add32(off, s);
    break;
  case IMAGE_REL_AMD64_SREL32:
  case IMAGE_REL_AMD64_EHANDLER:
  case IMAGE_REL_LLVM_FUSED_CALL:
  case IMAGE_REL_LLVM_FUSED_JUMP:
  case IMAGE_REL_LLVM_FUSED_ACCESS:
  case IMAGE_REL_LLVM_FUSED_INDEX:
  case IMAGE_REL_LLVM_FUSED_TEST:
    break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
  return true;
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
static uint32_t getArm64LdrScale(uint32_t insn) {
  uint32_t size = insn >> 30;
  // 0x04000000 indicates SIMD/FP registers
  // 0x00800000 indicates 128 bit
  if ((insn & 0x4800000) == 0x4800000)
    size += 4;
  return size;
}

static void applyArm64Ldr(uint8_t *off, uint64_t imm) {
  uint32_t size = getArm64LdrScale(read32le(off));
  if ((imm & ((1 << size) - 1)) != 0)
    error("misaligned ldr/str offset");
  applyArm64Imm(off, imm >> size, size);
}

static void applySecRelLow12A(const SectionChunk *sec, uint8_t *off,
                              OutputSection *os, uint64_t s) {
  if (checkSecRel(sec, os))
    applyArm64Imm(off, getSecRel(sec, os, s) & 0xfff, 0);
}

static void applySecRelHigh12A(const SectionChunk *sec, uint8_t *off,
                               OutputSection *os, uint64_t s) {
  if (!checkSecRel(sec, os))
    return;
  uint32_t orig = read32le(off);
  uint64_t imm = (orig >> 10) & 0xFFF;
  orig &= ~(0xFFF << 10);
  imm = (getSecRel(sec, os, s) + imm) >> 12;
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
    applyArm64Ldr(off, getSecRel(sec, os, s) & 0xfff);
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

// Returns false if the value does not fit the relocation's field.
bool SectionChunk::applyRelARM64(uint8_t *off, uint16_t type, OutputSection *os,
                                 uint64_t s, uint64_t p,
                                 uint64_t imageBase) const {
  switch (type) {
  case IMAGE_REL_ARM64_PAGEBASE_REL21: applyArm64Addr(off, s, p, 12); break;
  case IMAGE_REL_ARM64_REL21:          applyArm64Addr(off, s, p, 0); break;
  case IMAGE_REL_ARM64_PAGEOFFSET_12A: applyArm64Imm(off, s & 0xfff, 0); break;
  case IMAGE_REL_LLVM_FUSED_READ:
  case IMAGE_REL_ARM64_PAGEOFFSET_12L: applyArm64Ldr(off, s & 0xfff); break;
  case IMAGE_REL_LLVM_BRANCH_TAIL:
  case IMAGE_REL_ARM64_BRANCH26:       applyArm64Branch26(off, s - p); break;
  case IMAGE_REL_ARM64_BRANCH19:       applyArm64Branch19(off, s - p); break;
  case IMAGE_REL_ARM64_BRANCH14:       applyArm64Branch14(off, s - p); break;
  case IMAGE_REL_ARM64_ADDR32:
    add32(off, s + imageBase);
    break;
  case IMAGE_REL_LLVM_ADDR32NB_CALL:
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
  case IMAGE_REL_LLVM_REL32_CALL:
  case IMAGE_REL_ARM64_REL32:          return addRel32(off, s - p - 4);
  // The low or the second 16 bits of an absolute value in a MOVZ or MOVK.
  case IMAGE_REL_LLVM_MOVW_UABS_G0:
  case IMAGE_REL_LLVM_MOVW_UABS_G0_NC:
  case IMAGE_REL_LLVM_MOVW_UABS_G1: {
    uint32_t insn = read32le(off);
    uint64_t v = s + imageBase + ((insn >> 5) & 0xffff);
    unsigned shift = type == IMAGE_REL_LLVM_MOVW_UABS_G1 ? 16 : 0;
    if (type != IMAGE_REL_LLVM_MOVW_UABS_G0_NC && v >> (shift + 16))
      return false;
    write32le(off, (insn & ~(0xffff << 5)) | ((v >> shift) & 0xffff) << 5);
    break;
  }
  // An absolute value as the bitmask immediate of AND, ANDS or TST.
  case IMAGE_REL_LLVM_LOGIMM: {
    uint32_t insn = read32le(off);
    uint64_t imm;
    if (!AArch64_AM::processLogicalImmediate(s + imageBase,
                                             insn >> 31 ? 64 : 32, imm))
      return false;
    write32le(off, (insn & ~(0x1fff << 10)) | imm << 10);
    break;
  }
  // s is the offset from the thread pointer. The high part's field holds
  // the low twelve bits of an addend.
  case IMAGE_REL_LLVM_TPOFF_HIGH12A: {
    uint32_t insn = read32le(off);
    uint64_t v = (s + ((insn >> 10) & 0xfff)) >> 12;
    if (v > 0xfff)
      return false;
    write32le(off, (insn & ~(0xfff << 10)) | v << 10);
    break;
  }
  case IMAGE_REL_LLVM_TPOFF_LOW12A:
    applyArm64Imm(off, s & 0xfff, 0);
    break;
  case IMAGE_REL_LLVM_TPOFF_LOW12L:
    applyArm64Ldr(off, s & 0xfff);
    break;
  case IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL:
  case IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL:
  case IMAGE_REL_LLVM_FUSED_CALL:
  case IMAGE_REL_LLVM_FUSED_JUMP:
  case IMAGE_REL_LLVM_FUSED_TEST:
    break;
  default:
    error("unsupported relocation type 0x" + Twine::utohexstr(type) + " in " +
          toString(file));
  }
  return true;
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

bool SectionChunk::isPair(uint16_t type) const {
  switch (type) {
  case IMAGE_REL_AMD64_PAIR:
    return getArch() == Triple::x86_64;
  case IMAGE_REL_ARM64_PAIR:
    return getArch() == Triple::aarch64;
  default:
    return false;
  }
}

// Returns the PAIR naming the second operand B of rel, whose value is then
// A - B: a span and its PAIR at the same offset, or an ARM64 page offset
// followed by a PAIR.
const coff_relocation *SectionChunk::getPair(const coff_relocation &rel) const {
  const coff_relocation *next = &rel + 1;
  if (next == relocsData + relocsSize || !isPair(next->Type))
    return nullptr;
  bool x64 = next->Type == IMAGE_REL_AMD64_PAIR;
  switch (rel.Type) {
  case IMAGE_REL_ARM64_PAGEOFFSET_12A:
  case IMAGE_REL_ARM64_PAGEOFFSET_12L:
    return x64 ? nullptr : next;
  case IMAGE_REL_AMD64_SSPAN32:
  case IMAGE_REL_LLVM_SSPAN_EPILOGV2:
    if (!x64)
      return nullptr;
    break;
  case IMAGE_REL_LLVM_SSPAN32:
  case IMAGE_REL_LLVM_SSPAN_WORDS18:
  case IMAGE_REL_LLVM_USPAN8_W:
  case IMAGE_REL_LLVM_USPAN16_W:
    if (x64)
      return nullptr;
    break;
  case IMAGE_REL_LLVM_SSPAN8:
  case IMAGE_REL_LLVM_SSPAN16:
  case IMAGE_REL_LLVM_SSPAN64:
  case IMAGE_REL_LLVM_USPAN8:
  case IMAGE_REL_LLVM_USPAN16:
  case IMAGE_REL_LLVM_SSPAN_ULEB:
  case IMAGE_REL_LLVM_SSPAN_SLEB:
    break;
  default:
    return nullptr;
  }
  return rel.VirtualAddress == next->VirtualAddress ? next : nullptr;
}

Symbol *SectionChunk::getRelocTarget(const coff_relocation &rel) const {
  if (relocationNamesNoSymbol(rel.Type) ||
      (isPair(rel.Type) && !file->forkObject))
    return nullptr;
  return file->getSymbol(rel.SymbolTableIndex);
}

// Returns whether a relocation of the given type at off in sc, naming a
// function override, takes the stub rather than the body: debug info and
// unwind data describe the body, an ARM64 BRANCH26 is a site, which branches
// to the default body as linked, and an addend points into the body. A fused
// read's field on x86-64 holds no addend, and a JUMP8's is one byte.
static bool referencesStub(const SectionChunk *sc, uint16_t type,
                           const uint8_t *off) {
  StringRef name = sc->getSectionName();
  if (name.starts_with(".debug") || name.starts_with(".pdata") ||
      name.starts_with(".xdata"))
    return false;
  if (sc->getArch() != Triple::aarch64)
    return type == IMAGE_REL_LLVM_FUSED_READ ||
           type == IMAGE_REL_LLVM_FUSED_READ_REX2 ||
           (type == IMAGE_REL_AMD64_ADDR64 ? read64le(off)
            : type == IMAGE_REL_LLVM_JUMP8 ? *off
                                           : read32le(off)) == 0;
  switch (type) {
  case IMAGE_REL_ARM64_BRANCH26:
  case IMAGE_REL_LLVM_BRANCH_TAIL:
    return false;
  case IMAGE_REL_ARM64_ADDR64:
    return read64le(off) == 0;
  case IMAGE_REL_ARM64_PAGEBASE_REL21:
  case IMAGE_REL_ARM64_REL21:
    return (read32le(off) & 0x60ffffe0) == 0;
  case IMAGE_REL_ARM64_PAGEOFFSET_12A:
  case IMAGE_REL_ARM64_PAGEOFFSET_12L:
  case IMAGE_REL_LLVM_FUSED_READ:
    return (read32le(off) & 0x3ffc00) == 0;
  case IMAGE_REL_ARM64_BRANCH19:
    return (read32le(off) & 0xffffe0) == 0;
  case IMAGE_REL_ARM64_BRANCH14:
    return (read32le(off) & 0x7ffe0) == 0;
  default:
    return read32le(off) == 0;
  }
}

void SectionChunk::applyRelocation(uint8_t *off,
                                   const coff_relocation &rel) const {
  // ABSOLUTE writes nothing; a PAIR is applied with the relocation before it.
  COFFLinkerContext &ctx = file->symtab.ctx;
  if (relocationNamesNoSymbol(rel.Type)) {
    if (rel.Type != IMAGE_REL_LLVM_TLS_SCALE)
      return;
    // The scale site forms the image's thread-local base in one register, and
    // LLD writes the image's scale into its immediate. On x86-64 it is imul
    // r64, r64, imm32, then add r64, [rip+place] into the same register. On
    // ARM64 it is adrp and ldr of the place into Xp, movz Xs, #imm16, then
    // madd Xd, Xn, Xs, Xp.
    size_t size = getSize() - rel.VirtualAddress;
    std::string problem = "do not form a thread-local base";
    if (ctx.isTlsModel("pe")) {
      problem = "are in an image of the pe thread-local model, which has no "
                "scale";
    } else if (getArch() == Triple::x86_64 && size >= 14) {
      if ((off[0] & 0xfa) == 0x48 && off[1] == 0x69 && off[2] >= 0xc0 &&
          off[7] == (off[0] & 0xfc) && off[8] == 0x03 &&
          off[9] == (0x05 | (off[2] & 0x38))) {
        write32le(off + 3, ctx.tlsScale);
        return;
      }
    } else if (getArch() == Triple::aarch64 && size >= 16) {
      aarch64::Adrp adrp;
      aarch64::Ldr ldr;
      uint32_t movz = read32le(off + 8), madd = read32le(off + 12);
      uint32_t xs = movz & 31;
      if (aarch64::parseAdrp(read32le(off), adrp) &&
          aarch64::parseLdr(read32le(off + 4), ldr) && ldr.p2Size == 3 &&
          !ldr.isFloat && ldr.extendType == aarch64::ZeroExtend &&
          ldr.baseRegister == adrp.destRegister &&
          ldr.destRegister == adrp.destRegister && adrp.destRegister != 31 &&
          (movz & 0xffe00000) == 0xd2800000 && xs != 31 &&
          xs != adrp.destRegister && (madd & 0xffe08000) == 0x9b000000 &&
          ((madd >> 16) & 31) == xs &&
          ((madd >> 10) & 31) == ldr.destRegister) {
        if (isUInt<16>(ctx.tlsScale)) {
          write32le(off + 8, (movz & ~(0xffff << 5)) | ctx.tlsScale << 5);
          return;
        }
        problem = ("cannot hold the image's scale, " + Twine(ctx.tlsScale) +
                   ", in 16 bits")
                      .str();
      }
    }
    Err(ctx) << file << ": the instructions at the TLS_SCALE relocation at "
             << "offset 0x" << Twine::utohexstr(rel.VirtualAddress) << " of "
             << getSectionName() << " " << problem;
    return;
  }
  if (isPair(rel.Type)) {
    if (&rel == relocsData || getPair(*(&rel - 1)) != &rel)
      Err(ctx) << file << ": PAIR relocation in " << getSectionName()
               << " follows no relocation it pairs with";
    return;
  }

  // A symbol is discarded if it was discarded early, when it is null, or
  // late, when its chunk has no output section, unless it is an absolute or
  // synthetic symbol.
  auto isDiscarded = [&](Defined *d) {
    Chunk *c = d ? d->getChunk() : nullptr;
    return !d || (!(c && ctx.getOutputSection(c)) && !isa<DefinedAbsolute>(d) &&
                  !isa<DefinedSynthetic>(d));
  };
  auto *sym = dyn_cast_or_null<Defined>(file->getSymbol(rel.SymbolTableIndex));
  // The adrp and ldr of a local import pointer that bindLocalImports bypassed
  // become the adrp and add of its symbol.
  uint16_t type = rel.Type;
  auto *li = dyn_cast_or_null<DefinedLocalImport>(sym);
  if (li && li->getChunk()->isBypassed() &&
      (type == IMAGE_REL_ARM64_PAGEBASE_REL21 ||
       type == IMAGE_REL_ARM64_PAGEOFFSET_12L)) {
    sym = li->getTarget();
    if (type == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
      write32le(off, 0x91000000 | (read32le(off) & 0x3ff));
      type = IMAGE_REL_ARM64_PAGEOFFSET_12A;
    }
  }
  if (FunctionOverrideThunk *t = FunctionOverrideThunk::get(sym))
    if (!referencesStub(this, type, off))
      sym = t->fo.body;
  const coff_relocation *pair = getPair(rel);
  auto *b =
      pair ? dyn_cast_or_null<Defined>(file->getSymbol(pair->SymbolTableIndex))
           : nullptr;
  if (isDiscarded(sym)) {
    maybeReportRelocationToDiscarded(this, sym, rel, ctx.config.mingw);
    return;
  }
  if (pair && isDiscarded(b)) {
    maybeReportRelocationToDiscarded(this, b, *pair, ctx.config.mingw);
    return;
  }
  uint64_t s = sym->getRVA();

  // Get the output section of the symbol for this relocation.  The output
  // section is needed to compute SECREL and SECTION relocations used in debug
  // info.
  Chunk *c = sym->getChunk();
  OutputSection *os = c ? ctx.getOutputSection(c) : nullptr;

  // A pair's value is A - B. It overwrites a span's field, which must hold it,
  // a LEB128 keeping its assembled length, and is added to an ARM64 page
  // offset's immediate, a byte offset that must stay in the page.
  if (pair) {
    int64_t v = s - b->getRVA();
    auto check = [&](bool fits, bool aligned = true) {
      if (!fits || !aligned)
        Err(ctx) << file << ": "
                 << file->getCOFFObj()->getRelocationTypeName(rel.Type)
                 << " relocation against " << toString(ctx, *sym)
                 << " with a PAIR against " << toString(ctx, *b)
                 << (fits ? " is misaligned" : " is out of range");
      return fits && aligned;
    };
    switch (rel.Type) {
    case IMAGE_REL_AMD64_SSPAN32:
      write32le(off, v);
      return;
    case IMAGE_REL_LLVM_SSPAN8:
      if (check(isInt<8>(v)))
        *off = v;
      return;
    case IMAGE_REL_LLVM_USPAN8:
      if (check(isUInt<8>(v)))
        *off = v;
      return;
    case IMAGE_REL_LLVM_SSPAN16:
      if (check(isInt<16>(v)))
        write16le(off, v);
      return;
    case IMAGE_REL_LLVM_USPAN16:
      if (check(isUInt<16>(v)))
        write16le(off, v);
      return;
    case IMAGE_REL_LLVM_SSPAN32:
      if (check(isInt<32>(v)))
        write32le(off, v);
      return;
    // The word-scaled spans count instructions.
    case IMAGE_REL_LLVM_USPAN8_W:
      if (check(isUInt<10>(v), !(v & 3)))
        *off = v >> 2;
      return;
    case IMAGE_REL_LLVM_USPAN16_W:
      if (check(isUInt<18>(v), !(v & 3)))
        write16le(off, v >> 2);
      return;
    case IMAGE_REL_LLVM_SSPAN_WORDS18:
      if (check(isUInt<20>(v), !(v & 3)))
        write32le(off, (read32le(off) & ~0x3ffff) | v >> 2);
      return;
    case IMAGE_REL_LLVM_SSPAN64:
      write64le(off, v);
      return;
    case IMAGE_REL_LLVM_SSPAN_ULEB:
    case IMAGE_REL_LLVM_SSPAN_SLEB: {
      ArrayRef<uint8_t> field = getContents().drop_front(rel.VirtualAddress);
      const uint8_t *last =
          llvm::find_if(field, [](uint8_t c) { return !(c & 0x80); });
      unsigned size = last - field.begin() + 1;
      if (rel.Type == IMAGE_REL_LLVM_SSPAN_ULEB) {
        if (check(last != field.end() && v >= 0 && isUIntN(7 * size, v)))
          encodeULEB128(v, off, size);
      } else if (check(last != field.end() && isIntN(7 * size, v))) {
        encodeSLEB128(v, off, size);
      }
      return;
    }
    // The offset's low byte is the unwind code's first byte, and its high
    // four bits are the top of the second.
    case IMAGE_REL_LLVM_SSPAN_EPILOGV2:
      if (check(isUInt<12>(v))) {
        off[0] = v;
        off[1] = (off[1] & 0xf) | (v >> 8) << 4;
      }
      return;
    }
    uint32_t insn = read32le(off);
    uint32_t imm = (insn >> 10) & 0xfff;
    uint32_t scale = 0;
    if (rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
      scale = getArm64LdrScale(insn);
      v += SignExtend64<12>(imm << scale);
    } else {
      v += imm;
    }
    if (check(isUInt<12>(v), !(v & ((1 << scale) - 1))))
      write32le(off, (insn & ~(0xfff << 10)) | (v >> scale) << 10);
    return;
  }

  // ARM64's PC-relative page addresses cannot reach an absolute value, its
  // immediate forms hold nothing else, and an x86-64 32-bit address is
  // sign-extended where it is used.
  uint64_t imageBase = ctx.config.imageBase;
  if (isa<DefinedAbsolute>(sym) && getArch() == Triple::aarch64 &&
      (rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 ||
       rel.Type == IMAGE_REL_ARM64_REL21)) {
    Err(ctx) << file << ": PC-relative relocation against absolute symbol "
             << toString(ctx, *sym);
    return;
  }
  if (!isa<DefinedAbsolute>(sym) && getArch() == Triple::aarch64 &&
      rel.Type >= IMAGE_REL_LLVM_MOVW_UABS_G0 &&
      rel.Type <= IMAGE_REL_LLVM_LOGIMM) {
    Err(ctx) << file << ": "
             << file->getCOFFObj()->getRelocationTypeName(rel.Type)
             << " relocation against " << toString(ctx, *sym)
             << ", which is not an absolute symbol";
    return;
  }
  if (getArch() == Triple::x86_64 && rel.Type == IMAGE_REL_AMD64_ADDR32 &&
      !isUInt<31>(SignExtend64<32>(read32le(off)) + s + imageBase))
    Warn(ctx) << file << ": ADDR32 relocation against " << toString(ctx, *sym)
              << " is out of range";

  // A thread-pointer offset is the target's offset in the TLS groups plus the
  // image's place.
  if (rel.Type >= IMAGE_REL_LLVM_TPOFF32 &&
      rel.Type <= IMAGE_REL_LLVM_TPOFF_LOW12L) {
    if (ctx.tlsOversize) {
      if (!ctx.tlsOversizeReported.exchange(true))
        Err(ctx) << "TPOFF relocations need a thread-local place, which an "
                 << "executable has only for thread-locals of up to "
                 << ctx.tlsSlotSize << " bytes; it has " << ctx.tlsOversize;
      return;
    }
    std::optional<uint64_t> offset = ctx.tlsOffset(s, /*inclusive=*/true);
    if (!ctx.tlsPlace || !offset) {
      Err(ctx) << file << ": "
               << file->getCOFFObj()->getRelocationTypeName(rel.Type)
               << " relocation against " << toString(ctx, *sym)
               << (ctx.tlsPlace ? ", which is not a thread-local"
                                : " needs an image whose thread-local place "
                                  "the link knows");
      return;
    }
    s = *offset + *ctx.tlsPlace;
  }

  // Compute the RVA of the relocation for relative relocations.
  uint64_t p = rva + rel.VirtualAddress;
  // /force:unresolved resolves an undefined symbol to absolute zero too.
  auto reportMisfit = [&] {
    COFFSyncStream(ctx, ctx.config.forceUnresolved && isAbsoluteZero(sym)
                            ? DiagLevel::Warn
                            : DiagLevel::Err)
        << file << ": " << file->getCOFFObj()->getRelocationTypeName(rel.Type)
        << " relocation against " << toString(ctx, *sym)
        << (rel.Type == IMAGE_REL_LLVM_LOGIMM ? " is not a bitmask immediate"
            : isAbsoluteZero(sym)
                ? " is out of range: it resolves to address 0, as a weak "
                  "external nothing defines does; reference it through a "
                  "pointer, or define it"
                : " is out of range");
  };
  // No ARM64 branch reaches absolute zero from an image. In an object LLVM made
  // for Windows Itanium or NT-POSIX one goes to the next instruction instead,
  // as lld/ELF resolves a branch to an undefined weak symbol for the AArch64
  // ABI.
  if (getArch() == Triple::aarch64 && isAbsoluteZero(sym) &&
      (type == IMAGE_REL_ARM64_BRANCH26 || type == IMAGE_REL_LLVM_BRANCH_TAIL ||
       type == IMAGE_REL_ARM64_BRANCH19 || type == IMAGE_REL_ARM64_BRANCH14)) {
    if (!file->forkObject) {
      reportMisfit();
      return;
    }
    s = p + 4;
  }
  bool fits = true;
  switch (getArch()) {
  case Triple::x86_64:
    fits = applyRelX64(off, rel.Type, os, s, p, imageBase);
    break;
  case Triple::x86:
    applyRelX86(off, rel.Type, os, s, p, imageBase);
    break;
  case Triple::thumb:
    applyRelARM(off, rel.Type, os, s, p, imageBase);
    break;
  case Triple::aarch64:
    fits = applyRelARM64(off, type, os, s, p, imageBase);
    break;
  case Triple::mipsel:
    applyRelMIPS(off, rel.Type, os, s, p, imageBase);
    break;
  default:
    llvm_unreachable("unknown machine type");
  }
  if (!fits)
    reportMisfit();
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
  // Keep a PAIR after the relocation it belongs to.
  llvm::stable_sort(newRelocs, cmpByVa);
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
    auto *target = dyn_cast_or_null<Defined>(getRelocTarget(rel));
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

void LocalImportChunk::getBaserels(std::vector<Baserel> *res) {
  // An absolute value does not move with the image.
  if (!isa<DefinedAbsolute>(sym))
    res->emplace_back(getRVA(), ctx.config.machine);
}

size_t LocalImportChunk::getSize() const { return ctx.config.wordsize; }

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
    flag.flag = this->flags.lookup(sym);
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

FunctionOverrideThunk *FunctionOverrideThunk::get(const Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedSynthetic>(s);
  return d ? dyn_cast_or_null<FunctionOverrideThunk>(d->getChunk()) : nullptr;
}

void FunctionOverrideThunk::writeTo(uint8_t *buf) const {
  memcpy(buf, prefix.data(), prefix.size());
  buf += prefix.size();
  int64_t disp = fo.body->getRVA() - (getRVA() + prefix.size());
  if (machine == AMD64) {
    buf[0] = 0xe9; // jmp rel32
    write32le(buf + 1, disp - 5);
  } else {
    write32le(buf, 0x14000000); // b imm26
    applyArm64Branch26(buf, disp);
  }
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

  size = sizeof(coff_dynamic_reloc_table);
  if (!arm64xRelocs.empty())
    size += sizeof(coff_dynamic_relocation64);
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

  // The function-override record: its header, each function's entry with
  // its candidates' RVAs and its sites' blocks, then the diagrams, of which
  // equal ones are written once.
  functionOverrideRecord.clear();
  if (functionOverrides.empty())
    return;
  std::string entries, diagrams;
  raw_string_ostream eos(entries), dos(diagrams);
  endian::Writer e(eos, llvm::endianness::little);
  DenseMap<ArrayRef<uint8_t>, uint32_t> diagramOffsets;
  for (const FunctionOverride *fo : functionOverrides) {
    auto [it, inserted] =
        diagramOffsets.try_emplace(fo->diagram, diagrams.size());
    if (inserted)
      dos << toStringRef(fo->diagram);
    uint8_t type = fo->stub->getMachine() == AMD64
                       ? IMAGE_FUNCTION_OVERRIDE_X64_REL32
                       : IMAGE_FUNCTION_OVERRIDE_ARM64_BRANCH26;
    std::vector<Baserel> sites;
    for (auto [c, off] : fo->sites)
      sites.emplace_back(c->getRVA() + off, type);
    llvm::sort(sites, [](const Baserel &x, const Baserel &y) {
      return x.rva < y.rva;
    });
    std::string blocks;
    for (size_t i = 0, j; i != sites.size(); i = j) {
      uint32_t page = sites[i].rva & ~0xfffu;
      for (j = i + 1; j != sites.size() && (sites[j].rva & ~0xfffu) == page;)
        ++j;
      BaserelChunk block(page, sites.data() + i, sites.data() + j);
      size_t n = blocks.size();
      blocks.resize(n + block.getSize());
      block.writeTo(reinterpret_cast<uint8_t *>(&blocks[n]));
    }
    e.write<uint32_t>(fo->body->getRVA());
    e.write<uint32_t>(it->second);
    e.write<uint32_t>(fo->candidates.size() * sizeof(uint32_t));
    e.write<uint32_t>(blocks.size());
    for (Defined *c : fo->candidates)
      e.write<uint32_t>(c->getRVA());
    eos << blocks;
  }
  raw_string_ostream os(functionOverrideRecord);
  endian::Writer w(os, llvm::endianness::little);
  w.write<uint64_t>(IMAGE_DYNAMIC_RELOCATION_FUNCTION_OVERRIDE);
  w.write<uint32_t>(sizeof(coff_function_override_header) + entries.size() +
                    diagrams.size());
  w.write<uint32_t>(entries.size());
  os << entries << diagrams;
  size += functionOverrideRecord.size();
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
  table->Size = size - sizeof(*table);
  buf += sizeof(*table);

  if (!arm64xRelocs.empty()) {
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
    buf += relocSize;
  }
  memcpy(buf, functionOverrideRecord.data(), functionOverrideRecord.size());
}

} // namespace lld::coff
