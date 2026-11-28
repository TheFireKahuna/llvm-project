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
#include "Walker.h"
#include "Writer.h"
#include "lld/Common/AArch64Instructions.h"
#include "lld/Common/X86Instructions.h"
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
#include "llvm/Support/xxhash.h"
#include "llvm/Transforms/Utils/KCFIHash.h"
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
    : Chunk(k), file(f), header(h), keepUnique(false), resizable(false),
      movable(false), compacts(false), keepDistinct(false),
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

bool isSecRel(Triple::ArchType arch, uint16_t type) {
  switch (arch) {
  case Triple::x86:
    return type == IMAGE_REL_I386_SECREL || type == IMAGE_REL_I386_SECREL7;
  case Triple::x86_64:
    return type == IMAGE_REL_AMD64_SECREL || type == IMAGE_REL_AMD64_SECREL7;
  case Triple::aarch64:
    return (type >= IMAGE_REL_ARM64_SECREL &&
            type <= IMAGE_REL_ARM64_SECREL_LOW12L) ||
           type == IMAGE_REL_LLVM_DTPOFF_LOW12A ||
           type == IMAGE_REL_LLVM_DTPOFF_LOW12L;
  default:
    return false;
  }
}

// A thread-local of the loader's template counts from the template's start,
// whatever output section holds it; any other target from its output section,
// which for the image's template is the template.
static uint64_t getSecRel(const SectionChunk *sec, OutputSection *os,
                          uint64_t s) {
  return sec->file->symtab.ctx.tlsOffset(s, /*loader=*/true)
      .value_or(s - os->getRVA());
}

// Reports, once for each, a thread-local of one template that code of the
// other model reaches, how being the second model's.
static void reportTlsModelMismatch(COFFLinkerContext &ctx, Symbol *sym,
                                   bool loader, InputFile *from,
                                   const Twine &how) {
  {
    std::lock_guard<std::mutex> lock(ctx.tlsMismatchMutex);
    if (!ctx.tlsMismatched.insert(sym).second)
      return;
  }
  auto *d = dyn_cast<DefinedRegular>(sym);
  Err(ctx) << "thread-local model mismatch: " << toString(ctx, *sym)
           << "\n>>> defined in the " << (loader ? "loader's" : "image's")
           << " template by " << (d ? toString(d->getFile()) : "the linker")
           << "\n>>> " << how << " by " << from;
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
  if (isRel32Insn(type))
    return addRel32(off, s - p - 4 - (type & 15));
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
  case IMAGE_REL_LLVM_LEA:
  case IMAGE_REL_LLVM_LEA_OFFSET:
  case IMAGE_REL_LLVM_LEA_REX2:
  case IMAGE_REL_LLVM_LEA_OFFSET_REX2:
  case IMAGE_REL_LLVM_LEA_NARROW:
  case IMAGE_REL_LLVM_GOTTPOFF32:
  case IMAGE_REL_AMD64_REL32:    return addRel32(off, s - p - 4);
  // A fused test's field is before the byte of its immediate.
  case IMAGE_REL_LLVM_FUSED_TEST:
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
  // The field of a load of a pointer holds no addend: it says whether the
  // flags are dead at the load, and a fused read's how it is disposed of. That
  // of a jmp that may be deleted says whether a jcc jumps over it.
  case IMAGE_REL_LLVM_MEM_JUMP_TAIL:
  case IMAGE_REL_LLVM_MEM_LOAD:
  case IMAGE_REL_LLVM_MEM_LOAD_REX2:
  case IMAGE_REL_LLVM_MEM_LOAD_CALL:
  case IMAGE_REL_LLVM_FUSED_READ:
  case IMAGE_REL_LLVM_FUSED_READ_REX2:
    write32le(off, s - p - 4);
    return isInt<32>(int64_t(s - p - 4));
  // s is the offset from the thread pointer, which a displacement holds.
  case IMAGE_REL_LLVM_TPOFF32:
    if (s > INT32_MAX)
      return false;
    add32(off, s);
    break;
  case IMAGE_REL_AMD64_SREL32:
  case IMAGE_REL_AMD64_EHANDLER:
  case IMAGE_REL_LLVM_FUSED_ACCESS:
  case IMAGE_REL_LLVM_FUSED_INDEX:
  case IMAGE_REL_LLVM_FUSED_TEST_REG:
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
  int64_t imm = getPageOffsetImm(orig, IMAGE_REL_ARM64_REL21);
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
uint32_t getArm64LdrScale(uint32_t insn) {
  uint32_t size = insn >> 30;
  // 0x04000000 indicates SIMD/FP registers
  // 0x00800000 indicates 128 bit
  if ((insn & 0x4800000) == 0x4800000)
    size += 4;
  return size;
}

int64_t getPageOffsetImm(uint32_t insn, uint16_t type) {
  if (type == IMAGE_REL_ARM64_PAGEBASE_REL21 || type == IMAGE_REL_ARM64_REL21)
    return SignExtend64<21>((insn >> 29 & 3) | (insn >> 3 & 0x1ffffc));
  uint32_t imm = (insn >> 10) & 0xfff;
  if (type == IMAGE_REL_ARM64_PAGEOFFSET_12L)
    return SignExtend64<12>(imm << getArm64LdrScale(insn));
  return imm;
}

// The length of the LEB128 field at the start of field, or 0 if it does not
// end in it.
static unsigned getLEB128Size(ArrayRef<uint8_t> field) {
  const uint8_t *last =
      llvm::find_if(field, [](uint8_t c) { return !(c & 0x80); });
  return last == field.end() ? 0 : last - field.begin() + 1;
}

bool spanFits(uint16_t type, int64_t v, ArrayRef<uint8_t> field,
              bool &aligned) {
  aligned = true;
  switch (type) {
  case IMAGE_REL_LLVM_SSPAN8:
    return isInt<8>(v);
  case IMAGE_REL_LLVM_USPAN8:
    return isUInt<8>(v);
  case IMAGE_REL_LLVM_SSPAN16:
    return isInt<16>(v);
  case IMAGE_REL_LLVM_USPAN16:
    return isUInt<16>(v);
  case IMAGE_REL_LLVM_SSPAN32:
    return isInt<32>(v);
  case IMAGE_REL_LLVM_USPAN8_W:
    aligned = !(v & 3);
    return isUInt<10>(v);
  case IMAGE_REL_LLVM_USPAN16_W:
    aligned = !(v & 3);
    return isUInt<18>(v);
  case IMAGE_REL_LLVM_SSPAN_WORDS18:
    aligned = !(v & 3);
    return isUInt<20>(v);
  // A LEB128 keeps its assembled length.
  case IMAGE_REL_LLVM_SSPAN_ULEB: {
    unsigned size = getLEB128Size(field);
    return size && v >= 0 && isUIntN(7 * size, v);
  }
  case IMAGE_REL_LLVM_SSPAN_SLEB: {
    unsigned size = getLEB128Size(field);
    return size && isIntN(7 * size, v);
  }
  case IMAGE_REL_LLVM_SSPAN_EPILOGV2:
    return isUInt<12>(v);
  // A page offset is a byte offset that stays in the page.
  case IMAGE_REL_ARM64_PAGEOFFSET_12A:
    return isUInt<12>(v);
  case IMAGE_REL_ARM64_PAGEOFFSET_12L:
    aligned = !(v & ((1 << getArm64LdrScale(read32le(field.data()))) - 1));
    return isUInt<12>(v);
  default:
    return true;
  }
}

int64_t readSpan(uint16_t type, ArrayRef<uint8_t> field) {
  switch (type) {
  case IMAGE_REL_LLVM_SSPAN8:
    return int8_t(field[0]);
  case IMAGE_REL_LLVM_USPAN8:
    return field[0];
  case IMAGE_REL_LLVM_SSPAN16:
    return int16_t(read16le(field.data()));
  case IMAGE_REL_LLVM_USPAN16:
    return read16le(field.data());
  case IMAGE_REL_LLVM_SSPAN64:
    return read64le(field.data());
  case IMAGE_REL_LLVM_SSPAN_ULEB:
    return decodeULEB128(field.data(), nullptr, field.end());
  case IMAGE_REL_LLVM_SSPAN_SLEB:
    return decodeSLEB128(field.data(), nullptr, field.end());
  case IMAGE_REL_LLVM_SSPAN_EPILOGV2:
    return field[0] | (field[1] >> 4) << 8;
  default:
    return int32_t(read32le(field.data()));
  }
}

void writeSpan(uint16_t type, uint8_t *off, int64_t v,
               ArrayRef<uint8_t> field) {
  uint32_t insn = read32le(off);
  switch (type) {
  case IMAGE_REL_AMD64_SSPAN32:
  case IMAGE_REL_LLVM_SSPAN32:
    write32le(off, v);
    return;
  case IMAGE_REL_LLVM_SSPAN8:
  case IMAGE_REL_LLVM_USPAN8:
    *off = v;
    return;
  case IMAGE_REL_LLVM_SSPAN16:
  case IMAGE_REL_LLVM_USPAN16:
    write16le(off, v);
    return;
  // The word-scaled spans count instructions.
  case IMAGE_REL_LLVM_USPAN8_W:
    *off = v >> 2;
    return;
  case IMAGE_REL_LLVM_USPAN16_W:
    write16le(off, v >> 2);
    return;
  case IMAGE_REL_LLVM_SSPAN_WORDS18:
    write32le(off, (insn & ~0x3ffff) | v >> 2);
    return;
  case IMAGE_REL_LLVM_SSPAN64:
    write64le(off, v);
    return;
  case IMAGE_REL_LLVM_SSPAN_ULEB:
    encodeULEB128(v, off, getLEB128Size(field));
    return;
  case IMAGE_REL_LLVM_SSPAN_SLEB:
    encodeSLEB128(v, off, getLEB128Size(field));
    return;
  // The offset's low byte is the unwind code's first byte, and its high
  // four bits are the top of the second.
  case IMAGE_REL_LLVM_SSPAN_EPILOGV2:
    off[0] = v;
    off[1] = (off[1] & 0xf) | (v >> 8) << 4;
    return;
  }
  uint32_t scale =
      type == IMAGE_REL_ARM64_PAGEOFFSET_12L ? getArm64LdrScale(insn) : 0;
  write32le(off, (insn & ~(0xfff << 10)) | (v >> scale) << 10);
}

void applyArm64Ldr(uint8_t *off, uint64_t imm) {
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
  case IMAGE_REL_LLVM_GOTTPOFF_PAGE21:
  case IMAGE_REL_ARM64_PAGEBASE_REL21: applyArm64Addr(off, s, p, 12); break;
  case IMAGE_REL_ARM64_REL21:          applyArm64Addr(off, s, p, 0); break;
  case IMAGE_REL_ARM64_PAGEOFFSET_12A: applyArm64Imm(off, s & 0xfff, 0); break;
  case IMAGE_REL_LLVM_FUSED_READ:
  case IMAGE_REL_LLVM_GOTTPOFF_LOW12L:
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
  // The offset from the image's base, or from the thread pointer, plus the
  // addend in the field, checked to fit as ELF's DTPREL_LO12 and TPREL_LO12.
  case IMAGE_REL_LLVM_DTPOFF_LOW12A:
  case IMAGE_REL_LLVM_DTPOFF_LOW12L:
    if (!checkSecRel(this, os))
      break;
    s = getSecRel(this, os, s);
    [[fallthrough]];
  case IMAGE_REL_LLVM_TPOFF_LOW12A:
  case IMAGE_REL_LLVM_TPOFF_LOW12L: {
    uint32_t insn = read32le(off);
    bool load = type == IMAGE_REL_LLVM_DTPOFF_LOW12L ||
                type == IMAGE_REL_LLVM_TPOFF_LOW12L;
    uint32_t scale = load ? getArm64LdrScale(insn) : 0;
    if (s + (((insn >> 10) & 0xfff) << scale) > 0xfff)
      return false;
    load ? applyArm64Ldr(off, s) : applyArm64Imm(off, s, 0);
    break;
  }
  case IMAGE_REL_LLVM_TPOFF_LOW12A_NC:
    applyArm64Imm(off, s & 0xfff, 0);
    break;
  case IMAGE_REL_LLVM_TPOFF_LOW12L_NC:
    applyArm64Ldr(off, s & 0xfff);
    break;
  case IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL:
  case IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL:
  case IMAGE_REL_LLVM_FUSED_INDEX:
  case IMAGE_REL_LLVM_FUSED_CALL:
  case IMAGE_REL_LLVM_FUSED_JUMP:
  case IMAGE_REL_LLVM_FUSED_JUMP_TAIL:
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
  if (ScannedSection *s = getScanned())
    return writeScanned(*this, buf, *s);
  // Copy section contents from source object file to output file.
  ArrayRef<uint8_t> a = getContents().drop_front(getLeftOut());
  if (!a.empty())
    memcpy(buf + getLeftOut(), a.data(), a.size());

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
    applyRelocation(buf + rel.VirtualAddress, rel, rel.VirtualAddress, nullptr);
  }
  writeSpans(*this, buf);
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

Symbol *SectionChunk::getTarget(size_t i, const ScannedSection *s) const {
  return s ? s->relocs[i].sym : getRelocTarget(getRelocs()[i]);
}

Symbol *SectionChunk::getRelocTarget(const coff_relocation &rel) const {
  if (relocationNamesNoSymbol(rel.Type) ||
      (isPair(rel.Type) && !file->forkObject))
    return nullptr;
  return file->getSymbol(rel.SymbolTableIndex);
}

// Returns whether a relocation of the given type at off in sc, naming a
// function override's stub, takes the stub rather than the body: debug info
// and unwind data describe the body, and an addend points into it. The scan
// binds a site to the body. The field of a load of a pointer on x86-64 holds
// no addend, and a JUMP8's is one byte.
static bool referencesStub(const SectionChunk *sc, uint16_t type,
                           const uint8_t *off) {
  StringRef name = sc->getSectionName();
  if (name.starts_with(".debug") || name.starts_with(".pdata") ||
      name.starts_with(".xdata"))
    return false;
  if (sc->getArch() != Triple::aarch64)
    return type == IMAGE_REL_LLVM_MEM_LOAD ||
           type == IMAGE_REL_LLVM_MEM_LOAD_REX2 ||
           type == IMAGE_REL_LLVM_MEM_LOAD_CALL ||
           type == IMAGE_REL_LLVM_FUSED_READ ||
           type == IMAGE_REL_LLVM_FUSED_READ_REX2 ||
           (type == IMAGE_REL_AMD64_ADDR64 ? read64le(off)
            : type == IMAGE_REL_LLVM_JUMP8 ? *off
                                           : read32le(off)) == 0;
  switch (type) {
  case IMAGE_REL_ARM64_BRANCH26:
  case IMAGE_REL_LLVM_BRANCH_TAIL:
    return (read32le(off) & 0x3ffffff) == 0;
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

void SectionChunk::applyRelocation(uint8_t *off, const coff_relocation &rel,
                                   uint32_t offset, const Relocation *r) const {
  // ABSOLUTE writes nothing; a PAIR is applied with the relocation before it.
  // The scan rewrites a TLS_SCALE site, which only an object that LLVM made
  // for Windows Itanium or NT-POSIX may hold.
  COFFLinkerContext &ctx = file->symtab.ctx;
  if (relocationNamesNoSymbol(rel.Type)) {
    if (rel.Type == IMAGE_REL_LLVM_TLS_BASE && !file->forkObject)
      Err(ctx) << file << ": IMAGE_REL_LLVM_TLS_BASE relocation in "
               << getSectionName()
               << " of an object that LLVM did not make for Windows Itanium "
                  "or NT-POSIX";
    return;
  }
  if (isPair(rel.Type)) {
    if (&rel == relocsData || getPair(*(&rel - 1)) != &rel)
      Err(ctx) << file << ": PAIR relocation in " << getSectionName()
               << " follows no relocation it pairs with";
    return;
  }
  // The relocation of a fused site that has no field marks it, and writes
  // nothing whatever became of the pointer it names.
  if (getFusedMarkerUse(getMachine(), rel.Type) != FusedUse::None)
    return;

  // A symbol is discarded if it was discarded early, when it is null, or
  // late, when its chunk has no output section, unless it is an absolute or
  // synthetic symbol.
  auto isDiscarded = [&](Defined *d) {
    Chunk *c = d ? d->getChunk() : nullptr;
    return !d || (!(c && ctx.getOutputSection(c)) && !isa<DefinedAbsolute>(d) &&
                  !isa<DefinedSynthetic>(d));
  };
  // An initial-exec access reads the word of its thread-local's offset from
  // the thread pointer that the link makes, where it makes one.
  bool gotTpoff = rel.Type >= IMAGE_REL_LLVM_GOTTPOFF32 &&
                  rel.Type <= IMAGE_REL_LLVM_GOTTPOFF_LOW12L;
  Symbol *target = r ? r->sym : file->getSymbol(rel.SymbolTableIndex);
  if (Symbol *word = gotTpoff ? file->symtab.tpoffWords.lookup(target) : nullptr)
    target = word;
  auto *sym = dyn_cast_or_null<Defined>(target);
  if (FunctionOverrideThunk *t = FunctionOverrideThunk::get(sym))
    if (!referencesStub(this, rel.Type, off))
      sym = t->fo.body;
  const coff_relocation *pair = getPair(rel);
  auto *b = pair ? dyn_cast_or_null<Defined>(
                       r ? r[1].sym : file->getSymbol(pair->SymbolTableIndex))
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

  // A target inside a resized section, a symbol plus an addend, names the
  // point the addend has in the object: the symbol moved, and so may have the
  // bytes between it and the point. A PC-relative addend compensates the
  // place instead.
  auto *dr = dyn_cast<DefinedRegular>(sym);
  ScannedSection *ts =
      dr && dr->getChunk()->resizable ? dr->getChunk()->getScanned() : nullptr;
  if (ts && ts->aux && ts->aux->resized && !pair) {
    int64_t addend = 0;
    if (getArch() == Triple::x86_64 ? rel.Type == IMAGE_REL_AMD64_ADDR64
                                    : rel.Type == IMAGE_REL_ARM64_ADDR64)
      addend = read64le(off);
    else if (getArch() == Triple::x86_64
                 ? rel.Type == IMAGE_REL_AMD64_ADDR32 ||
                       rel.Type == IMAGE_REL_AMD64_ADDR32NB ||
                       rel.Type == IMAGE_REL_AMD64_SECREL
                 : rel.Type == IMAGE_REL_ARM64_ADDR32 ||
                       rel.Type == IMAGE_REL_ARM64_ADDR32NB ||
                       rel.Type == IMAGE_REL_ARM64_SECREL)
      addend = SignExtend64<32>(read32le(off));
    if (addend)
      s = dr->getChunk()->getRVA() - addend +
          dr->getChunk()->getOutputOffset(getInputOffset(*ts, dr) + addend);
  }

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
    if (rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12A ||
        rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L)
      v += getPageOffsetImm(read32le(off), rel.Type);
    bool aligned;
    ArrayRef<uint8_t> field = getContents().drop_front(rel.VirtualAddress);
    if (!spanFits(rel.Type, v, field, aligned) || !aligned) {
      Err(ctx) << file << ": "
               << file->getCOFFObj()->getRelocationTypeName(rel.Type)
               << " relocation against " << toString(ctx, *sym)
               << " with a PAIR against " << toString(ctx, *b)
               << (aligned ? " is out of range" : " is misaligned");
      return;
    }
    writeSpan(rel.Type, off, v, field);
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

  // Code of the model reaches its thread-locals through the thread pointer;
  // _tls_used's index field names _tls_index as data.
  if (file->tlsModel && (header->Characteristics & IMAGE_SCN_CNT_CODE) &&
      sym->getName() == "_tls_index") {
    Err(ctx) << file << ": code of the " << ctx.tlsRuntime()
             << " thread-local model names _tls_index; its thread-locals are "
                "in the image's template";
    return;
  }

  // Code reaches only its own model's template; debug data describes both.
  bool tpoff = (rel.Type >= IMAGE_REL_LLVM_TPOFF32 &&
                rel.Type <= IMAGE_REL_LLVM_TPOFF_LOW12L_NC) ||
               rel.Type == IMAGE_REL_LLVM_TPOFF_LOW12A ||
               rel.Type == IMAGE_REL_LLVM_TPOFF_LOW12L ||
               (gotTpoff && !isa<DefinedLocalImport>(sym));
  if ((tpoff || isSecRel(getArch(), rel.Type)) && !isCodeView() && !isDWARF() &&
      ctx.tlsOffset(s, file->tlsModel)) {
    reportTlsModelMismatch(ctx, sym, file->tlsModel, file,
                           file->tlsModel ? "reached from the thread pointer"
                                          : "reached through _tls_index");
    return;
  }

  // A thread-pointer offset is the target's offset in the image's template
  // plus the image's place.
  if (tpoff) {
    if (ctx.tlsWide && !ctx.config.dll) {
      if (!ctx.tlsWideReported.exchange(true))
        Err(ctx) << "TPOFF relocations need a thread-local place, which an "
                 << "executable has only for thread-locals of up to "
                 << TP_BLOCK << " bytes; it has " << ctx.tlsSize;
      return;
    }
    std::optional<uint64_t> offset = ctx.tlsOffset(s);
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

  // The offset itself, as ELF relaxes initial-exec to local-exec: an x86-64
  // mov or add of the word becomes one of an immediate, and an ARM64 adrp and
  // load of it a movz and a movk of the load's register.
  if (gotTpoff && tpoff) {
    uint32_t insn = read32le(off);
    if (getArch() == Triple::x86_64
            ? (off[-3] & 0xfa) != 0x48 || (off[-2] != 0x8b && off[-2] != 0x03)
            : rel.Type == IMAGE_REL_LLVM_GOTTPOFF_LOW12L &&
                  (insn >> 5 & 31) != (insn & 31)) {
      Err(ctx) << file << ": "
               << file->getCOFFObj()->getRelocationTypeName(rel.Type)
               << " relocation against " << toString(ctx, *sym) << " in "
               << getSectionName()
               << (getArch() == Triple::x86_64
                       ? " is not on a 64-bit mov or add"
                       : " is on a load into another register than its "
                         "address's");
      return;
    }
    if (getArch() == Triple::x86_64) {
      off[-3] = 0x48 | (off[-3] >> 2 & 1);
      off[-1] = 0xc0 | (off[-1] >> 3 & 7);
      off[-2] = off[-2] == 0x8b ? 0xc7 : 0x81;
      write32le(off, s);
    } else {
      write32le(off, (rel.Type == IMAGE_REL_LLVM_GOTTPOFF_PAGE21
                          ? 0xd2a00000 | (s >> 16 & 0xffff) << 5
                          : 0xf2800000 | (s & 0xffff) << 5) |
                         (insn & 31));
    }
    return;
  }

  // Compute the RVA of the relocation for relative relocations.
  uint64_t p = rva + offset;
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
      (rel.Type == IMAGE_REL_ARM64_BRANCH26 ||
       rel.Type == IMAGE_REL_LLVM_BRANCH_TAIL ||
       rel.Type == IMAGE_REL_ARM64_BRANCH19 ||
       rel.Type == IMAGE_REL_ARM64_BRANCH14)) {
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
    fits = applyRelARM64(off, rel.Type, os, s, p, imageBase);
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
    applyRelocation(&buf[rel.VirtualAddress - vaBegin], rel, rel.VirtualAddress,
                    nullptr);
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
  ArrayRef<coff_relocation> rels = getRelocs();
  ScannedSection *s = rels.empty() ? nullptr : getScanned();
  for (size_t i = 0, e = rels.size(); i != e; ++i) {
    const coff_relocation &rel = rels[i];
    uint8_t ty = getBaserelType(rel, getArch());
    if (ty == IMAGE_REL_BASED_ABSOLUTE)
      continue;
    // The loader writes an absolute address into an in-place import slot,
    // and the image's fill into a word of its own, which nothing may then
    // adjust.
    Symbol *target = getTarget(i, s);
    if (!isa_and_nonnull<Defined>(target) || isa<DefinedAbsolute>(target) ||
        (s && (s->relocs[i].expr == R_IMPORT_SLOT ||
               s->relocs[i].expr == R_FILL_WORD)))
      continue;
    res->emplace_back(rva + getOutputOffset(rel.VirtualAddress), ty);
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
  // An object for Windows Itanium or NT-POSIX takes none, and the relocation
  // scan reported each reference that would need one.
  if (file->forkObject)
    return;
  ArrayRef<coff_relocation> rels = getRelocs();
  ScannedSection *s = rels.empty() ? nullptr : getScanned();
  for (size_t i = 0, e = rels.size(); i != e; ++i) {
    const coff_relocation &rel = rels[i];
    auto *target = dyn_cast_or_null<Defined>(getRelocTarget(rel));
    // The loader binds an in-place import slot, the image's fill writes a
    // word of its own, and code that the scan made load the address reads
    // the import's own entry.
    if (!target || !target->isRuntimePseudoReloc ||
        (s && (s->relocs[i].expr == R_IMPORT_SLOT ||
               s->relocs[i].expr == R_FILL_WORD ||
               s->relocs[i].expr == R_IMPORT_LOAD)))
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
  if ((linePlacesBarred | other->linePlacesBarred) != 15)
    linePlacesBarred |= other->linePlacesBarred;
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
  if (near()) {
    write32le(buf, 0x58000010); // ldr  x16, the pointer
    applyArm64Branch19(buf, impSymbol->getRVA() - rva);
    memcpy(buf + 4, importThunkARM64 + 8, 4);
    return;
  }
  memcpy(buf, importThunkARM64, sizeof(importThunkARM64));
  applyArm64Addr(buf, impSymbol->getRVA(), rva, 12);
  applyArm64Ldr(buf + 4, off);
}

bool ImportThunkChunkARM64::near() const { return !far && ctx.hasForkObject; }

bool ImportThunkChunkARM64::verifyRanges() {
  return !near() || isInt<21>(impSymbol->getRVA() - rva);
}

uint32_t ImportThunkChunkARM64::extendRanges() {
  if (verifyRanges())
    return 0;
  far = true;
  return 4;
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

// The RVA of a point stored by its chunk and its offset there in the input.
static uint32_t getOutputRVA(const Chunk *c, uint32_t offset) {
  auto *sc = dyn_cast<SectionChunk>(c);
  return c->getRVA() + (sc ? sc->getOutputOffset(offset) : offset);
}

size_t RVATableChunk::write(uint8_t *buf) const {
  ulittle32_t *begin = reinterpret_cast<ulittle32_t *>(buf);
  size_t cnt = 0;
  for (const ChunkAndOffset &co : syms)
    begin[cnt++] = getOutputRVA(co.inputChunk, co.offset);
  llvm::sort(begin, begin + cnt);
  ulittle32_t *end = std::unique(begin, begin + cnt);
  std::fill(end, begin + cnt, 0);
  return end - begin;
}

size_t RVAFlagTableChunk::write(uint8_t *buf) const {
  struct RVAFlag {
    ulittle32_t rva;
    uint8_t flag;
  };
  auto flags =
      MutableArrayRef(reinterpret_cast<RVAFlag *>(buf), syms.size());
  for (auto t : zip(syms, flags)) {
    const auto &sym = std::get<0>(t);
    auto &flag = std::get<1>(t);
    flag.rva = getOutputRVA(sym.inputChunk, sym.offset);
    flag.flag = this->flags.lookup(sym);
    // An export is suppressed where it is 16-byte aligned in the image.
    if (flag.flag == uint8_t(GuardFidFlags::EXPORT_SUPPRESSED) && flag.rva % 16)
      flag.flag = 0;
  }
  llvm::sort(flags,
             [](const RVAFlag &a, const RVAFlag &b) { return a.rva < b.rva; });
  // An address is suppressed where every symbol at it is.
  RVAFlag *end = flags.begin();
  for (const RVAFlag &f : flags) {
    if (end != flags.begin() && end[-1].rva == f.rva)
      end[-1].flag &= f.flag;
    else
      *end++ = f;
  }
  memset(static_cast<void *>(end), 0, (flags.end() - end) * sizeof(RVAFlag));
  return end - flags.begin();
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

TlsRecordChunk::TlsRecordChunk(SymbolTable &symtab, Symbol *sym, Symbol *var,
                               InputFile *file)
    : NonSectionChunk(TlsRecordKind), sym(sym), var(var), file(file),
      live(!symtab.ctx.config.doGC), symtab(symtab) {
  setAlignment(8);
}

TlsRecordChunk *TlsRecordChunk::get(const Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedSynthetic>(s);
  return d ? dyn_cast_or_null<TlsRecordChunk>(d->getChunk()) : nullptr;
}

// An export, a root or an object that names X$tls sets the symbol's use.
// Code names __imp_X$tls, whose local import pointer is read or kept as the
// scan left it, and a load of the pointer that the scan made the address of
// the record names the record; before LTO a reference to either name stands
// for them.
bool TlsRecordChunk::isNamed(bool beforeLTO) const {
  if (sym->isUsedInRegularObj || named)
    return true;
  Symbol *imp = symtab.find(("__imp_" + sym->getName()).str());
  if (beforeLTO)
    return sym->forkReferenced || imp;
  auto *li = dyn_cast_or_null<DefinedLocalImport>(imp);
  return li && li->getTarget() == sym && li->getChunk()->isLive();
}

void TlsRecordChunk::getBaserels(std::vector<Baserel> *res) {
  if (named)
    res->emplace_back(getRVA(), symtab.ctx.config.machine);
}

void TlsRecordChunk::writeTo(uint8_t *buf) const {
  if (!named)
    return;
  COFFLinkerContext &ctx = symtab.ctx;
  // The offsets count from the start of the image's template.
  auto offset = [&](Symbol *s) -> uint32_t {
    auto *d = dyn_cast_or_null<DefinedRegular>(s ? s->getDefined() : nullptr);
    if (std::optional<uint64_t> o =
            d ? ctx.tlsOffset(d->getRVA()) : std::nullopt)
      return *o;
    if (d && ctx.tlsOffset(d->getRVA(), /*loader=*/true))
      reportTlsModelMismatch(ctx, s, true, file,
                             "reached through its record " +
                                 toString(ctx, *sym));
    else
      Err(ctx) << "thread-local record " << toString(ctx, *sym) << ": "
               << (s ? toString(ctx, *s) + " is"
                     : "the guard of " + toString(ctx, *var) +
                           ", which no object states, is")
               << " not in the thread-local data of the image";
    return 0;
  };
  // The record names the image's place word.
  if (TlsPlaceChunk *place = symtab.placeWord)
    write64le(buf, place->getRVA() + ctx.config.imageBase);
  write32le(buf + 8, offset(var));
  write32le(buf + 12, offset(guard));
}

// A wide executable's word holds its wide word's place, H, and its key until
// the runtime writes it; a wide DLL's key is its fill's, and a direct
// image's 0.
void TlsPlaceChunk::writeTo(uint8_t *buf) const {
  bool exe = ctx.tlsWide && !ctx.config.dll;
  write64le(buf + TP_PLACE_AT,
            exe ? TP_H : ctx.tlsPlace.value_or(TP_UNFILLED));
  write64le(buf + TP_FORM_AT,
            ctx.tlsWide ? TP_FORM_WIDE : TP_FORM_DIRECT);
  write64le(buf + TP_KEY_AT, ctx.tlsWide ? TP_UNFILLED : 0);
}

FunctionOverride *FunctionOverride::get(const Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedRegular>(s);
  SectionChunk *c = d ? d->getChunk() : nullptr;
  return c && c->keepDistinct
             ? c->file->symtab.ctx.functionOverrideBodies.lookup(d)
             : nullptr;
}

uint32_t
FunctionOverride::getSiteRVA(std::pair<const Chunk *, uint32_t> site) const {
  auto *sc = dyn_cast<SectionChunk>(site.first);
  return site.first->getRVA() +
         (sc ? getFunctionOverrideField(*sc, site.second) : site.second);
}

FunctionOverrideThunk *FunctionOverrideThunk::get(const Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedSynthetic>(s);
  return d ? dyn_cast_or_null<FunctionOverrideThunk>(d->getChunk()) : nullptr;
}

void FunctionOverrideThunk::writeTo(uint8_t *buf) const {
  // Filler precedes the copy where less than a word is left out.
  memset(buf + getLeftOut(), 0xCC, getEntry() - prefix.size() - getLeftOut());
  buf += getEntry();
  memcpy(buf - prefix.size(), prefix.data(), prefix.size());
  int64_t disp = fo.body->getRVA() - (getRVA() + getEntry());
  if (machine == AMD64) {
    buf[0] = 0xe9; // jmp rel32
    write32le(buf + 1, disp - 5);
  } else {
    write32le(buf, 0x14000000); // b imm26
    applyArm64Branch26(buf, disp);
  }
}

OverrideThunk::OverrideThunk(COFFLinkerContext &ctx, StringRef key,
                             std::optional<uint32_t> typeId, Defined *target,
                             bool moves)
    : ctx(ctx), target(target), moves(moves) {
  setAlignment(16);
  // The key is the marker in hexadecimal and the number of patchable
  // prefix nops, which follow the type ID.
  auto [marker, nops] = key.split('.');
  uint32_t m = 0, n = 0;
  if (typeId && !marker.getAsInteger(16, m) && !nops.getAsInteger(10, n)) {
    prefix.resize(12);
    write64le(prefix.data(), getTypePrefixPattern(m, /*SecondWord=*/false));
    write32le(prefix.data() + 8, *typeId);
    for (; n; --n)
      if (getMachine() == AMD64)
        prefix.push_back(0x90);
      else
        prefix.append({0x1f, 0x20, 0x03, 0xd5});
  }
  leftOut = (getEntry() - prefix.size()) / 4;
}

size_t OverrideThunk::getSize() const {
  bool offer = isa<DefinedImportData>(target);
  return getEntry() +
         (getMachine() == AMD64 ? 5 + offer + moves * 3 : 12 + moves * 4);
}

MachineTypes OverrideThunk::getMachine() const { return ctx.config.machine; }

void OverrideThunk::writeTo(uint8_t *buf) const {
  // Filler precedes the prefix where less than a word is left out.
  memset(buf + getLeftOut(), 0xCC, getEntry() - prefix.size() - getLeftOut());
  buf += getEntry();
  memcpy(buf - prefix.size(), prefix.data(), prefix.size());
  uint64_t p = getRVA() + getEntry(), s = target->getRVA();
  if (getMachine() == AMD64) {
    if (moves) {
      // mov rdx, r8, or by the System V convention mov rsi, rdx.
      memcpy(buf,
             ctx.tlsRuntime() == "ntposix" ? "\x48\x89\xd6" : "\x4c\x89\xc2",
             3);
      buf += 3;
      p += 3;
    }
    bool offer = isa<DefinedImportData>(target);
    memcpy(buf, offer ? "\xff\x25" : "\xe9", 1 + offer); // jmp *, jmp
    write32le(buf + 1 + offer, s - p - 5 - offer);
    return;
  }
  if (moves) {
    write32le(buf, 0xaa0203e1); // mov x1, x2
    buf += 4;
    p += 4;
  }
  if (!isa<DefinedImportData>(target) && isInt<28>(s - p)) {
    write32le(buf, 0x14000000); // b target
    applyArm64Branch26(buf, s - p);
    write32le(buf + 4, 0xd4200000); // brk #0
    write32le(buf + 8, 0xd4200000);
    return;
  }
  // adrp x16, target; add x16, x16, :lo12:target, or for an offer ldr; br x16
  write32le(buf, 0x90000010);
  applyArm64Addr(buf, s, p, 12);
  if (isa<DefinedImportData>(target)) {
    write32le(buf + 4, 0xf9400210);
    applyArm64Ldr(buf + 4, s & 0xfff);
  } else {
    write32le(buf + 4, 0x91000210);
    applyArm64Imm(buf + 4, s & 0xfff, 0);
  }
  write32le(buf + 8, 0xd61f0200);
}

void OverrideAnchorChunk::getBaserels(std::vector<Baserel> *res) {
  for (unsigned i = 0; i != count; ++i)
    if (slots[i])
      res->emplace_back(getRVA() + OverrideAnchorHeaderSize + i * 8,
                        ctx.config.machine);
}

// The table's header and the anchor begin alike.
void OverrideAnchorChunk::writeHeader(uint8_t *buf) const {
  write32le(buf, OverrideTableMagic);
  write16le(buf + 4, OverrideTableFormat);
  write16le(buf + 6, count);
}

void OverrideAnchorChunk::writeTable(uint8_t *page) const {
  for (unsigned i = 0; i != count; ++i)
    if (slots[i]) {
      write32le(page + getSlotOffset(i), OverrideTableSlotTag ^ i);
      write32le(page + getSlotOffset(i) + 4, slots[i]->getRVA());
    }
  writeHeader(page + OverrideTableHeaderOffset);
  write32le(page + OverrideTableHeaderOffset + 8, getRVA());
}

void OverrideAnchorChunk::writeTo(uint8_t *buf) const {
  memset(buf, 0, getSize());
  writeHeader(buf);
  write32le(buf + 8, mask);
  uint32_t offers = 0;
  for (unsigned i = 0; i != count; ++i) {
    if (!slots[i])
      continue;
    offers |= isa<DefinedImportData>(slots[i]) << i;
    write64le(buf + OverrideAnchorHeaderSize + i * 8,
              slots[i]->getRVA() + ctx.config.imageBase);
    buf[OverrideAnchorHeaderSize + count * 8 + i] = calls[i];
  }
  write32le(buf + 20, offers);
  if (OfferTableChunk *t = ctx.symtab.offerTable)
    write32le(buf + 12, t->getRVA());
}

uint64_t OfferTableChunk::getKey(StringRef name) { return xxh3_64bits(name); }

uint32_t OfferTableChunk::getIdentity(ArrayRef<COFFLinkImage> closure,
                                      const COFFLinkImage &image) {
  SmallString<0> s;
  raw_svector_ostream os(s);
  for (const COFFLinkImage::Entity &e : image.Kept)
    os << e.Name << '\0' << e.Flags << ' ' << unsigned(e.Log2Align) << ' '
       << e.Size << ' ' << e.CheckSum << e.Shape << '\0';
  for (uint32_t import : image.Imports)
    os << StringRef(closure[import / 2].Name).lower() << ' ' << import % 2
       << '\0';
  return xxh3_64bits(s);
}

static unsigned getLog2Buckets(size_t entries) {
  return entries ? Log2_64_Ceil(entries) : 0;
}

uint32_t OfferTableChunk::getNamesOffset() const {
  return 16 + 24 * entries.size() +
         4 * ((1 << getLog2Buckets(entries.size())) + 1);
}

size_t OfferTableChunk::getSize() const {
  return getNamesOffset() + names.size();
}

void OfferTableChunk::finalizeContents(COFFLinkerContext &ctx) {
  // An executable's names each image whose link records its link read that
  // the process loads as it starts, with the identity of the record, whose
  // fill so knows that the table names every instance that it shares.
  if (!ctx.config.dll)
    ctx.driver.forEachImageBreadthFirst([&](const COFFLinkImage &image,
                                            bool late,
                                            ArrayRef<COFFLinkImage> closure) {
      if (!late && (!image.Kept.empty() || !image.Imports.empty()))
        entries.push_back({saver().save(image.Name.lower()), nullptr,
                           LinkImageEntityImage, 0,
                           getIdentity(closure, image)});
    });
  ArrayRef<Export> exports = ctx.symtab.exports;
  for (Entry &e : entries) {
    e.key = getKey(e.name);
    auto *imp = dyn_cast_or_null<DefinedImportData>(e.at);
    auto it = llvm::partition_point(
        exports, [&](const Export &x) { return x.exportName < e.name; });
    if (imp && imp->file->hintName && imp->getExternalName() == e.name) {
      e.nameChunk = imp->file->hintName;
      e.nameOffset = 2;
    } else if (it != exports.end() && it->exportName == e.name &&
               it->nameChunk) {
      e.nameChunk = it->nameChunk;
    } else {
      e.nameOffset = names.size();
      names += e.name;
      names += '\0';
    }
  }
  llvm::stable_sort(
      entries, [](const Entry &a, const Entry &b) { return a.key < b.key; });
  for (auto [i, e] : enumerate(entries)) {
    if (e.at)
      indices[e.at] = i;
    if (i && e.key == entries[i - 1].key && e.name != entries[i - 1].name)
      Err(ctx) << e.name << " and " << entries[i - 1].name
               << " have one key in the offer table; rename one of the two";
  }
}

std::pair<uint32_t, uint64_t>
OfferTableChunk::getEntry(const Defined *at) const {
  auto it = indices.find(at);
  assert(it != indices.end() && "every vague cell has an entry");
  return {getRVA() + 16 + 24 * it->second, entries[it->second].key};
}

uint32_t OfferTableChunk::getImageEntry(StringRef name) const {
  auto it = llvm::find_if(entries, [&](const Entry &e) {
    return (e.flags & LinkImageEntityImage) && e.name == name;
  });
  assert(it != entries.end() && "a DLL with a vague cell names itself");
  return getRVA() + 16 + 24 * (it - entries.begin());
}

void OfferTableChunk::writeTo(uint8_t *buf) const {
  unsigned log2 = getLog2Buckets(entries.size());
  write32le(buf, OfferTableMagic);
  write32le(buf + 4, log2);
  write32le(buf + 8, entries.size());
  bool descriptors = llvm::any_of(entries, [](const Entry &e) {
    return e.flags & LinkImageEntityDescriptor;
  });
  write32le(buf + 12, descriptors ? OfferTableDescriptors : 0);
  uint8_t *p = buf + 16;
  for (const Entry &e : entries) {
    write64le(p, e.key);
    write32le(p + 8, (e.nameChunk ? e.nameChunk->getRVA()
                                  : getRVA() + getNamesOffset()) +
                         e.nameOffset);
    write32le(p + 12, e.at ? e.at->getRVA() : 0);
    write32le(p + 16, e.size);
    write16le(p + 20, e.flags);
    p[22] = e.log2Align;
    p[23] = 0;
    p += 24;
  }
  // A bucket begins at its first key whose top bits are its index or more.
  for (size_t b = 0, i = 0, n = size_t(1) << log2; b <= n; ++b, p += 4) {
    while (i != entries.size() && (entries[i].key >> 1 >> (63 - log2)) < b)
      ++i;
    write32le(p, i);
  }
  memcpy(p, names.data(), names.size());
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
    uint8_t type = fo->body->getChunk()->getMachine() == AMD64
                       ? IMAGE_FUNCTION_OVERRIDE_X64_REL32
                       : IMAGE_FUNCTION_OVERRIDE_ARM64_BRANCH26;
    std::vector<Baserel> sites;
    for (auto site : fo->sites)
      sites.emplace_back(fo->getSiteRVA(site), type);
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
