//===- Relocations.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The scan gives each relocation of a fork object, and of an object that
// refers to a local import pointer or an import, its expression, as lld/ELF's
// scan does, and keeps the relocations of each section the link rewrites,
// which Relaxation.cpp writes.
//
//===----------------------------------------------------------------------===//

#include "Relocations.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "DLL.h"
#include "Driver.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Walker.h"
#include "lld/Common/AArch64Instructions.h"
#include "lld/Common/BranchToBranch.h"
#include "lld/Common/X86Instructions.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include <array>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::coff;

//===----------------------------------------------------------------------===//
// Sites: the instructions a relocation type states, checked before a rewrite.
//===----------------------------------------------------------------------===//

bool lld::coff::isX64(const SectionChunk &sc) {
  return sc.getArch() == Triple::x86_64;
}
static bool isCode(const SectionChunk &sc) {
  return sc.header->Characteristics & IMAGE_SCN_CNT_CODE;
}
static bool isLoaded(const SectionChunk &sc) {
  return !(sc.header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE);
}
uint32_t lld::coff::read32(const SectionChunk &sc, uint32_t o) {
  return read32le(&sc.getContents()[o]);
}
bool lld::coff::isRead(uint16_t t) {
  return t == IMAGE_REL_LLVM_FUSED_READ || t == IMAGE_REL_LLVM_FUSED_READ_REX2;
}
bool lld::coff::isLoad(uint16_t t) {
  return isRead(t) || t == IMAGE_REL_LLVM_MEM_LOAD ||
         t == IMAGE_REL_LLVM_MEM_LOAD_REX2;
}
static bool isRex2(uint16_t t) {
  return t == IMAGE_REL_LLVM_MEM_LOAD_REX2 ||
         t == IMAGE_REL_LLVM_FUSED_READ_REX2 || t == IMAGE_REL_LLVM_LEA_REX2 ||
         t == IMAGE_REL_LLVM_LEA_OFFSET_REX2;
}
// A typed lea of 64 bits relative to rip, of a symbol plus the offset its
// field holds or of the symbol alone.
static bool isLeaOffset(uint16_t t) {
  return t == IMAGE_REL_LLVM_LEA_OFFSET || t == IMAGE_REL_LLVM_LEA_OFFSET_REX2;
}
static bool isLea(uint16_t t) {
  return isLeaOffset(t) || t == IMAGE_REL_LLVM_LEA ||
         t == IMAGE_REL_LLVM_LEA_REX2;
}
bool lld::coff::isMemJump(uint16_t t) {
  return t == IMAGE_REL_LLVM_MEM_JUMP || t == IMAGE_REL_LLVM_MEM_JUMP_TAIL ||
         t == IMAGE_REL_LLVM_MEM_JUMP_P1;
}
// An x86-64 call or jump through a pointer.
static bool isPointerBranch(uint16_t t) {
  return isMemJump(t) || t == IMAGE_REL_LLVM_MEM_CALL ||
         t == IMAGE_REL_LLVM_MEM_LOAD_CALL;
}
FusedUse lld::coff::getUse(const SectionChunk &sc, uint16_t type) {
  return getFusedMarkerUse(sc.getMachine(), type);
}

namespace {
// The instructions a type states: on x86-64 the bytes before its field and
// those from it on, the bytes before having mask and value, the first in the
// low byte, and its field holding no addend but the bits of field; on ARM64
// the instruction at the field having mask and value. A mask of zero is
// checked apart.
struct SiteForm {
  uint16_t type;
  bool x64;
  uint8_t before, from;
  uint32_t mask, value, field;
};
} // namespace

// clang-format off
static constexpr SiteForm siteForms[] = {
  // mov r64, [rip+P] behind REX.W or REX2; the call through it after the load.
  {IMAGE_REL_LLVM_MEM_LOAD,        1, 3, 4, 0xc7fff8,   0x058b48,   LoadFlagsDead},
  {IMAGE_REL_LLVM_MEM_LOAD_REX2,   1, 4, 4, 0xc7ff08ff, 0x058b08d5, LoadFlagsDead},
  {IMAGE_REL_LLVM_FUSED_READ,      1, 3, 4, 0xc7fff8,   0x058b48,   0x3f},
  {IMAGE_REL_LLVM_FUSED_READ_REX2, 1, 4, 4, 0xc7ff08ff, 0x058b08d5, 0x3f},
  {IMAGE_REL_LLVM_MEM_LOAD_CALL,   1, 3, 6, 0xc7fffb,   0x058b48,   0},
  // call or jmp [rip+P], after one prefix byte; cmp qword [rip+P], 0.
  {IMAGE_REL_LLVM_MEM_CALL,        1, 2, 4, 0xffff,     0x15ff,     0},
  {IMAGE_REL_LLVM_MEM_JUMP,        1, 2, 4, 0xffff,     0x25ff,     0},
  {IMAGE_REL_LLVM_MEM_JUMP_TAIL,   1, 2, 4, 0xffff,     0x25ff,     JumpOverJcc},
  {IMAGE_REL_LLVM_MEM_JUMP_P1,     1, 3, 4, 0xffff00,   0x25ff00,   0},
  {IMAGE_REL_LLVM_FUSED_TEST,      1, 3, 5, 0xffffff,   0x3d8348,   0},
  {IMAGE_REL_LLVM_BRANCH_TAIL,     1, 1, 4, 0xff,       0xe9,       ~0u},
  // A jump of either form, and the instructions a decoder reads.
  {IMAGE_REL_LLVM_JUMP8,           1, 1, 1, 0, 0, 0},
  {IMAGE_REL_LLVM_FUSED_ADD,       1, 0, 0, 0, 0, 0},
  {IMAGE_REL_LLVM_FUSED_TEST_REG,  1, 0, 3, 0, 0, 0},
  {IMAGE_REL_LLVM_FUSED_ACCESS,    1, 0, 4, 0, 0, 0},
  {IMAGE_REL_LLVM_FUSED_INDEX,     1, 0, 4, 0, 0, 0},
  // blr, br, b, cbz or cbnz, add xd, xn, #imm.
  {IMAGE_REL_LLVM_FUSED_CALL,      0, 0, 4, 0xfffffc1f, 0xd63f0000, 0},
  {IMAGE_REL_LLVM_FUSED_JUMP,      0, 0, 4, 0xfffffc1f, 0xd61f0000, 0},
  {IMAGE_REL_LLVM_FUSED_JUMP_TAIL, 0, 0, 4, 0xfffffc1f, 0xd61f0000, 0},
  {IMAGE_REL_LLVM_BRANCH_TAIL,     0, 0, 4, 0xfc000000, 0x14000000, 0},
  {IMAGE_REL_LLVM_FUSED_TEST,      0, 0, 4, 0xfe000000, 0xb4000000, 0},
  {IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, 0, 0, 4, 0xffc00000, 0x91000000, 0},
};
// clang-format on

static const SiteForm *getSiteForm(bool x64, uint16_t type) {
  static constexpr auto index = [] {
    std::array<uint8_t, 256> a{};
    for (size_t i = 0; i != std::size(siteForms); ++i)
      a[(siteForms[i].type & 0x7f) | siteForms[i].x64 << 7] = i + 1;
    return a;
  }();
  uint8_t i = (type & 0xff80) == 0x8000 ? index[(type & 0x7f) | x64 << 7] : 0;
  return i ? &siteForms[i - 1] : nullptr;
}

// The bytes of an x86-64 site before its relocation and from it on, as its
// type states them; a fused add's lea has the length its index states, and a
// call follows a load through r8 to r11 a byte later.
unsigned lld::coff::getBytesBefore(uint16_t type) {
  const SiteForm *f = getSiteForm(true, type);
  return f ? f->before : isRel32Insn(type) ? type >> 4 & 15 : 2;
}
unsigned lld::coff::getBytesFrom(const SectionChunk &sc,
                                 const coff_relocation &rel) {
  if (rel.Type == IMAGE_REL_LLVM_FUSED_ADD)
    return rel.SymbolTableIndex & 15;
  const SiteForm *f = getSiteForm(true, rel.Type);
  return !f ? 4 + (isRel32Insn(rel.Type) ? rel.Type & 15 : 0)
         : rel.Type == IMAGE_REL_LLVM_MEM_LOAD_CALL
             ? 6 + bool(sc.getContents()[rel.VirtualAddress - 3] & 4)
             : f->from;
}

// What the lea of a FUSED_ADD adds: the zigzag in its index above the length.
int64_t lld::coff::getFusedAddend(const coff_relocation &rel) {
  uint32_t z = rel.SymbolTableIndex >> 4;
  return int64_t(z >> 1) ^ -int64_t(z & 1);
}

// The register the x86-64 load of a pointer with its field at o writes.
unsigned lld::coff::getLoadRegister(const SectionChunk &sc, uint32_t o,
                                    bool rex2) {
  ArrayRef<uint8_t> c = sc.getContents();
  return (c[o - 1] >> 3 & 7) | (c[o - 3] & 4) << 1 |
         (rex2 ? (c[o - 3] & 0x40) >> 2 : 0);
}

static bool isLdr64(uint32_t insn, aarch64::Ldr &l) {
  return aarch64::parseLdr(insn, l) && l.p2Size == 3 && !l.isFloat &&
         l.extendType == aarch64::ZeroExtend;
}
static bool isPointerLdr(uint32_t insn) {
  aarch64::Ldr l;
  return isLdr64(insn, l) && !l.offset;
}

// The padding ALIGN's index asks for at pos, in a section aligned to at
// least it: to the alignment unless that skips more than its most or, in
// boundary mode, the span after it, shrink bytes shorter than assembled,
// would neither cross nor end on a boundary. In line mode, bit 27, the
// object's padding is the span, a loop less its last byte.
uint32_t lld::coff::getAlignPadding(uint32_t index, uint64_t pos, int shrink) {
  unsigned log2 = index & 31, maxSkip = (index >> 9) & 0xfff;
  unsigned span = ((index >> 22) & 31) - shrink;
  if (index >> 27 & 1)
    return span;
  uint32_t pad = -pos & ((uint64_t(1) << log2) - 1);
  if (index >> 21 & 1)
    return span && ((pos >> log2) != ((pos + span - 1) >> log2) ||
                    !((pos + span) & ((uint64_t(1) << log2) - 1)))
               ? pad
               : 0;
  return maxSkip && pad > maxSkip ? 0 : pad;
}

// An error at offset of sc, the place of what it reports.
COFFSyncStream lld::coff::errAt(const SectionChunk &sc, uint32_t offset) {
  COFFSyncStream e = Err(sc.file->symtab.ctx);
  e << sc.file << ": " << sc.getSectionName() << "+0x"
    << Twine::utohexstr(offset) << ": ";
  return e;
}
static void reportSite(const SectionChunk &sc, const coff_relocation &rel,
                       const Twine &problem) {
  errAt(sc, rel.VirtualAddress)
      << sc.file->getCOFFObj()->getRelocationTypeName(rel.Type) << " "
      << problem;
}

// Whether an x86-64 site is read through the decoder.
static bool decodes(const SectionChunk &sc, uint16_t t) {
  return isX64(sc) &&
         (t == IMAGE_REL_LLVM_FUSED_TEST_REG || t == IMAGE_REL_LLVM_FUSED_ADD ||
          t == IMAGE_REL_LLVM_FUSED_ACCESS || t == IMAGE_REL_LLVM_FUSED_INDEX);
}

// Whether the instructions at rel have the form its type states; stays says
// that a fused site has it but no rewrite of it fits, and stays as compiled.
static bool hasForm(const SectionChunk &sc, const coff_relocation &rel,
                    X86Decoder *dec, bool &stays) {
  ArrayRef<uint8_t> c = sc.getContents();
  size_t o = rel.VirtualAddress;
  uint16_t t = rel.Type;
  bool x64 = isX64(sc);
  ArrayRef<uint8_t> b = c.drop_front(o);
  if (b.size() < 4 && t != IMAGE_REL_LLVM_ALIGN &&
      !(x64 && t == IMAGE_REL_LLVM_JUMP8) && !decodes(sc, t))
    return false;
  uint32_t w = b.size() >= 4 ? read32le(b.data()) : 0;
  uint8_t p = o >= 3 ? c[o - 3] : 0, op = o >= 2 ? c[o - 2] : 0,
          modRm = o ? c[o - 1] : 0;
  // A fork type of the other machine states no site of this one.
  if (t >> 15 &&
      !(getLLVMRelocProperties(t) & getLLVMRelocMachine(sc.getMachine())))
    return false;
  if (const SiteForm *f = getSiteForm(x64, t);
      f && f->mask &&
      (x64 ? o < f->before ||
                 (read32le(&c[o - f->before]) & f->mask) != f->value ||
                 (w & ~f->field)
           : (w & f->mask) != f->value))
    return false;
  switch (t) {
  // The wide DLL's form of a base site, which names t and the thread
  // pointer, tp, and on ARM64 the key's register k (spec section 15.1):
  // x86-64 mov t, [rip+P]; mov t, [tp+t] (base and index either way, a zero
  // displacement where the base is rbp or r13); ror t, 17; xor t, [rip+K].
  // ARM64 adrp and ldr of P into t with their relocations; ldr t, [tp, t];
  // adrp and ldr of K into k; eor t, k, t, ror #17.
  case IMAGE_REL_LLVM_TLS_BASE: {
    if (x64) {
      if (b.size() < 22)
        return false;
      unsigned r = (b[0] & 4) << 1 | (b[2] >> 3 & 7), d = b[9] >> 6 == 1;
      unsigned base = (b[7] & 1) << 3 | (b[10] & 7);
      unsigned index = (b[7] & 2) << 2 | (b[10] >> 3 & 7);
      const uint8_t *e = &b[11 + d];
      return b.size() >= 22u + d && (b[0] & 0xfb) == 0x48 && b[1] == 0x8b &&
             (b[2] & 0xc7) == 0x05 && (b[7] & 0xfc) == (b[0] & 0xfc) &&
             b[8] == 0x8b && (b[9] & 0xbf) == (0x04 | (b[2] & 0x38)) &&
             ((base & 7) == 5) == d && (base == r) != (index == r) &&
             index != 4 && (!d || b[11] == 0) && e[0] == (0x48 | r >> 3) &&
             e[1] == 0xc1 && e[2] == (0xc8 | (r & 7)) && e[3] == TP_ROTATE &&
             e[4] == b[0] && e[5] == 0x33 && e[6] == b[2];
    }
    if (b.size() < 24)
      return false;
    aarch64::Adrp a, ak;
    aarch64::Ldr l, lk;
    uint32_t r = w & 31, load = read32le(&b[8]), eor = read32le(&b[20]);
    uint32_t tp = load >> 5 & 31, k = read32le(&b[12]) & 31;
    auto has = [&](uint32_t at, uint16_t type) {
      return llvm::any_of(sc.getRelocs(), [&](const coff_relocation &r) {
        return r.VirtualAddress == at && r.Type == type;
      });
    };
    auto pair = [&](uint32_t at, aarch64::Adrp &p, aarch64::Ldr &q) {
      return aarch64::parseAdrp(read32le(&b[at]), p) &&
             isLdr64(read32le(&b[at + 4]), q) && p.destRegister != 31 &&
             q.baseRegister == p.destRegister &&
             q.destRegister == p.destRegister &&
             has(o + at, IMAGE_REL_ARM64_PAGEBASE_REL21) &&
             has(o + at + 4, IMAGE_REL_ARM64_PAGEOFFSET_12L);
    };
    return pair(0, a, l) && pair(12, ak, lk) && k != r && tp != r && tp != 31 &&
           load == (0xf8606800 | r << 16 | tp << 5 | r) &&
           eor == (0xcac00000 | r << 16 | TP_ROTATE << 10 | k << 5 | r);
  }
  // Padding the link redoes is no-operations.
  case IMAGE_REL_LLVM_ALIGN: {
    uint32_t pad = getAlignPadding(rel.SymbolTableIndex, o);
    if ((1u << (rel.SymbolTableIndex & 31)) > sc.getAlignment() ||
        (rel.SymbolTableIndex >> 27 & 1 && !sc.file->lineLoops.count(&sc)) ||
        pad > b.size() || (!x64 && pad & 3))
      return false;
    for (ArrayRef<uint8_t> n = b.take_front(pad); !n.empty();) {
      unsigned len = x64                                ? x86::getNopLength(n)
                     : read32le(n.data()) == 0xd503201f ? 4
                                                        : 0;
      if (!len)
        return false;
      n = n.drop_front(len);
    }
    return true;
  }
  // A load whose next instruction is a call through its register; a tail
  // jump after a jcc over it, which only a section whose code moves may
  // hold over a jump of any length; a jump after one prefix byte.
  case IMAGE_REL_LLVM_MEM_LOAD_CALL: {
    bool rex = p & 4;
    return o + 6 + rex <= c.size() && (!rex || c[o + 4] == 0x41) &&
           c[o + 4 + rex] == 0xff &&
           c[o + 5 + rex] == (0xd0 | (modRm >> 3 & 7));
  }
  case IMAGE_REL_LLVM_MEM_JUMP_TAIL:
    return !(w & JumpOverJcc) ||
           (o >= 4 && (c[o - 4] & 0xf0) == 0x70 && (sc.movable || p == 6));
  case IMAGE_REL_LLVM_MEM_JUMP_P1:
    return (p & 0xf0) == 0x40 ||
           StringRef("\x26\x2e\x36\x3e\x64\x65\x66\x67\xf0\xf2\xf3")
               .contains(char(p));
  case IMAGE_REL_LLVM_FUSED_READ:
    return x64 || isPointerLdr(w);
  case IMAGE_REL_LLVM_LEA:
  case IMAGE_REL_LLVM_LEA_OFFSET:
  case IMAGE_REL_LLVM_LEA_REX2:
  case IMAGE_REL_LLVM_LEA_OFFSET_REX2:
    return isRipLea64(c, o) && !w == !isLeaOffset(t) &&
           (o >= 4 && c[o - 4] == 0xd5) >= isRex2(t);
  case IMAGE_REL_LLVM_JUMP32:
    return modRm == 0xe9 || (o > 1 && op == 0x0f && (modRm & 0xf0) == 0x80);
  case IMAGE_REL_LLVM_JUMP8:
    return !b.empty() && (modRm == 0xeb || (modRm & 0xf0) == 0x70);
  case IMAGE_REL_LLVM_FUSED_TEST:
    return !x64 || (b.size() >= 5 && !b[4]);
  case IMAGE_REL_LLVM_FUSED_TEST_REG: {
    FusedInsn insn;
    stays = dec->decodeFused(b, insn) && insn.use == FusedUse::Test;
    return stays && insn.size == 3;
  }
  // An access through the register alone, whose form relative to rip is at
  // most fifteen bytes. An index reads the record word its fact states: the
  // TLS index's address as an access, on ARM64 ldr or ldrsw, an offset added
  // unscaled to another register.
  case IMAGE_REL_LLVM_FUSED_ACCESS:
  case IMAGE_REL_LLVM_FUSED_INDEX:
  case IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL: {
    bool index = t == IMAGE_REL_LLVM_FUSED_INDEX;
    size_t i = &rel - sc.getRelocs().data();
    std::optional<uint64_t> fact = sc.file->getSiteFact(&sc, i, SiteFactIndex);
    uint64_t f = fact.value_or(0), word = f & ~uint64_t(SiteFactViaFirst);
    if (index && getChainSite(sc, i) != ChainSite::None)
      return true;
    if (index && (!fact || !getTlsRecordWordSize(word)))
      return false;
    if (!x64)
      return !index  ? (w & 0x3b000000) == 0x39000000 ||
                           (w & 0x3b800000) == 0x29000000
             : !word ? (w & 0xfffffc00) == 0xb9400000 ||
                           (w & 0xfffffc00) == 0xb9800000
                     : (w & 0xffe0fc00) == 0x8b000000 ||
                           (w & 0x3b20fc00) == 0x38206800;
    FusedInsn insn;
    SmallString<16> direct;
    unsigned field;
    stays =
        dec->decodeFused(b, insn, &direct, &field, !(f & SiteFactViaFirst)) &&
        insn.use == (index && word ? FusedUse::Index : FusedUse::Access);
    return stays && !direct.empty() && direct.size() <= 15;
  }
  // lea a(r), r of the stated length and constant, after a load into r;
  // ARM64 add xd, xn, #imm.
  case IMAGE_REL_LLVM_FUSED_ADD:
  case IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL: {
    if (!x64)
      return true;
    const coff_relocation *load = &rel - 1;
    FusedInsn insn;
    return &rel != sc.getRelocs().begin() && o >= 7 &&
           (load->Type == IMAGE_REL_LLVM_MEM_LOAD ||
            load->Type == IMAGE_REL_LLVM_MEM_LOAD_REX2) &&
           load->VirtualAddress + 4 == o && dec->decodeFused(b, insn) &&
           insn.use == FusedUse::Add && insn.size == getBytesFrom(sc, rel) &&
           insn.disp == getFusedAddend(rel) &&
           insn.reg == getLoadRegister(sc, o - 4, isRex2(load->Type));
  }
  default:
    return true;
  }
}

// Returns whether rel's instructions have the form its type states, and
// reports them otherwise. Without a decoder an x86-64 fused site stays.
static bool checkSite(const SectionChunk &sc, const coff_relocation &rel) {
  X86Decoder *dec = X86Decoder::get();
  if (rel.VirtualAddress > sc.getContents().size() ||
      (!dec && decodes(sc, rel.Type)))
    return false;
  if (rel.Type == IMAGE_REL_LLVM_TLS_BASE && !sc.file->tlsModel) {
    reportSite(sc, rel,
               "in an object that does not state the owned thread-local model");
    return false;
  }
  bool stays = false;
  if (hasForm(sc, rel, dec, stays))
    return true;
  if (!stays)
    reportSite(sc, rel, "does not have the instructions its type states");
  return false;
}

//===----------------------------------------------------------------------===//
// The scan.
//===----------------------------------------------------------------------===//

namespace {
// What one object's scan leaves for the decisions after it: the in-place
// slots, the address-takes that name an address inside an import, and the
// guarded sites.
struct ImportSites {
  std::vector<ImportSlot> slots, interior;
  std::vector<DelayGuardSite> guarded;
};
// How one object's live sections use each local import pointer.
using LocalImportUses = MapVector<DefinedLocalImport *, uint8_t>;
} // namespace

// An ARM64 adrp with no addend, or a 64-bit ldr of a whole pointer.
static bool isArm64PointerLoad(const SectionChunk &sc,
                               const coff_relocation &rel) {
  if (rel.VirtualAddress + 4 > sc.getContents().size())
    return false;
  uint32_t insn = read32(sc, rel.VirtualAddress);
  aarch64::Adrp a;
  if (rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21)
    return aarch64::parseAdrp(insn, a) && !a.addend;
  return (rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L ||
          rel.Type == IMAGE_REL_LLVM_FUSED_READ) &&
         isPointerLdr(insn) && !sc.getPair(rel);
}

// Whether the ARM64 adrp and low, the instruction after it, are one address:
// an adrp of no addend into Xd, then an add of no addend or, with ldr, a load
// of no offset, from Xd into Xd (lld/ELF's AArch64Relaxer::tryRelaxAdrpAdd
// test).
bool lld::coff::isAdrpPair(uint32_t adrp, uint32_t low, aarch64::Ldr *ldr) {
  aarch64::Adrp a;
  aarch64::Add b;
  if (!aarch64::parseAdrp(adrp, a) || a.addend || a.destRegister == 31)
    return false;
  if (!ldr)
    return aarch64::parseAdd(low, b) && !b.addend &&
           b.srcRegister == a.destRegister && b.destRegister == a.destRegister;
  return aarch64::parseLdr(low, *ldr) && !ldr->offset && !ldr->isFloat &&
         ldr->baseRegister == a.destRegister &&
         ldr->destRegister == a.destRegister;
}

// Whether adrp and low, consecutive relocations of sc of one symbol, are one
// address or one load of a pointer: an adrp pair of an add, or of the ldr of
// a fused read.
static bool isAddressPair(const SectionChunk &sc, const coff_relocation &adrp,
                          const coff_relocation &low) {
  aarch64::Ldr l;
  bool load = low.Type == IMAGE_REL_LLVM_FUSED_READ;
  return adrp.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
         (load || low.Type == IMAGE_REL_ARM64_PAGEOFFSET_12A) &&
         adrp.VirtualAddress + 4 == low.VirtualAddress &&
         uint64_t(low.VirtualAddress) + 4 <= sc.getContents().size() &&
         adrp.SymbolTableIndex == low.SymbolTableIndex && !sc.getPair(low) &&
         isAdrpPair(read32(sc, adrp.VirtualAddress),
                    read32(sc, low.VirtualAddress), load ? &l : nullptr) &&
         (!load || (l.p2Size == 3 && l.extendType == aarch64::ZeroExtend));
}

// Whether rel is either half of an ARM64 adrp and add proven one address.
static bool isAddressPairPart(const SectionChunk &sc,
                              const coff_relocation &rel) {
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  return (rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
          &rel + 1 != rels.end() &&
          (&rel + 1)->Type == IMAGE_REL_ARM64_PAGEOFFSET_12A &&
          isAddressPair(sc, rel, *(&rel + 1))) ||
         (rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12A && &rel != rels.begin() &&
          isAddressPair(sc, *(&rel - 1), rel));
}

// Whether rel is a direct call or jump to its symbol, with no addend.
static bool isDirectBranch(const SectionChunk &sc, const coff_relocation &rel) {
  uint32_t mask = 0;
  switch (isX64(sc) ? rel.Type : rel.Type | 0x10000) {
  case IMAGE_REL_LLVM_JUMP8:
    mask = 0xff;
    break;
  case IMAGE_REL_LLVM_BRANCH32:
  case IMAGE_REL_LLVM_JUMP32:
  case IMAGE_REL_LLVM_BRANCH_TAIL:
    mask = 0xffffffff;
    break;
  case IMAGE_REL_ARM64_BRANCH26 | 0x10000:
  case IMAGE_REL_LLVM_BRANCH_TAIL | 0x10000:
    mask = 0x03ffffff;
    break;
  case IMAGE_REL_ARM64_BRANCH19 | 0x10000:
    mask = 0x00ffffe0;
    break;
  case IMAGE_REL_ARM64_BRANCH14 | 0x10000:
    mask = 0x0007ffe0;
    break;
  }
  ArrayRef<uint8_t> b = sc.getContents();
  size_t o = rel.VirtualAddress, size = mask == 0xff ? 1 : 4;
  return mask && o + size <= b.size() &&
         !((size == 1 ? b[o] : read32le(&b[o])) & mask);
}

// Whether the link binds the fused sites of sc: those the assembler checked,
// in code, and on ARM64 in a native image.
static bool bindsFusedSites(const SectionChunk &sc) {
  SymbolTable &symtab = sc.file->symtab;
  return sc.file->fusedChecked && isCode(sc) &&
         (isX64(sc) || (symtab.machine == ARM64 && !symtab.ctx.hybridSymtab));
}

// Reports a fork object's reference to data that resolved to another
// image's, which its code reached as local and its data as a word no
// in-place slot holds.
static void checkImportedData(const SectionChunk &sc, Symbol *sym) {
  auto *d = dyn_cast_or_null<Defined>(sym);
  if (!d || !d->isRuntimePseudoReloc || !isLoaded(sc))
    return;
  auto *imp = dyn_cast<DefinedImportData>(d);
  std::string from = imp ? imp->getDLLName().str() : toString(d->getFile());
  StringRef name = d->getName();
  auto e = Err(sc.file->symtab.ctx);
  e << sc.file << ": " << name << " is provided by " << from << ", but ";
  if (sc.file->symtab.vagueEntities.count(sym))
    e << "is referenced as if it were local; recompile, or give it hidden "
         "visibility";
  else if (!isCode(sc))
    e << "needs a runtime pseudo relocation; declare it dllimport, or refer "
         "to __imp_"
      << name;
  else
    e << "was compiled as local; mark it dllimport, or compile with "
         "-fno-direct-access-external-data; in assembly, use __imp_"
      << name;
}

// The expression of a reference to a pointer that holds target, a local
// import's, setting use to how it uses the pointer and sym to what the
// expression reaches. A typed x86-64 site reaches the target by itself; an
// ARM64 adrp or ldr only with every other reference to the pointer.
static RelExpr getLocalImportExpr(const SectionChunk &sc,
                                  const coff_relocation &rel, Defined *target,
                                  Symbol *&sym, uint8_t &use) {
  use = isCode(sc) ? LocalImportChunk::Read : LocalImportChunk::Keep;
  auto *abs = dyn_cast<DefinedAbsolute>(target);
  if (!isCode(sc))
    return R_TYPE;
  if (sc.getArch() == Triple::aarch64) {
    if (sc.file->symtab.machine != ARM64 || sc.file->symtab.ctx.hybridSymtab ||
        (abs && !isUInt<16>(abs->getVA())) || !isArm64PointerLoad(sc, rel) ||
        sc.file->symtab.ctx.delayBound.count(target->getChunk()))
      return R_TYPE;
    use = LocalImportChunk::Load;
    return R_AARCH64_LOCAL;
  }
  if (!isX64(sc) || !sc.file->forkObject)
    return R_TYPE;
  bool constant = abs && isInt<32>(abs->getVA());
  RelExpr expr = R_TYPE;
  if (isLoad(rel.Type))
    expr = !abs ? R_LOCAL_LOAD : constant ? R_ABS_LOAD : R_TYPE;
  else if (rel.Type == IMAGE_REL_LLVM_MEM_LOAD_CALL)
    expr = !abs ? R_LOCAL_CALL : constant ? R_ABS_LOAD : R_TYPE;
  else if (rel.Type == IMAGE_REL_LLVM_MEM_CALL)
    expr = !abs ? R_LOCAL_CALL : !abs->getVA() ? R_ZERO_CALL : R_TYPE;
  // A direct jump into its own function ends no epilogue for the unwinder.
  else if (isMemJump(rel.Type) && !abs &&
           !(isa<DefinedRegular>(target) &&
             cast<DefinedRegular>(target)->getChunk() == &sc))
    expr = R_LOCAL_JUMP;
  if (expr == R_TYPE || !checkSite(sc, rel))
    return R_TYPE;
  use = LocalImportChunk::Load;
  sym = target;
  return expr;
}

// The word of a thread-local record that relocation i reads: an index's, as
// its fact states, or the one a marked access loads into a register and does
// nothing else with; or a negative number.
static int64_t getRecordWord(const SectionChunk &sc, size_t i) {
  const coff_relocation &rel = sc.getRelocs()[i];
  if (rel.Type == IMAGE_REL_LLVM_FUSED_INDEX)
    return sc.file->getSiteFact(&sc, i, SiteFactIndex).value_or(-1) &
           ~uint64_t(SiteFactViaFirst);
  if (rel.VirtualAddress + 4 > sc.getContents().size())
    return -1;
  int word = -1;
  unsigned size = 0;
  FusedInsn insn;
  aarch64::Ldr l;
  if (!isX64(sc)) {
    if (aarch64::parseLdr(read32(sc, rel.VirtualAddress), l) && !l.isFloat &&
        l.extendType == aarch64::ZeroExtend && l.p2Size >= 2)
      word = l.offset, size = 1 << l.p2Size;
  } else if (X86Decoder *dec = X86Decoder::get();
             dec &&
             dec->decodeFused(sc.getContents().drop_front(rel.VirtualAddress),
                              insn) &&
             insn.use == FusedUse::Access && insn.loads && insn.dest < 16) {
    word = insn.disp, size = insn.loads;
  }
  return getTlsRecordWordSize(word) == size ? word : -1;
}

// The code of the value that the marker at relocation j of sc, at offset o,
// says its instruction reads of the record pointer whose index is p, or -1.
static int getIndexCode(const SectionChunk &sc, size_t j, uint32_t o,
                        uint32_t p) {
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  if (j >= rels.size() || rels[j].VirtualAddress != o ||
      rels[j].SymbolTableIndex != p ||
      rels[j].Type != IMAGE_REL_LLVM_FUSED_INDEX)
    return -1;
  std::optional<uint64_t> f = sc.file->getSiteFact(&sc, j, SiteFactIndex);
  return f ? int(*f & 15) : -1;
}

ChainSite lld::coff::getChainSite(const SectionChunk &sc, size_t i) {
  uint32_t o = sc.getRelocs()[i].VirtualAddress;
  uint32_t p = sc.getRelocs()[i].SymbolTableIndex;
  int code = getIndexCode(sc, i, o, p), prev = getIndexCode(sc, i - 1, o, p);
  int next = getIndexCode(sc, i + 1, o, p);
  if (!sc.file->tlsModel || unsigned(code) > SiteFactIndexForm)
    return ChainSite::None;
  if (prev >= 0)
    return ChainSite::Second;
  // The value says what the instruction does with it, and on x86-64, which
  // tests the form in memory and loads the wide word through the place, the
  // instruction's shape; a step after the wide word rotates it or decodes it
  // with the key, which its bytes tell, as chains that share the key's load
  // mark it at the first decode alone.
  ChainSite s = code == 0   ? ChainSite::Place
                : code == 1 ? ChainSite::Base
                : next >= 0 ? ChainSite::Select
                : code == 2 ? ChainSite::Wide
                : code == 5 ? ChainSite::Form
                            : ChainSite::Rotate;
  ArrayRef<uint8_t> b = sc.getContents().drop_front(o);
  uint64_t w = 0;
  for (size_t k = std::min<size_t>(b.size(), 8); k--;)
    w = w << 8 | b[k];
  FusedInsn f{};
  X86Decoder *dec = X86Decoder::get();
  bool x64 = isX64(sc);
  if (x64 && (!dec ||
              !dec->decodeFused(b, f, nullptr, nullptr,
                                !(*sc.file->getSiteFact(&sc, i, SiteFactIndex) &
                                  SiteFactViaFirst))))
    f.use = FusedUse::None;
  if (x64 && s == ChainSite::Place && f.loads != 8)
    s = ChainSite::Form;
  if (x64 && s == ChainSite::Base && (w & 0xfff8) != 0x8d48)
    s = ChainSite::Wide;
  // The bytes of each site, under a mask: on ARM64 ldp place, form, [p];
  // cmp form, #0; add t, tp, place; ldr w, [t]; eor k, k, w, ror #17; csel.
  // On x86-64 REX.W mov place, [p]; cmp qword [p + 8], 0; lea t, [tp +
  // place]; the wide word's mov or rorx from there; ror w, 17 or rorx of a
  // register; xor w, [k], or xor w, k where the chains share the key's
  // load; cmovcc.
  static constexpr struct {
    ChainSite s;
    bool x64;
    uint64_t mask, bits;
  } forms[] = {{ChainSite::Place, 0, 0xffff8000, 0xa9400000},
               {ChainSite::Form, 0, 0xfffffc1f, 0xf100001f},
               {ChainSite::Base, 0, 0xffe0fc00, 0x8b000000},
               {ChainSite::Wide, 0, 0xfffffc00, 0xf9400000},
               {ChainSite::Decode, 0, 0xffe0fc00, 0xcac00000 | TP_ROTATE << 10},
               {ChainSite::Select, 0, 0xffe00c00, 0x9a800000},
               {ChainSite::Place, 1, 0xfff8, 0x8b48},
               {ChainSite::Form, 1, 0x38fff8, 0x388348},
               {ChainSite::Base, 1, 0xfff8, 0x8d48},
               {ChainSite::Wide, 1, 0xfff8, 0x8b48},
               {ChainSite::Wide, 1, 0xffff1fff, 0xf0fb03c4},
               {ChainSite::Rotate, 1, 0xfff8fff8, 0xc8c148 | TP_ROTATE << 24},
               {ChainSite::Rotate, 1, 0xffc0ffff1fff,
                0xc0f0fb03c4 | uint64_t(TP_ROTATE) << 40},
               {ChainSite::Decode, 1, 0xfff8, 0x3348},
               {ChainSite::Decode, 1, 0xc0fff8, 0xc03148},
               {ChainSite::Select, 1, 0xc0f0fff8, 0xc0400f48}};
  // An x86-64 index, and a read of the place's word or the form's, reads
  // its register alone at the displacement the site has.
  bool fits =
      !x64 || s > ChainSite::Wide ||
      (f.use == (s <= ChainSite::Form ? FusedUse::Access : FusedUse::Index) &&
       f.disp == (s == ChainSite::Form ? TP_FORM_AT : 0) &&
       b[f.size - 1] == (s == ChainSite::Form               ? 0
                         : s == ChainSite::Wide && !f.loads ? TP_ROTATE
                                                            : b[f.size - 1]));
  const auto *r = llvm::find_if(forms, [&](const auto &r) {
    return (r.s == s || (s == ChainSite::Rotate && r.s == ChainSite::Decode)) &&
           r.x64 == x64 && (w & r.mask) == r.bits;
  });
  return fits && r != std::end(forms) ? r->s : ChainSite::None;
}

// The wide forms keep the wide arm, which the fact names where its code is
// the wide word's.
std::pair<unsigned, unsigned> lld::coff::getSelect(const SectionChunk &sc,
                                                   size_t i, TlsForm form) {
  ArrayRef<uint8_t> b =
      sc.getContents().drop_front(sc.getRelocs()[i].VirtualAddress);
  uint64_t fact = *sc.file->getSiteFact(&sc, i, SiteFactIndex);
  uint32_t w = read32le(b.data());
  bool x64 = isX64(sc);
  // cmovcc reg, rm, whose reg is the destination; csel d, n, m.
  unsigned arms[2] = {x64 ? (b[3] >> 3 & 7) | (b[0] & 4) << 1 : w >> 5 & 31,
                      x64 ? (b[3] & 7) | (b[0] & 1) << 3 : w >> 16 & 31};
  bool wide = form == TlsForm::Wide || form == TlsForm::ExeWide;
  return {
      x64 ? arms[0] : w & 31,
      arms[!(fact & SiteFactViaFirst) ^
           ((fact & ~uint64_t(SiteFactViaFirst)) == SiteFactIndexWide) ^ wide]};
}

// The expression of site s of an owned-model record chain, relocation i of
// sc, in an image of the form: what the form keeps of the chain is the code a
// local variable gets. By form (Wide, Direct, Place, ExeWide), each step from
// Place to Decode is kept as compiled (k), dead (-), written (w), or on
// x86-64 a load of the place word (l); a select is a mov where it does not
// write the arm it keeps. ARM64 loads a wide word through the base, which a
// wide executable forms as the thread pointer plus H.
static RelExpr getChainExpr(const SectionChunk &sc, size_t i, ChainSite s,
                            TlsForm form) {
  static constexpr const char *steps[2][6] = {
      {"ll--", "----", "-kw-", "k--w", "k--k", "k--k"},
      {"ww--", "----", "kkww", "k--k", "k--k", "k--k"}};
  if (s == ChainSite::Second)
    return R_NONE;
  if (s == ChainSite::Select) {
    auto [dest, kept] = getSelect(sc, i, form);
    return dest != kept ? R_TLS_MOVE : R_DEAD;
  }
  char c = steps[!isX64(sc)][unsigned(s) - 1][unsigned(form)];
  return c == 'k'   ? R_NONE
         : c == 'w' ? R_TLS
         : c == 'l' ? R_LOCAL_ACCESS
                    : R_DEAD;
}

// The $tpoff word of an import of a record that the importer reads for the
// record's word: __imp_X$tpoff for the variable's offset at 8, and
// __imp_X$guard$tpoff for its guard's at 12; null where the import library
// offers no __tp_unit, or the word is not the link's.
static DefinedLocalImport *getTpoffWord(SymbolTable &symtab,
                                        DefinedImportData *imp, int word) {
  StringRef rec = imp->getName();
  if (!rec.consume_back("$tls") || (word != 8 && word != 12))
    return nullptr;
  auto *li = dyn_cast_or_null<DefinedLocalImport>(
      symtab.find((rec + (word == 12 ? "$guard$tpoff" : "$tpoff")).str()));
  return li && li->getChunk()->filled ? li : nullptr;
}

// The expression of a fused index, or an access loading one word, of a
// record, where the site need not go through the record. The image's own,
// rec, gives the place word, the variable or its guard, which sym is set to,
// and the chain's other sites take the image's form. Another image's, imp,
// in an importer whose import library offers __tp_unit, gives a load of the
// variable's or the guard's word its $tpoff word, which sym is set to, and
// an access adds that to the thread pointer in the register its fact names,
// and else to the chain's base, a place form's with no place (spec section
// 15.1, ELF's initial-exec). An x86-64 site changes length, so only a section
// whose code moves has one. On ARM64 an index of the place word's address
// reads its page, which the word's load gives only where indexes alone read
// it, an offset needs a movz to reach it, and an access adds a $tpoff word
// unextended.
static RelExpr getTlsExpr(const SectionChunk &sc, const coff_relocation &rel,
                          FusedUse use, TlsRecordChunk *rec,
                          DefinedImportData *imp, Symbol *&sym,
                          LocalImportUses &uses) {
  ObjFile *file = sc.file;
  bool x64 = isX64(sc), load = use == FusedUse::Access;
  size_t i = &rel - sc.getRelocs().data();
  Defined *index = file->symtab.placeSym;
  TlsForm form = rec ? getTlsForm(sc) : TlsForm::Place;
  // A link-time place needs no place word.
  if (!file->tlsModel ||
      (rec ? !index && form != TlsForm::Place
           : !getTpoffWord(file->symtab, imp, 8)) ||
      (x64 && !sc.movable) || (!load && use != FusedUse::Index))
    return R_TYPE;
  if (ChainSite s = getChainSite(sc, i); s != ChainSite::None) {
    if (!checkSite(sc, rel))
      return R_TYPE;
    if (rec)
      sym = index ? index : rec->var;
    return getChainExpr(sc, i, s, form);
  }
  bool indexed = load && file->getSiteFact(&sc, i, SiteFactIndexedRead);
  int64_t word = getRecordWord(sc, i);
  if (!rec) {
    uint32_t w = read32(sc, rel.VirtualAddress);
    DefinedLocalImport *li = getTpoffWord(file->symtab, imp, word);
    if ((load && !indexed) || !checkSite(sc, rel) ||
        (!load && (!li || (!x64 && (w & 0x3b200c00) == 0x38200800 &&
                           (w >> 13 & 7) != 3))))
      return R_TYPE;
    if (!load)
      return file->getSiteFact(&sc, i, SiteFactThreadPointer) ? R_TLS_TP
                                                              : R_NONE;
    if (!word || !li)
      return word ? R_TYPE : R_DEAD;
    uses[li] |= LocalImportChunk::Read;
    sym = li;
    return R_TLS;
  }
  // A load of the place word's address that indexes alone do not read gives
  // it to a chain the compiler did not state, so its record stays; a place
  // the link knows needs no place word.
  if (!word && load && !indexed)
    return R_TYPE;
  if (!word && (form == TlsForm::Place || form == TlsForm::ExeWide))
    return checkSite(sc, rel) ? R_DEAD : R_TYPE;
  Symbol *to = word == 0 ? index : word == 8 ? rec->var : rec->guard;
  auto *d = dyn_cast_or_null<Defined>(to ? to->getDefined() : nullptr);
  if (word < 0 || !d ||
      (!x64 && (word ? !file->symtab.ctx.tlsFitsMovz
                     : index->getChunk()->getAlignment() < 4 ||
                           index->getRVA() % 4)) ||
      !checkSite(sc, rel))
    return R_TYPE;
  sym = d;
  return x64 && !load && !word ? R_LOCAL_ACCESS
         : x64 && indexed      ? R_DEAD
                               : R_TLS;
}

// The expression of a fused site, which reads the register a load of the
// pointer sym set, by what the pointer holds. A call or jump stops reading
// it where the pointer is a local import's, reaching its target, a replaced
// overridable default's, reaching the replacement, or a delay-loaded
// import's, reaching its thunk. An access or an ARM64 add stops
// where the pointer is a local import's: an x86-64 access, in a section whose
// code moves, reaches the target relative to rip, and an ARM64 site takes the
// target's offset in the page the register then holds.
static RelExpr getFusedExpr(const SectionChunk &sc, const coff_relocation &rel,
                            Symbol *&sym, LocalImportUses &uses) {
  FusedUse use = getUse(sc, rel.Type);
  bool branch = use == FusedUse::Call || use == FusedUse::Jump;
  if (!bindsFusedSites(sc))
    return R_TYPE;
  auto *li = dyn_cast_or_null<DefinedLocalImport>(sym);
  auto *imp = dyn_cast_or_null<DefinedImportData>(sym);
  Defined *target = nullptr;
  if (li && !isa<DefinedAbsolute>(li->getTarget())) {
    target = li->getTarget();
  } else if (imp && branch && imp->file->replacement) {
    // A DLL's call of a default that forwards to another reads the pointer
    // of the default's second import instead.
    if (isa<DefinedImportData>(imp->file->replacement)) {
      sym = imp->file->replacement;
      return R_TYPE;
    }
    target = imp->file->replacement;
  } else if (imp && branch && imp->file->delayLoaded) {
    target = dyn_cast_or_null<DefinedImportThunk>(imp->file->thunkSym);
  }
  if (TlsRecordChunk *rec = TlsRecordChunk::get(target);
      rec || (imp && !branch && isTlsRecordPointerName(imp->getName()))) {
    RelExpr e = getTlsExpr(sc, rel, use, rec, imp, sym, uses);
    // A word's load that stays reads the record's address itself.
    if (e != R_TYPE || !rec ||
        (use == FusedUse::Access &&
         getRecordWord(sc, &rel - sc.getRelocs().data()) >= 0))
      return e;
  }
  if (!branch) {
    if ((use != FusedUse::Add && use != FusedUse::Access) || !target ||
        (isX64(sc) && !sc.movable) ||
        sc.file->symtab.ctx.delayBound.count(target->getChunk()) ||
        !checkSite(sc, rel))
      return R_TYPE;
    sym = target;
    return isX64(sc) ? R_LOCAL_ACCESS : R_PAGEOFF;
  }
  if (!target)
    return R_TYPE;
  bool ok = checkSite(sc, rel);
  if (li)
    uses[li] |= ok ? LocalImportChunk::Load : LocalImportChunk::Read;
  if (!ok)
    return R_TYPE;
  if (imp && isa<DefinedImportThunk>(target))
    imp->file->uses |= ImportFile::Thunk;
  sym = target;
  return rel.Type == IMAGE_REL_LLVM_FUSED_CALL ? R_LOCAL_CALL : R_LOCAL_JUMP;
}

// Whether -ignore-function-address-equality names file, by its name or its
// archive's, or every input.
static bool ignoresAddressEquality(const ObjFile *file) {
  const StringSet<> &names = file->symtab.ctx.config.ignoredAddressEquality;
  auto has = [&](StringRef path) {
    return names.contains(sys::path::filename(path).lower());
  };
  return !names.empty() &&
         (names.contains("") || has(file->getName()) || has(file->parentName));
}

// What a reference of a foreign object, which types no site, does with its
// symbol. Code is what the walker finds; it takes an address only where a
// load of the address fits, an x86-64 lea of 64 bits with no addend or an
// ARM64 adrp and add proven one address. A word of data holds an address;
// the fields of .pdata and .xdata are calls, and other data has no room.
static Site classifyForeign(const SectionChunk &sc,
                            const coff_relocation &rel) {
  if (!isCode(sc)) {
    StringRef name = sc.getSectionName();
    if (rel.Type ==
        (isX64(sc) ? IMAGE_REL_AMD64_ADDR64 : IMAGE_REL_ARM64_ADDR64))
      return SiteKind::Address;
    return name.starts_with(".pdata") || name.starts_with(".xdata")
               ? SiteKind::Branch
               : SiteKind::Data;
  }
  Site site = sc.file->symtab.ctx.walker.classify(sc, rel);
  if (site.kind == SiteKind::Address &&
      (isX64(sc) ? read32(sc, rel.VirtualAddress)
                 : !isAddressPairPart(sc, rel)))
    site.kind = SiteKind::Operand;
  return site;
}

bool lld::coff::takesAddress(const SectionChunk &sc,
                             const coff_relocation &rel) {
  if (sc.file->forkObject)
    return !isDirectBranch(sc, rel) &&
           rel.Type != IMAGE_REL_LLVM_ADDR32NB_CALL &&
           rel.Type != IMAGE_REL_LLVM_REL32_CALL;
  Symbol *s = sc.getRelocTarget(rel);
  auto *d = dyn_cast_or_null<DefinedRegular>(s);
  bool function = isa_and_nonnull<DefinedImportThunk>(s) ||
                  (d && d->getChunk() && isCode(*d->getChunk()));
  return (isX64(sc) || sc.getArch() == Triple::aarch64) &&
         !(function && ignoresAddressEquality(sc.file)) &&
         classifyForeign(sc, rel).kind == SiteKind::Address;
}

// Whether the ARM64 low part low, of the page whose adrp has the addend
// page, may read the address of a function whose address is its delay-loaded
// DLL's own: an add, or a load of bytes overlapping a word of in's section;
// of an import's entry, a load whose register a br or blr right after it
// does not take. A store reads no such address.
static bool readsDelayBound(const SectionChunk &sc, const coff_relocation &low,
                            int64_t page, DefinedImportData *imp,
                            DefinedRegular *in, ArrayRef<uint32_t> words) {
  uint32_t insn = read32(sc, low.VirtualAddress), o = low.VirtualAddress + 4;
  aarch64::Ldr l;
  if (low.Type == IMAGE_REL_ARM64_PAGEOFFSET_12A)
    return true;
  if (!aarch64::parseLdr(insn, l))
    return (insn & 0x3fc00000) != 0x39000000 &&
           (insn & 0x3f400000) != 0x3d000000;
  if (imp) {
    uint32_t next = o + 4 <= sc.getContents().size() ? read32(sc, o) : 0;
    return (next & 0xffdffc1f) != 0xd61f0000 ||
           (next >> 5 & 31) != l.destRegister;
  }
  int64_t at = in ? in->getValue() + l.offset : 0;
  return !in || page != l.offset || llvm::any_of(words, [&](uint32_t w) {
    return at < w + 8 && w < at + (1 << l.p2Size);
  });
}

bool lld::coff::observesFunction(const SectionChunk &sc,
                                 const coff_relocation &rel) {
  Symbol *s = sc.getRelocTarget(rel);
  if (isa_and_nonnull<DefinedImportThunk>(s))
    return takesAddress(sc, rel);
  auto *imp = dyn_cast_or_null<DefinedImportData>(s);
  ReadInsn insn;
  unsigned before;
  if (!imp || !isCode(sc))
    return false;
  if (!isX64(sc))
    return rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L &&
           readsDelayBound(sc, rel, 0, imp, nullptr, {});
  if (sc.file->forkObject)
    return isLoad(rel.Type) && !isRead(rel.Type);
  X86Decoder *dec = X86Decoder::get();
  return rel.Type == IMAGE_REL_AMD64_REL32 && dec &&
         (before = sc.file->symtab.ctx.walker.classify(sc, rel).before) &&
         before <= rel.VirtualAddress &&
         dec->decodeRead(
             sc.getContents().drop_front(rel.VirtualAddress - before), insn) &&
         insn.reads;
}

// Reports a foreign reference to name, a function whose address in the rest
// of the image is dll's: an unclassified site is an error, since it may take
// the address, and one with no room for a load of the address a warning.
static void reportForeignSite(const SectionChunk &sc,
                              const coff_relocation &rel, const Site &site,
                              StringRef name, StringRef dll) {
  COFFLinkerContext &ctx = sc.file->symtab.ctx;
  bool bound = site.kind == SiteKind::Branch || site.kind == SiteKind::Address;
  if (bound && !(isCode(sc) && ctx.config.verbose))
    return;
  static const char *const kinds[] = {"is a branch", "takes its address",
                                      "is another operand", "is data",
                                      "is not classified: "};
  std::string msg =
      (toString(sc.file) + ": the reference to " + name + " at " +
       sc.getSectionName() + "+0x" + Twine::utohexstr(rel.VirtualAddress) +
       (site.function.empty() ? "" : " in ") + site.function + " " +
       kinds[size_t(site.kind)] + (site.reason ? site.reason : "") + site.how)
          .str();
  if (bound)
    Log(ctx) << msg;
  else if (site.kind != SiteKind::Refused)
    Warn(ctx) << msg << "; it keeps this image's address of " << name;
  else
    Err(ctx) << msg << "; it may take the address of " << name << ", which is "
             << dll << "'s; -ignore-function-address-equality:"
             << sys::path::filename(sc.file->parentName.empty()
                                        ? sc.file->getName()
                                        : sc.file->parentName)
             << " keeps this image's own";
}

// A guard's form: inline in code that moves, on x86-64 where a load of a
// pointer states that the flags are dead, on ARM64 at an adrp whose pair the
// assembler lets the link resize; else a call of its leaf, or on ARM64 a
// branch to its guard, in the site's bytes.
static RelExpr getGuardExpr(const SectionChunk &sc,
                            const coff_relocation &rel) {
  uint32_t o = rel.VirtualAddress;
  ArrayRef<uint8_t> c = sc.getContents();
  return sc.movable &&
                 (isX64(sc) ? (rel.Type == IMAGE_REL_LLVM_MEM_LOAD ||
                               rel.Type == IMAGE_REL_LLVM_MEM_LOAD_REX2) &&
                                  (read32(sc, o) & LoadFlagsDead)
                            : o + 8 <= c.size() &&
                                  relocationMayResizeCode(ARM64, rel.Type, "",
                                                          read64le(&c[o])))
             ? R_GUARD
             : R_GUARD_CALL;
}

// The expression of a code reference that takes the address of imp where it
// can load the address from the import's entry instead, so that it agrees
// with the in-place slots: an x86-64 lea becomes mov, and an ARM64 adrp and
// add proven one address become adrp and ldr. The load of a delay-loaded
// function's address is guarded (section 15.3), inline in code that moves
// (on x86-64 where the flags are dead), else by a call (x86-64) or a branch
// (ARM64) in its bytes: an x86-64 lea or load of its entry, the adrp of an
// ARM64 pair or of a fork object's ldr of the entry. foreign is a foreign
// site's class.
static std::optional<RelExpr>
getAddressExpr(const SectionChunk &sc, const coff_relocation &rel, Symbol *&sym,
               DefinedImportData *imp, ImportSites &sites,
               const Site *foreign) {
  bool x64 = isX64(sc), guard = imp->file->observed;
  bool lea = isLea(rel.Type);
  bool adrp = rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21;
  bool load = x64 ? rel.Type == IMAGE_REL_LLVM_MEM_LOAD ||
                        rel.Type == IMAGE_REL_LLVM_MEM_LOAD_REX2
                  : adrp && sym == imp &&
                        llvm::any_of(sc.getRelocs(), [&](auto &r) {
                          return r.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L &&
                                 r.SymbolTableIndex == rel.SymbolTableIndex;
                        });
  if (!isCode(sc) || (foreign ? foreign->kind != SiteKind::Address
                      : x64   ? !lea && !(guard && load)
                      : sym == imp ? !(guard && load)
                          : !isAddressPairPart(sc, rel)))
    return std::nullopt;
  if (!foreign && x64) {
    if (!checkSite(sc, rel))
      return R_TYPE;
    if (isLeaOffset(rel.Type))
      sites.interior.push_back(
          {const_cast<SectionChunk *>(&sc), imp, rel.VirtualAddress, false});
  }
  if (!isLeaOffset(rel.Type))
    imp->file->uses |= isa<DefinedImportData>(sym)
                           ? ImportFile::Read
                           : ImportFile::Loaded | ImportFile::Read;
  sym = imp;
  if (!guard || (!x64 && !adrp))
    return R_IMPORT_LOAD;
  // The site of an address inside the function loads the entry of the name
  // its DLL exports for it, which bindImportSlots gives it.
  bool rex2 = foreign ? foreign->before == 4 : isRex2(rel.Type);
  sites.guarded.push_back(
      {const_cast<SectionChunk *>(&sc), imp,
       uint32_t(&rel - sc.getRelocs().data()), 0, false,
       int8_t(x64 ? getLoadRegister(sc, rel.VirtualAddress, rex2)
                  : read32(sc, rel.VirtualAddress) & 31)});
  sites.guarded.back().before = 3 + rex2;
  return getGuardExpr(sc, rel);
}

// The expression of a reference to the import imp by its own name, sym being
// its thunk, the image's copy of a function a DLL offers, or automatically
// imported data. An imported function has no address in the image, so a
// fork object's reference gets one only from a provider (section 9.1): the
// loader for a whole word of data, an in-place slot; the import's entry for
// code that takes the address; the thunk for a branch or a call-only field.
// A section the image does not load and a delay-loaded function keep the
// thunk; so does a foreign branch, and all of an object that
// -ignore-function-address-equality names.
static RelExpr getImportExpr(const SectionChunk &sc, const coff_relocation &rel,
                             Symbol *&sym, DefinedImportData *imp,
                             ImportSites &sites) {
  COFFLinkerContext &ctx = sc.file->symtab.ctx;
  ImportFile *file = imp->file;
  bool thunk = !isa<DefinedImportData>(sym), x64 = isX64(sc);
  bool exec =
      sc.header->Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE);
  bool word =
      rel.Type == (x64                               ? IMAGE_REL_AMD64_ADDR64
                   : sc.getArch() == Triple::aarch64 ? IMAGE_REL_ARM64_ADDR64
                                                     : 0xffff);
  bool foreign = !sc.file->forkObject;
  bool inPlace = isLoaded(sc) && !file->delayLoaded &&
                 !(thunk && foreign && ignoresAddressEquality(sc.file));
  bool bound = isLoaded(sc) && file->observed;
  bool hybrid = ctx.hybridSymtab || isArm64EC(ctx.config.machine);
  bool fork = !foreign && !hybrid;
  // An x86-64 field before an immediate reads its target.
  bool reads = x64 && rel.Type >= IMAGE_REL_AMD64_REL32_1 &&
               rel.Type <= IMAGE_REL_AMD64_REL32_5;
  StringRef name = imp->getName().substr(strlen("__imp_"));
  if (!word && ((inPlace && !(hybrid && (thunk || !foreign))) ||
                (bound && !hybrid))) {
    std::optional<Site> site;
    if (foreign)
      site = classifyForeign(sc, rel);
    if (foreign && thunk && !reads)
      reportForeignSite(sc, rel, *site, name, file->dllName);
    if (std::optional<RelExpr> e =
            getAddressExpr(sc, rel, sym, imp, sites, site ? &*site : nullptr))
      return *e;
  }
  // A direct call or jump of the executable's to a default that it does not
  // replace reaches what the default's slot names, and a DLL's the thunk of
  // the import that its calls read.
  if (thunk && fork && file->replacement && isDirectBranch(sc, rel)) {
    auto *call = dyn_cast<DefinedImportData>(file->replacement);
    sym = call ? call->file->thunkSym : file->replacement;
    if (call)
      call->file->uses |= ImportFile::Thunk;
    return R_TYPE;
  }
  if (!word || !inPlace) {
    if (!thunk) {
      // A pseudo relocation reads the pointer where it is.
      if (!foreign)
        checkImportedData(sc, sym);
      else
        file->uses |= ImportFile::Keep;
      return R_TYPE;
    }
    if (inPlace && !(fork ? isDirectBranch(sc, rel) ||
                                rel.Type == IMAGE_REL_LLVM_ADDR32NB_CALL ||
                                rel.Type == IMAGE_REL_LLVM_REL32_CALL
                          : !reads)) {
      reportSite(sc, rel,
                 "needs an address for " + name + ", which " + file->dllName +
                     " provides and this image has none of; call it, or use " +
                     imp->getName() +
                     (x64 ? ", in assembly " + name + "@GOTPCREL(%rip)"
                          : ", in assembly :got:" + name));
      return R_TYPE;
    }
    // The image's own copy of a function a DLL offers is no thunk.
    if (isa<DefinedImportThunk>(sym))
      file->uses |= ImportFile::Thunk;
    return R_TYPE;
  }
  if (exec || uint64_t(rel.VirtualAddress) + 8 > sc.getSize()) {
    reportSite(sc, rel,
               "holds the address of " + name + ", imported from " +
                   file->dllName +
                   ", which the loader writes only in a whole "
                   "word of data");
    return R_TYPE;
  }
  if (thunk)
    file->uses |= ImportFile::InPlace;
  sites.slots.push_back(
      {const_cast<SectionChunk *>(&sc), imp, rel.VirtualAddress, false});
  // A hybrid image has no slots: bindImportSlots reports the first.
  if (hybrid)
    return R_TYPE;
  sym = imp;
  return R_IMPORT_SLOT;
}

// The je or jne right after the x86-64 fused test at relocation i, if its
// relocation states it.
const coff_relocation *lld::coff::getTestJump(const SectionChunk &sc,
                                              size_t i) {
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  if (i + 1 == rels.size())
    return nullptr;
  const coff_relocation &j = rels[i + 1];
  uint32_t end = rels[i].VirtualAddress + getBytesFrom(sc, rels[i]);
  unsigned gap = j.Type == IMAGE_REL_LLVM_JUMP8    ? 1
                 : j.Type == IMAGE_REL_LLVM_JUMP32 ? 2
                                                   : 0;
  return gap && j.VirtualAddress == end + gap ? &j : nullptr;
}

// Whether the fused test at relocation i tests a pointer whose value the
// link knows, a local import pointer, and then whether the jump that reads
// it is taken: on x86-64 the je or jne after it, which a relocation states
// where the code moves, on ARM64 the cbz or cbnz itself, whose target the
// relocation before the marker names if any does. A jump with an addend or
// another relocation stays, and so does its test.
static std::optional<bool> getTestOutcome(const SectionChunk &sc, size_t i) {
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  const coff_relocation &rel = rels[i];
  bool x64 = isX64(sc);
  if (rel.Type != IMAGE_REL_LLVM_FUSED_TEST &&
      (!x64 || rel.Type != IMAGE_REL_LLVM_FUSED_TEST_REG))
    return std::nullopt;
  auto *li = dyn_cast_or_null<DefinedLocalImport>(sc.getRelocTarget(rel));
  if (!li || !bindsFusedSites(sc) || !checkSite(sc, rel))
    return std::nullopt;
  ArrayRef<uint8_t> c = sc.getContents();
  bool zero = isAbsoluteZero(li->getTarget());
  auto jumps = [&](const coff_relocation &j) {
    return isDirectBranch(sc, j) &&
           isa_and_nonnull<Defined>(sc.getRelocTarget(j));
  };
  if (!x64) {
    if (i && rels[i - 1].Type == IMAGE_REL_ARM64_BRANCH19 &&
        rels[i - 1].VirtualAddress == rel.VirtualAddress && !jumps(rels[i - 1]))
      return std::nullopt;
    return bool(c[rel.VirtualAddress + 3] & 1) != zero;
  }
  size_t end = rel.VirtualAddress + getBytesFrom(sc, rel);
  const coff_relocation *jump = getTestJump(sc, i);
  bool wide = end < c.size() && c[end] == 0x0f;
  size_t size = wide ? 6 : 2;
  if (end + size > c.size() || (c[end + wide] & 0xfe) != (wide ? 0x84 : 0x74) ||
      (!jump && sc.movable)) {
    reportSite(sc, rel, "is not followed by a je or jne");
    return std::nullopt;
  }
  if (jump ? !jumps(*jump)
           : i + 1 != rels.size() && rels[i + 1].VirtualAddress < end + size)
    return std::nullopt;
  return bool(c[end + wide] & 1) != zero;
}

// Whether rel, a reference of sc to an import's pointer, is of an
// instruction that reads all of the pointer and nothing else, so that any
// aligned word holding the import's address serves it.
static bool readsPointer(const SectionChunk &sc, const coff_relocation &rel) {
  if (!isCode(sc))
    return false;
  if (sc.getArch() == Triple::aarch64)
    return isArm64PointerLoad(sc, rel);
  return (isLoad(rel.Type) || isPointerBranch(rel.Type)) &&
         sc.file->forkObject && checkSite(sc, rel);
}

// Whether rel, of an object for Windows Itanium or NT-POSIX, reads the
// pointer it names for a call or jump: an x86-64 one, or an ARM64 fused
// site's read and its adrp.
static bool readsCallWord(const SectionChunk &sc, const coff_relocation &rel) {
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  if (!sc.file->forkObject)
    return false;
  if (isX64(sc))
    return isPointerBranch(rel.Type);
  return rel.Type == IMAGE_REL_LLVM_FUSED_READ ||
         (rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
          &rel + 1 != rels.end() &&
          (&rel + 1)->Type == IMAGE_REL_LLVM_FUSED_READ &&
          (&rel + 1)->SymbolTableIndex == rel.SymbolTableIndex);
}

// Whether sc is the section of an overridable default that the image defines
// or of its own entry, whose check and transfer read the check word.
static bool isOwnDefaultSection(const SectionChunk &sc,
                                const SymbolTable::OwnDefault &o) {
  auto *x = cast<DefinedRegular>(o.address->getTarget());
  auto *e = dyn_cast_or_null<DefinedRegular>(o.entry);
  return o.slot != SymbolTable::VagueSlot &&
         (x->getChunk() == &sc || (e && e->getChunk() == &sc));
}

// Returns the expression of rel, a reference of sc to an overridable default
// or a vague entity that the image keeps preemptible, or with check to the
// default's check word, which neither the default nor its own entry is. A
// call or jump reaches the own entry, and an address-take or any other read
// of the check word reads the word of the address. A foreign site takes the
// address where it is classified so, and keeps the image's own otherwise, as
// one of an imported function does.
static RelExpr getOwnDefaultExpr(const SectionChunk &sc,
                                 const coff_relocation &rel,
                                 const SymbolTable::OwnDefault &o, bool check,
                                 Symbol *&sym, LocalImportUses &uses) {
  if (!sc.file->forkObject) {
    if (ignoresAddressEquality(sc.file))
      return R_TYPE;
    Site site = classifyForeign(sc, rel);
    reportForeignSite(sc, rel, site, sym->getName(), "the program");
    if (site.kind != SiteKind::Address)
      return R_TYPE;
  }
  bool x64 = isX64(sc);
  bool call =
      check ? x64 && isPointerBranch(rel.Type) : isDirectBranch(sc, rel);
  // A call through a vague function's cell keeps the image's own copy.
  Defined *own =
      o.slot == SymbolTable::VagueSlot ? o.address->getTarget() : o.entry;
  if (call && check && own) {
    uint8_t use;
    return getLocalImportExpr(sc, rel, own, sym, use);
  }
  if (call) {
    sym = o.entry ? o.entry : sym;
    return R_TYPE;
  }
  // An executable has no cells, and its address-takes of a default that it
  // defines take the default, which its own calls reach too, unless a DLL's
  // offer replaces it, whose address the word takes.
  if ((!sc.file->symtab.ctx.config.dll &&
       !sc.file->symtab.replacedByOffer(o)) ||
      (!check && sc.file->forkObject &&
       !(x64 ? isLea(rel.Type) && !isLeaOffset(rel.Type) && checkSite(sc, rel)
             : isAddressPairPart(sc, rel))))
    return R_TYPE;
  uses[o.address] |= LocalImportChunk::Read;
  sym = o.address;
  return check ? R_TYPE : R_IMPORT_LOAD;
}

// Whether rels[i] is part of an ARM64 site that loads li, the cell of a vague
// function the image keeps preemptible, only to call or jump through it: a
// call keeps the image's own copy, so the site binds as a local import's.
static bool callsVagueCell(const SectionChunk &sc, size_t i,
                           DefinedLocalImport *li) {
  SymbolTable::OwnDefault *o = sc.file->symtab.findOwnDefault(li->getTarget());
  if (!o || o->slot != SymbolTable::VagueSlot || o->address != li)
    return false;
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  for (size_t j = i;
       j != rels.size() && rels[j].SymbolTableIndex == rels[i].SymbolTableIndex;
       ++j) {
    FusedUse use = getUse(sc, rels[j].Type);
    if (use == FusedUse::Call || use == FusedUse::Jump)
      return true;
    if (rels[j].Type != IMAGE_REL_ARM64_PAGEBASE_REL21 &&
        rels[j].Type != IMAGE_REL_LLVM_FUSED_READ)
      return false;
  }
  return false;
}

bool lld::coff::isKeyImport(Symbol *s) {
  StringRef n = s->getName();
  return n.consume_front("__imp_") && n == COFF::TP::Key;
}

static RelExpr getRelExpr(const SectionChunk &sc, const coff_relocation &rel,
                          Symbol *&sym, LocalImportUses &uses,
                          ImportSites &sites) {
  // What a reference the rows below leave as it is becomes.
  RelExpr plain = R_TYPE;
  switch (rel.Type) {
  case IMAGE_REL_LLVM_TLS_BASE:
    return checkSite(sc, rel) ? R_TLS_BASE : R_TYPE;
  case IMAGE_REL_LLVM_ALIGN:
    return (sc.movable || sc.compacts) && checkSite(sc, rel) ? R_ALIGN : R_TYPE;
  case IMAGE_REL_LLVM_BRANCH_TAIL:
    if (sc.resizable)
      checkSite(sc, rel);
    break;
  case IMAGE_REL_LLVM_JUMP8:
  case IMAGE_REL_LLVM_JUMP32:
    if (sc.movable && isX64(sc) && checkSite(sc, rel))
      plain = R_JUMP;
    break;
  // The assembler resolved the field within its section.
  case IMAGE_REL_AMD64_SREL32:
    if (!sc.resizable || !isX64(sc))
      break;
    if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
        d && d->getChunk() == &sc)
      return R_SHIFT;
    reportSite(sc, rel, "names a target outside its section");
    return R_TYPE;
  case IMAGE_REL_LLVM_FUSED_CALL:
  case IMAGE_REL_LLVM_FUSED_JUMP:
  case IMAGE_REL_LLVM_FUSED_JUMP_TAIL:
    return getFusedExpr(sc, rel, sym, uses);
  }
  // The marker of any other fused site that stays writes nothing.
  bool marker = getUse(sc, rel.Type) != FusedUse::None;
  if (marker && sym)
    if (RelExpr e = getFusedExpr(sc, rel, sym, uses); e != R_TYPE)
      return e;
  if (auto *li = dyn_cast_or_null<DefinedLocalImport>(sym); li && !marker) {
    uint8_t use;
    RelExpr e = getLocalImportExpr(sc, rel, li->getTarget(), sym, use);
    uses[li] |= use;
    return e;
  }
  SymbolTable &symtab = sc.file->symtab;
  if (SymbolTable::OwnDefault *o = symtab.findOwnDefault(sym);
      o && isCode(sc) && !isOwnDefaultSection(sc, *o))
    if (RelExpr e = getOwnDefaultExpr(sc, rel, *o, false, sym, uses);
        e != R_TYPE || sym != sc.getRelocTarget(rel))
      return e == R_TYPE ? plain : e;
  // Only the cell's own object refers to a local name in its section.
  if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
      d && !symtab.cells.empty())
    if (LocalImportChunk *c = symtab.cells.lookup(d->getChunk()))
      c->uses |= LocalImportChunk::Keep;
  if (symtab.ctx.hasForkObject) {
    // An offered function's address is the DLL's, its calls the image's; but
    // a DLL's calls of an overridable default it offers to replace reach what
    // the program chose, through the default's import thunk.
    DefinedImportData *imp = nullptr;
    // Only a function's entity has an owner.
    if (DefinedImportData *owner =
            sym && sym->addressImported
                ? symtab.vagueEntities.lookup(sym).owner
                : nullptr) {
      if (sc.file->forkObject && !takesAddress(sc, rel))
        sym = owner->file->thunkSym ? owner->file->thunkSym : sym;
      else if (isLoaded(sc))
        imp = owner;
    }
    if (auto *t = dyn_cast_or_null<DefinedImportThunk>(sym))
      imp = t->wrappedSym;
    else if (auto *d = dyn_cast_or_null<DefinedImportData>(sym);
             d && d->isRuntimePseudoReloc)
      imp = d->file->impSym;
    if (imp) {
      RelExpr e = getImportExpr(sc, rel, sym, imp, sites);
      return e == R_TYPE ? plain : e;
    }
    // A call or jump through an overridable default's pointer reaches what
    // the default's slot names, as one through a local import's its target;
    // one through the executable's pointer to a DLL's offer, or a DLL's of a
    // default that forwards to another, reads that import's pointer instead.
    // A reference to an import's pointer keeps its entry unless it reads all
    // of the pointer, wherever that is.
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(sym); imp && !marker) {
      if (imp->file->observed && sc.file->forkObject && isLoaded(sc))
        if (std::optional<RelExpr> e =
                getAddressExpr(sc, rel, sym, imp, sites, nullptr))
          return *e;
      uint8_t use;
      if (Defined *b = imp->file->replacement;
          b && (isa<DefinedImportData>(b) ? readsCallWord(sc, rel)
                                          : isPointerBranch(rel.Type))) {
        if (auto *call = dyn_cast<DefinedImportData>(b)) {
          call->file->uses |= ImportFile::Read;
          sym = call;
          return plain;
        }
        if (RelExpr e = getLocalImportExpr(sc, rel, b, sym, use); e != R_TYPE)
          return e;
      }
      // The key's import is read where a reference stays once its chains
      // are decided.
      if (!isKeyImport(imp))
        imp->file->uses |=
            readsPointer(sc, rel) ? ImportFile::Read : ImportFile::Keep;
    }
  }
  if (sc.file->forkObject)
    checkImportedData(sc, sym);
  return plain;
}

static bool inResizable(Symbol *sym) {
  auto *d = dyn_cast_or_null<DefinedRegular>(sym);
  return d && d->data && *d->data && (*d->data)->resizable;
}

// The guard of a site that reads a word holding the address of a function
// whose address is its delay-loaded DLL's own, or takes the address of such
// words (section 15.3): a word of a delay-bound section, sym, by its own name
// or by a local import's pointer to it, or the function's entry. On x86-64 a
// lea and a load of 64 bits leave their value in their register; any other
// instruction is replayed behind the guard. A store, a call or jump through
// memory and a read that misses the section's words reach no such address,
// and an access through a register is behind the guard of the register's
// load. On ARM64 the site is the adrp of the page, which its low parts read.
static RelExpr getDelayBoundExpr(const SectionChunk &sc,
                                 const coff_relocation &rel, Symbol *sym,
                                 RelExpr expr, ImportSites &sites) {
  COFFLinkerContext &ctx = sc.file->symtab.ctx;
  auto *li = dyn_cast_or_null<DefinedLocalImport>(sym);
  auto *imp = dyn_cast_or_null<DefinedImportData>(sym);
  auto *d = dyn_cast_or_null<Defined>(li ? li->getTarget() : sym);
  uint16_t t = rel.Type;
  uint32_t o = rel.VirtualAddress;
  bool x64 = isX64(sc);
  auto it = d ? ctx.delayBound.find(d->getChunk()) : ctx.delayBound.end();
  if (!d || (!(imp && imp->file->observed) && it == ctx.delayBound.end()) ||
      !isCode(sc) || !isLoaded(sc) || getUse(sc, t) != FusedUse::None ||
      (x64 && isPointerBranch(t)) || (expr != R_TYPE && expr != R_LOCAL_LOAD))
    return expr;
  auto refuse = [&](const char *why) {
    reportSite(sc, rel,
               "reaches " + d->getName() +
                   ", which holds the address of a function of a "
                   "delay-loaded DLL, in an instruction " +
                   why);
    return expr;
  };
  DelayGuardSite g{const_cast<SectionChunk *>(&sc), cast<Defined>(sym),
                   uint32_t(&rel - sc.getRelocs().data()), 0,
                   expr == R_LOCAL_LOAD || isLea(t)};
  auto *in = dyn_cast<DefinedRegular>(d);
  ArrayRef<uint32_t> words;
  if (it != ctx.delayBound.end())
    words = it->second.words;
  if (!x64) {
    switch (t) {
    case IMAGE_REL_ARM64_PAGEBASE_REL21:
      break;
    case IMAGE_REL_ARM64_REL21:
    case IMAGE_REL_ARM64_BRANCH19:
    case IMAGE_REL_ARM64_ADDR32:
    case IMAGE_REL_ARM64_ADDR32NB:
    case IMAGE_REL_ARM64_ADDR64:
    case IMAGE_REL_LLVM_MOVW_UABS_G0:
    case IMAGE_REL_LLVM_MOVW_UABS_G0_NC:
    case IMAGE_REL_LLVM_MOVW_UABS_G1:
    case IMAGE_REL_LLVM_LOGIMM:
      return refuse("that does not compute its address from a page, which "
                    "the link cannot guard");
    default:
      return expr;
    }
    // The low parts of the page are those of its symbol from its register.
    aarch64::Adrp page;
    aarch64::parseAdrp(read32(sc, o), page);
    bool any = false, reads = li;
    for (const coff_relocation &low : sc.getRelocs())
      if (low.SymbolTableIndex == rel.SymbolTableIndex &&
          (low.Type == IMAGE_REL_ARM64_PAGEOFFSET_12A ||
           low.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L) &&
          (read32(sc, low.VirtualAddress) >> 5 & 31) == page.destRegister) {
        any = true;
        reads |= readsDelayBound(sc, low, page.addend >> 12, imp, in, words);
      }
    if (any && !reads)
      return expr;
    g.reg = read32(sc, o) & 31;
    sites.guarded.push_back(g);
    return getGuardExpr(sc, rel);
  }
  bool stated = isRel32Insn(t);
  if (t - IMAGE_REL_AMD64_REL32 > 5u && !stated) {
    if ((!g.address && !isLoad(t)) || (expr == R_TYPE && !checkSite(sc, rel)))
      return expr;
    if (isLeaOffset(t))
      g.addend = read32(sc, o);
    g.before = 3 + isRex2(t);
    g.reg = getLoadRegister(sc, o, isRex2(t));
    sites.guarded.push_back(g);
    return getGuardExpr(sc, rel);
  }
  // A fork object's type states where the instruction begins; a foreign
  // object's begins where the walker found it.
  unsigned before = stated               ? getBytesBefore(t)
                    : sc.file->forkObject ? 0
                                          : ctx.walker.classify(sc, rel).before;
  ArrayRef<uint8_t> c = sc.getContents();
  X86Decoder *dec = X86Decoder::get();
  ReadInsn insn;
  if (!before && sc.file->forkObject)
    return refuse("whose start its object does not state, as an object from "
                  "an older compiler");
  if (!before || before > o || !dec ||
      !dec->decodeRead(c.drop_front(o - before), insn) ||
      insn.size != before + (stated ? getBytesFrom(sc, rel)
                                    : 4 + t - IMAGE_REL_AMD64_REL32))
    return refuse("that the link cannot find to guard");
  g.addend = read32(sc, o);
  // An access of unknown width may reach any word.
  int64_t at = in ? in->getValue() + g.addend : 0;
  if (!insn.address && (!insn.reads || (!imp && !li && in && insn.width &&
                                        llvm::none_of(words, [&](uint32_t w) {
                                          return at < w + 8 &&
                                                 w < at + insn.width;
                                        }))))
    return expr;
  if (insn.stack)
    return refuse("that uses the stack pointer, which the link cannot guard");
  // A site of a value's leaf is the leaf's own instruction, prefixes aside.
  if (before != 3 && !(before == 4 && c[o - 4] == 0xd5))
    insn.value = -1;
  g.address = insn.address;
  g.reg = insn.value;
  g.before = before;
  if (insn.value < 0)
    g.insn = llvm::toStringRef(c.slice(o - before, insn.size));
  sites.guarded.push_back(g);
  return R_GUARD_CALL;
}

// Scans the parts of a function's code, whose sites the link decides as one:
// a fused read is dead, and an ARM64 site takes an offset in a page, by what
// every site of the pointer in them does, as a register's value crosses from
// a function's part to its cold part but no further.
static void scanFunction(ArrayRef<SectionChunk *> parts, LocalImportUses &uses,
                         ImportSites &sites) {
  struct Part {
    Part(SectionChunk &sc) : sc(sc) {}
    SectionChunk &sc;
    SmallVector<Relocation, 0> out;
    // The fused reads.
    SmallVector<size_t, 0> reads;
    bool sorted, reported = false;
  };
  SmallVector<Part, 2> scans;
  // The pointers a fused site still reads; those in whose page ARM64 sites
  // take an offset.
  SmallDenseSet<uint32_t, 4> kept, paged;
  // A resized section, and an x86-64 site that stays in place, are rewritten
  // by what the relocations beside each site say.
  auto checkOrder = [&](Part &p, const coff_relocation &rel) {
    if (!p.sorted && !p.sc.resizable && !std::exchange(p.reported, true))
      reportSite(p.sc, rel,
                 "is in a section whose relocations are not in order");
  };
  for (SectionChunk *part : parts) {
    Part &p = scans.emplace_back(*part);
    SectionChunk &sc = p.sc;
    ObjFile *file = sc.file;
    bool x64 = isX64(sc);
    ArrayRef<coff_relocation> rels = sc.getRelocs();
    SmallVector<Relocation, 0> &out = p.out;
    p.sorted = llvm::is_sorted(
        rels, [](const coff_relocation &a, const coff_relocation &b) {
          return a.VirtualAddress < b.VirtualAddress;
        });
    if (sc.resizable && !p.sorted)
      Err(file->symtab.ctx) << file << ": the relocations of "
                            << sc.getSectionName()
                            << ", which the link may resize, are not in order";
    std::optional<RelExpr> jump;
    for (size_t i = 0, e = rels.size(); i != e; ++i) {
      const coff_relocation &rel = rels[i];
      // A value that depends on the layout of resizable code may stop
      // fitting its field: a span over it, or a narrow ARM64 branch in it.
      if (const coff_relocation *pair = sc.getPair(rel)) {
        if (inResizable(file->getSymbol(rel.SymbolTableIndex)) ||
            inResizable(file->getSymbol(pair->SymbolTableIndex)))
          file->fitChecks.emplace_back(&sc, i);
      } else if (sc.resizable && !x64 &&
                 (rel.Type == IMAGE_REL_ARM64_BRANCH19 ||
                  rel.Type == IMAGE_REL_ARM64_BRANCH14 ||
                  rel.Type == IMAGE_REL_ARM64_REL21)) {
        file->fitChecks.emplace_back(&sc, i);
      }
      Symbol *const target = sc.getRelocTarget(rel);
      Symbol *sym = target;
      RelExpr expr;
      bool branch = false;
      // A fused test whose outcome the link knows is dead and reads no
      // pointer. Its jump is dead where not taken, else a jump to its
      // target: an ARM64 cbz or cbnz to that of the relocation before the
      // marker. An x86-64 je or jne the assembler resolved goes with its
      // test.
      if (jump) {
        expr = *std::exchange(jump, std::nullopt);
      } else if (auto *li = dyn_cast_or_null<DefinedLocalImport>(sym);
                 li && li->getChunk()->filled &&
                 !((file->getSiteFact(&sc, i, SiteFactValueOnly) ||
                    file->getSiteFact(&sc, i, SiteFactValueOnlyAccess)) &&
                   file->symtab.findOwnDefault(li->getTarget())) &&
                 !(!x64 && callsVagueCell(sc, i, li))) {
        // Only the image's fill knows what the pointer holds, but a value
        // read of a constant that the image keeps preemptible reads its own
        // copy.
        uses[li] |= LocalImportChunk::Keep;
        expr = R_TYPE;
        SymbolTable &symtab = sc.file->symtab;
        if (SymbolTable::OwnDefault *o = symtab.findOwnDefault(li->getTarget());
            o &&
            (o->slot != SymbolTable::VagueSlot  ? o->check
             : x64 && isPointerBranch(rel.Type) ? o->address
                                                : nullptr) == li &&
            sc.file->forkObject && !isOwnDefaultSection(sc, *o))
          expr = getOwnDefaultExpr(sc, rel, *o, true, sym, uses);
      } else if (std::optional<bool> taken = getTestOutcome(sc, i)) {
        expr = x64 || !*taken ? R_DEAD : R_LOCAL_JUMP;
        branch = !x64 && i && rels[i - 1].Type == IMAGE_REL_ARM64_BRANCH19 &&
                 rels[i - 1].VirtualAddress == rel.VirtualAddress;
        if (x64 && getTestJump(sc, i))
          jump = *taken ? R_LOCAL_JUMP : R_DEAD;
        else if (x64)
          checkOrder(p, rel);
        else
          sym = branch ? sc.getRelocTarget(rels[i - 1]) : nullptr;
      } else {
        expr = getRelExpr(sc, rel, sym, uses, sites);
        expr = getDelayBoundExpr(sc, rel, sym, expr, sites);
      }
      // A branch to a function override's stub that Windows can rewrite
      // reaches the default body, as a site of the function's record.
      if (FunctionOverrideThunk *t = FunctionOverrideThunk::get(sym);
          t && isFunctionOverrideSite(sc, rel, expr))
        sym = t->fo.body;
      // The lea of a fused add goes into the lea its load of a local import
      // pointer becomes.
      if (rel.Type == IMAGE_REL_LLVM_FUSED_ADD && x64 && file->fusedChecked &&
          !out.empty() && out.back().expr == R_LOCAL_LOAD && checkSite(sc, rel))
        expr = R_DEAD;
      if (isRead(rel.Type))
        p.reads.push_back(i);
      else if (expr == R_PAGEOFF)
        paged.insert(rel.SymbolTableIndex), file->fitChecks.emplace_back(&sc, i);
      else if (expr == R_TYPE && sym && getUse(sc, rel.Type) != FusedUse::None)
        kept.insert(rel.SymbolTableIndex);
      // A section keeps its relocations from its first rewrite or retarget
      // on, and a resizable one and one with a fused read keep them all.
      if (out.empty()) {
        if (expr == R_TYPE && sym == target && !sc.resizable &&
            !isRead(rel.Type))
          continue;
        out.reserve(e);
        for (size_t j = 0; j != i; ++j)
          out.push_back({rels[j].VirtualAddress, rels[j].Type, R_TYPE, false,
                         sc.getRelocTarget(rels[j])});
      }
      out.push_back({rel.VirtualAddress, rel.Type, expr, false, sym});
      if (branch)
        out[i - 1].expr = R_NONE;
    }
  }
  auto reset = [&](Part &p, size_t j, Symbol *sym = nullptr) {
    const coff_relocation &rel = p.sc.getRelocs()[j];
    p.out[j] = {rel.VirtualAddress, rel.Type, R_TYPE, false,
                sym ? sym : p.sc.getRelocTarget(rel)};
  };

  // An owned-model record chain relaxes whole or not at all: where a site of
  // it stays, but a word's load or access of the image's own record, which
  // keeps its value, every site of its pointer reads the record as compiled.
  // The key's sites follow the chains: where every chain of the function
  // relaxes, an x86-64 decode of a wide word the image keeps reads the
  // image's own key word, as does an ARM64 key's load where the image is
  // wide, and the rest are dead. A relaxed import's read goes with its sites,
  // its ARM64 adrp giving the page of its $tpoff words.
  SmallDenseMap<uint32_t, Symbol *, 4> released;
  {
    // Each import's chain: whether it relaxes, and whether an access of it
    // adds its word to the thread pointer and whether one does not; where
    // every one does, the chain's other sites are dead.
    struct Chain {
      bool ok = true, tp = false, notTp = false;
    };
    SmallDenseMap<uint32_t, Chain, 4> chains;
    SmallVector<std::pair<Part *, size_t>, 4> keys;
    bool own = false;
    for (Part &p : scans) {
      ArrayRef<coff_relocation> rels = p.sc.getRelocs();
      for (size_t i = 0, n = rels.size(); p.sc.file->tlsModel && i != n; ++i) {
        Symbol *t = p.sc.getRelocTarget(rels[i]);
        bool index = rels[i].Type == IMAGE_REL_LLVM_FUSED_INDEX;
        if (getUse(p.sc, rels[i].Type) == FusedUse::None || !t)
          continue;
        // The key's index at an ARM64 decode is its chain's site, which the
        // chain's own index of the instruction writes.
        if (isKeyImport(t) && !index)
          keys.push_back({&p, i});
        else if (isKeyImport(t) && !p.out.empty())
          p.out[i].expr = R_NONE;
        if (isKeyImport(t) || !isTlsRecordPointerName(t->getName()))
          continue;
        auto *li = dyn_cast<DefinedLocalImport>(t);
        RelExpr e = p.out.empty() ? R_TYPE : p.out[i].expr;
        // A chain's own codes are below the record's words at 8 and 12.
        int64_t at = getRecordWord(p.sc, i);
        bool word = at == 8 || at == 12;
        Chain &c = chains[rels[i].SymbolTableIndex];
        own |= li && e != R_TYPE;
        c.ok &= e != R_TYPE || (li && word);
        if (word && index && isa<DefinedImportData>(t))
          (e == R_TLS_TP ? c.tp : c.notTp) = true;
        if (e == R_TLS && isa<DefinedLocalImport>(p.out[i].sym) && !li)
          released.try_emplace(rels[i].SymbolTableIndex, p.out[i].sym);
      }
    }
    for (Part &p : scans)
      for (size_t j = 0, n = p.out.size(); j != n; ++j) {
        auto it = chains.find(p.sc.getRelocs()[j].SymbolTableIndex);
        if (it != chains.end() && !it->second.ok &&
            getUse(p.sc, p.out[j].type) != FusedUse::None)
          reset(p, j);
        else if (it != chains.end() && it->second.ok && it->second.tp &&
                 !it->second.notTp && p.out[j].expr != R_NONE &&
                 p.out[j].type == IMAGE_REL_LLVM_FUSED_INDEX &&
                 unsigned(getRecordWord(p.sc, j)) <= SiteFactIndexForm)
          p.out[j].expr = R_DEAD;
      }
    bool keyFree = !chains.empty();
    for (auto [index, c] : chains)
      if (!c.ok)
        keyFree = false, released.erase(index), kept.insert(index);
    for (auto [p, i] : keyFree ? ArrayRef(keys) : ArrayRef(keys).take_front(0)) {
      ArrayRef<coff_relocation> rels = p->sc.getRelocs();
      Relocation &r = p->out[i];
      bool x64 = isX64(p->sc), decode = false;
      TlsForm form = getTlsForm(p->sc);
      bool stays = own && (form == TlsForm::Wide || form == TlsForm::ExeWide);
      // An x86-64 decode of the key in memory stays where its own marker
      // beside the key's says so; a load of the key alone, which chains
      // share, stays where an own chain's decode does.
      for (size_t j : {i - 1, i + 1})
        if (x64 && j < rels.size() && rels[j].VirtualAddress == r.offset &&
            rels[j].Type == IMAGE_REL_LLVM_FUSED_INDEX)
          decode = true, stays = p->out[j].expr == R_NONE;
      r.expr = stays    ? (x64 ? R_LOCAL_ACCESS : R_TLS)
               : decode ? R_NONE
                        : R_DEAD;
      r.sym = stays ? p->sc.file->symtab.keySym : r.sym;
      kept.erase(rels[i].SymbolTableIndex);
      released.try_emplace(rels[i].SymbolTableIndex, r.sym);
    }
  }

  // An ARM64 site reads the page only where every reference to its pointer
  // in the function is a fused read, the adrp only that reads, or a site,
  // and no site still reads the pointer.
  SmallDenseSet<uint32_t, 4> loaded;
  for (Part &p : paged.empty() ? MutableArrayRef<Part>() : MutableArrayRef(scans))
    for (size_t i : p.reads)
      loaded.insert(p.sc.getRelocs()[i].SymbolTableIndex);
  for (Part &p : paged.empty() ? MutableArrayRef<Part>() : MutableArrayRef(scans)) {
    ArrayRef<coff_relocation> rels = p.sc.getRelocs();
    for (size_t i = 0, e = rels.size(); i != e && !paged.empty(); ++i) {
      uint32_t index = rels[i].SymbolTableIndex;
      if (relocationNamesNoSymbol(rels[i].Type) || !paged.contains(index) ||
          (loaded.contains(index) && !kept.contains(index) &&
           (getUse(p.sc, rels[i].Type) != FusedUse::None ||
            (rels[i].Type == IMAGE_REL_LLVM_FUSED_READ && i &&
             isAddressPair(p.sc, rels[i - 1], rels[i])) ||
            (i + 1 != e && rels[i + 1].Type == IMAGE_REL_LLVM_FUSED_READ &&
             isAddressPair(p.sc, rels[i], rels[i + 1])))))
        continue;
      paged.erase(index);
      kept.insert(index);
      // An access of a pointer no fused read loads waits on none.
      for (Part &q : scans)
        for (size_t j = 0, n = q.out.size(); j != n; ++j)
          if (q.out[j].expr == R_PAGEOFF &&
              q.sc.getRelocs()[j].SymbolTableIndex == index)
            !loaded.contains(index) &&
                    q.out[j].type == IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL
                ? void(q.out[j].expr = R_LITERAL_ACCESS)
                : reset(q, j);
    }
  }

  SectionChunk *first = nullptr, *prev = nullptr;
  for (Part &p : scans) {
    SectionChunk &sc = p.sc;
    ObjFile *file = sc.file;
    bool x64 = isX64(sc);
    ArrayRef<coff_relocation> rels = sc.getRelocs();
    SmallVector<Relocation, 0> &out = p.out;
    // A reference to the key that stays reads its import, which an image
    // whose every chain relaxes leaves out.
    auto readKey = [&] {
      for (size_t i = 0, e = rels.size(); i != e; ++i)
        if (auto *imp = dyn_cast_or_null<DefinedImportData>(
                out.empty() ? sc.getRelocTarget(rels[i]) : out[i].sym);
            imp && isKeyImport(imp) &&
            (out.empty() || (out[i].expr != R_DEAD && out[i].expr != R_NONE)))
          imp->file->uses |= ImportFile::Read;
    };
    if (out.empty()) {
      readKey();
      continue;
    }
    // An x86-64 load of a record's word is dead where every index of the
    // word takes it for itself; where a site of the pointer stays, it gives
    // its register the word.
    for (size_t i = 0, e = x64 ? out.size() : 0; i != e; ++i)
      if (out[i].expr == R_DEAD &&
          rels[i].Type == IMAGE_REL_LLVM_FUSED_ACCESS &&
          kept.contains(rels[i].SymbolTableIndex))
        out[i].expr = R_TLS;
    // A fused read of a pointer the link binds, a local import's or a
    // delay-loaded import's, is dead exactly where every fused site of the
    // pointer in its function stops reading its register. The adrp only a
    // dead ARM64 read reads goes with it, or gives the page of the target to
    // the sites that take an offset in it.
    for (size_t i : p.reads) {
      const coff_relocation &rel = rels[i];
      Symbol *sym = sc.getRelocTarget(rel);
      auto *li = dyn_cast_or_null<DefinedLocalImport>(sym);
      auto *imp = dyn_cast_or_null<DefinedImportData>(sym);
      auto free = released.find(rel.SymbolTableIndex);
      if (!bindsFusedSites(sc) ||
          !(li ||
            (imp && (imp->file->delayLoaded || free != released.end()))) ||
          (x64 && li ? out[i].expr == R_TYPE : !checkSite(sc, rel)) ||
          kept.contains(rel.SymbolTableIndex))
        continue;
      out[i].expr = R_DEAD;
      if (x64)
        checkOrder(p, rel);
      else if (i && isAddressPair(sc, rels[i - 1], rel))
        paged.contains(rel.SymbolTableIndex) ? reset(p, i - 1, li->getTarget())
        : imp && free != released.end() &&
                isa<DefinedSynthetic, DefinedLocalImport>(free->second)
            ? reset(p, i - 1, free->second)
            : void(out[i - 1].expr = R_DEAD);
    }
    readKey();
    // What still refers to a thread-local variable's record, or to its
    // pointer, names the record, which the image then holds.
    if (!file->symtab.tlsRecords.empty())
      for (const Relocation &r : out) {
        auto *li = dyn_cast_or_null<DefinedLocalImport>(r.sym);
        TlsRecordChunk *rec = TlsRecordChunk::get(li ? li->getTarget() : r.sym);
        if (rec && r.expr != R_DEAD && r.expr != R_NONE)
          rec->named = true;
      }
    file->relocations[&sc].relocs = std::move(out);
    // The parts the scan keeps are a ring.
    if (prev)
      file->relocations[prev].next = &sc;
    first = first ? first : &sc;
    prev = &sc;
  }
  if (prev != first)
    prev->file->relocations[prev].next = first;
}

// Reports LNK4217 for another producer's references that keep a local import
// pointer, and leaves out each pointer no reference keeps. A fork object's
// import form is its compiler's default, so its kept pointer is no warning.
static void postScan(COFFLinkerContext &ctx) {
  for (ObjFile *file : ctx.objFileInstances)
    for (auto [li, use] : file->localImportUses)
      if (!file->forkObject &&
          (use & (LocalImportChunk::Load | LocalImportChunk::Read)))
        file->symtab.reportLocalImport(toString(file), li);
  ctx.forEachSymtab([](SymbolTable &symtab) {
    llvm::erase_if(symtab.localImportChunks, [](Chunk *c) {
      auto *lc = cast<LocalImportChunk>(c);
      if (lc->cell)
        lc->cell->live = lc->isLive();
      return !lc->isLive() || lc->cell;
    });
  });
}

// The import of the name imp's DLL exports for the address offset bytes
// after imp, X$so<offset> or X$ap<offset> (section 8.5).
static DefinedImportData *findInteriorImport(SymbolTable &symtab,
                                             DefinedImportData *imp,
                                             int64_t offset) {
  if (offset <= 0)
    return nullptr;
  for (StringRef kind : {"$so", "$ap"}) {
    DefinedImportData *interior =
        symtab.findImport(imp->getName() + kind + Twine(offset),
                          "an address inside " + imp->getName());
    if (interior &&
        interior->getDLLName().equals_insensitive(imp->getDLLName())) {
      interior->file->live = true;
      return interior;
    }
  }
  return nullptr;
}

// The imports that addresses inside them name, each with the first place.
using ImportBases =
    MapVector<ImportFile *, std::pair<SectionChunk *, uint32_t>>;

// Appends a section's words in the order of their offsets, identical ones
// once.
static void addWords(std::vector<ImportSlot> &from,
                     std::vector<ImportSlot> &to) {
  for (auto i = from.begin(), e = from.end(); i != e;) {
    auto j = std::find_if(
        i, e, [&](const ImportSlot &s) { return s.chunk != i->chunk; });
    std::stable_sort(i, j, [](const ImportSlot &a, const ImportSlot &b) {
      return a.offset < b.offset;
    });
    i = j;
  }
  std::unique_copy(from.begin(), from.end(), std::back_inserter(to),
                   [](const ImportSlot &a, const ImportSlot &b) {
                     return a.chunk == b.chunk && a.offset == b.offset &&
                            a.sym == b.sym;
                   });
}

Relocation &lld::coff::getScannedAt(SectionChunk &sc, uint32_t o) {
  auto [it, made] = sc.file->relocations.try_emplace(&sc);
  if (made)
    for (const coff_relocation &rel : sc.getRelocs())
      it->second.relocs.push_back({rel.VirtualAddress, rel.Type, R_TYPE, false,
                                   sc.getRelocTarget(rel)});
  return *llvm::find_if(it->second.relocs,
                        [&](const Relocation &r) { return r.offset == o; });
}

// Finds the delay-bound words and sections (section 15.3): the whole words
// of loaded data that hold the address of a function whose address is its
// delay-loaded DLL's own, and the sections that hold one or, closed over
// words of data, an address in such a section, each with the DLLs whose
// gates a site reaching it tests. A word holding an address inside the
// function keeps the thunk's.
static void findDelayBound(COFFLinkerContext &ctx) {
  if (llvm::none_of(ctx.importFileInstances,
                    [](ImportFile *f) { return f->observed; }))
    return;
  auto forEachWord = [&](auto fn) {
    for (ObjFile *file : ctx.objFileInstances)
      for (Chunk *c : file->getChunks())
        if (auto *sc = dyn_cast<SectionChunk>(c);
            sc && sc->live && isLoaded(*sc) &&
            !(sc->header->Characteristics &
              (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)))
          for (const coff_relocation &rel : sc->getRelocs())
            if (rel.Type == (isX64(*sc) ? IMAGE_REL_AMD64_ADDR64
                                        : IMAGE_REL_ARM64_ADDR64) &&
                uint64_t(rel.VirtualAddress) + 8 <= sc->getSize())
              fn(*sc, rel.VirtualAddress, sc->getRelocTarget(rel));
  };
  std::vector<ImportSlot> words;
  forEachWord([&](SectionChunk &sc, uint32_t o, Symbol *sym) {
    DefinedImportData *imp = nullptr;
    if (auto *t = dyn_cast_or_null<DefinedImportThunk>(sym))
      imp = t->wrappedSym;
    else if (sym && sym->addressImported)
      imp = sc.file->symtab.vagueEntities.find(sym)->second.owner;
    if (!imp || !imp->file->observed || read64le(sc.getContents().data() + o))
      return;
    words.push_back({&sc, imp, o, false});
    COFFLinkerContext::DelayBound &b = ctx.delayBound[&sc];
    b.dlls.insert(saver().save(imp->getDLLName().lower()));
    b.words.push_back(o);
  });
  addWords(words, ctx.delayWords);
  for (bool more = !words.empty(); more;) {
    more = false;
    forEachWord([&](SectionChunk &sc, uint32_t o, Symbol *sym) {
      auto *d = dyn_cast_or_null<DefinedRegular>(sym);
      auto it = d ? ctx.delayBound.find(d->getChunk()) : ctx.delayBound.end();
      if (it == ctx.delayBound.end() || d->getChunk() == &sc)
        return;
      SmallVector<StringRef, 2> dlls(it->second.dlls.getArrayRef());
      COFFLinkerContext::DelayBound &b = ctx.delayBound[&sc];
      if (!llvm::is_contained(b.words, o))
        b.words.push_back(o);
      for (StringRef dll : dlls)
        more |= b.dlls.insert(dll);
    });
  }
}

// Gives the image the in-place slots the scan found and binds the addresses
// inside an import. Returns whether the image may hold them.
static bool bindImportSlots(COFFLinkerContext &ctx,
                            MutableArrayRef<ImportSites> found,
                            ImportBases &bases) {
  // The loader adds nothing to an import's address: a word or lea naming an
  // address inside the import takes the name its DLL exports for it. Where it
  // exports none, the image's fill writes a word inside data from the
  // import's entry, which the import keeps, as it writes every word holding
  // the address of an import it binds.
  auto bindInterior = [&](ImportSlot &s, int64_t addend, RelExpr expr) {
    ObjFile *file = s.chunk->file;
    bool late = expr == R_IMPORT_SLOT && s.sym->file->binding;
    DefinedImportData *interior =
        late ? nullptr : findInteriorImport(file->symtab, s.sym, addend);
    if (!interior && expr == R_IMPORT_SLOT &&
        (late || s.sym->file->hdr->getType() != IMPORT_CODE)) {
      for (Relocation &r : s.chunk->getScanned()->relocs)
        if (r.expr == expr && r.offset == s.offset)
          r.expr = R_FILL_WORD;
      s.sym->file->uses |= ImportFile::Keep;
      ctx.fill.words.push_back(
          {s.chunk, s.offset, s.sym, addend, FillWord::Address});
      return false;
    }
    if (!interior) {
      StringRef name = s.sym->getName().substr(strlen("__imp_"));
      errAt(*s.chunk, s.offset)
          << "addresses " << name << "+" << addend << ", but "
          << s.sym->getDLLName() << " exports neither " << name << "$so"
          << addend << " nor " << name << "$ap" << addend;
      return false;
    }
    for (Relocation &r : s.chunk->getScanned()->relocs)
      if (r.offset == s.offset &&
          (r.expr == expr || r.expr == R_GUARD || r.expr == R_GUARD_CALL))
        r.sym = interior;
    bases.try_emplace(s.sym->file, s.chunk, s.offset);
    if (expr == R_IMPORT_LOAD)
      interior->file->uses |= ImportFile::Read;
    s.sym = interior;
    return true;
  };
  for (ImportSites &sites : found) {
    // An address inside an import is the name its DLL exports for it.
    for (ImportSlot &s : sites.interior)
      bindInterior(s,
                   int32_t(read32le(s.chunk->getContents().data() + s.offset)),
                   R_IMPORT_LOAD);
    // A guarded read of a local import's pointer whose register nothing
    // reads is dead. An ARM64 site's guard follows its section.
    for (DelayGuardSite g : sites.guarded) {
      Relocation &r = g.chunk->getScanned()->relocs[g.index];
      if (r.expr != R_GUARD && r.expr != R_GUARD_CALL)
        continue;
      g.value = cast<Defined>(r.sym);
      if (isX64(*g.chunk)) {
        r.sym = getDelayGuard(ctx, g, r.expr == R_GUARD_CALL);
        continue;
      }
      if (Defined *d = addDelayGuardA64(ctx, g)) {
        r.sym = d;
        continue;
      }
      r.expr = R_TYPE;
      errAt(*g.chunk, r.offset)
          << "reaches " << g.value->getName()
          << ", whose address is a delay-loaded DLL's, in a function whose "
             "unwind data is neither its own nor packed, which a guard cannot "
             "extend";
    }
    std::vector<ImportSlot> &slots = sites.slots;
    if (!slots.empty() && (ctx.hybridSymtab || isArm64EC(ctx.config.machine))) {
      Err(ctx) << slots[0].chunk->file << ": "
               << slots[0].chunk->getSectionName()
               << " holds the address of an import, " << slots[0].sym->getName()
               << ", in a word of data, which the loader binds in place only "
                  "in an x64 or ARM64 image";
      return false;
    }
    llvm::erase_if(slots, [&](ImportSlot &s) {
      int64_t addend = read64le(s.chunk->getContents().data() + s.offset);
      return (addend || s.sym->file->binding) &&
             !bindInterior(s, addend, R_IMPORT_SLOT);
    });
    addWords(slots, ctx.importSlots);
  }
  return true;
}

COFFSyncStream lld::coff::reportFillWord(COFFLinkerContext &ctx,
                                         const FillWord &w) {
  auto *imp = cast<DefinedImportData>(w.source);
  StringRef name = imp->getName().substr(strlen("__imp_"));
  auto *sc = dyn_cast<SectionChunk>(w.chunk);
  COFFSyncStream e = sc ? errAt(*sc, w.offset) : Err(ctx);
  if (sc)
    e << "holds an address inside " << name << ", for which "
      << imp->getDLLName() << " exports no name";
  else if (w.kind == FillWord::Import)
    e << imp->getName() << ", imported from " << imp->getDLLName()
      << ", is bound by name as the image starts";
  else
    e << "the image reads a word that it writes from " << name;
  return e;
}

// Leaves out each import thunk that only references to the function's own
// address named, and each import only addresses inside it name.
static void selectImports(COFFLinkerContext &ctx, const ImportBases &bases) {
  // A root, an export among them, or a local import pointer that names a
  // thunk keeps it, and one that names an import's pointer keeps its entry.
  auto keep = [](Symbol *s) {
    if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s))
      thunk->wrappedSym->file->uses |= ImportFile::Thunk;
    else if (auto *imp = dyn_cast_or_null<DefinedImportData>(s))
      imp->file->uses |= ImportFile::Keep;
  };
  for (Symbol *s : ctx.config.gcroot)
    keep(s->getDefined());
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    for (Chunk *c : symtab.localImportChunks)
      keep(cast<LocalImportChunk>(c)->getTarget());
  });
  // A thunk only in-place words or loaded addresses named goes; one that
  // stays reads the pointer.
  for (ImportFile *file : ctx.importFileInstances) {
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(file->thunkSym);
    if (!thunk ||
        !(thunk->getChunk()->live || (file->uses & ImportFile::Thunk)))
      continue;
    thunk->getChunk()->live =
        (file->uses & ImportFile::Thunk) ||
        !(file->uses & (ImportFile::InPlace | ImportFile::Loaded));
    if (thunk->getChunk()->live)
      file->uses |= ImportFile::Read;
  }
  // An import only addresses inside it name is not imported itself; its
  // symbol stays at the first of them, for what describes the image.
  DenseSet<ImportFile *> slotted;
  for (const ImportSlot &s : ctx.importSlots)
    slotted.insert(s.sym->file);
  for (auto &[file, place] : bases)
    if (!file->uses && !slotted.contains(file)) {
      file->live = false;
      file->location = place.first;
      file->offset = place.second;
    }
  // Nor is an import of an overridable default whose calls all reach what
  // the link binds them to, nor the runtime's key where every chain that
  // reads it relaxes, where nothing else uses it.
  for (ImportFile *file : ctx.importFileInstances)
    if (file->live && !file->uses && !file->observed &&
        !slotted.contains(file) &&
        (ctx.symtab.findOverrideSlot(file->externalName) >= 0 ||
         file->externalName == COFF::TP::Key))
      file->live = false;
}

// Whether relocation i of sc, of expression expr, is a direct call or jump to
// its symbol that the link may retarget: not a REL32_CALL, whose caller checks
// its destination's type prefix, nor a JUMP8 that keeps its width.
static bool isBranchSite(const SectionChunk &sc, const coff_relocation &rel,
                         RelExpr expr) {
  return expr == R_LOCAL_CALL || expr == R_LOCAL_JUMP ||
         ((expr == R_JUMP ||
           (expr == R_TYPE && rel.Type != IMAGE_REL_LLVM_JUMP8)) &&
          isDirectBranch(sc, rel));
}

bool lld::coff::isFunctionOverrideSite(const SectionChunk &sc,
                                       const coff_relocation &rel,
                                       RelExpr expr) {
  if (!sc.file->forkObject && isX64(sc))
    return rel.Type == IMAGE_REL_AMD64_REL32 && isCode(sc) &&
           !read32(sc, rel.VirtualAddress) &&
           classifyForeign(sc, rel).kind == SiteKind::Branch;
  bool local = expr == R_LOCAL_CALL || expr == R_LOCAL_JUMP;
  return isBranchSite(sc, rel, expr) &&
         (!isX64(sc) ? local || rel.Type == IMAGE_REL_ARM64_BRANCH26 ||
                           rel.Type == IMAGE_REL_LLVM_BRANCH_TAIL
          : local    ? isPointerBranch(rel.Type)
                     : rel.Type != IMAGE_REL_LLVM_JUMP8);
}

// Where the destination of the unconditional jump sym's place begins with is
// held, or null: the place is a fork object's code the image cannot write,
// the jump one its relocation states with no addend, its destination in the
// image, and neither the place nor the destination a function override,
// whose sites the kernel rewrites.
static Symbol *const *getJumpAt(Symbol *sym) {
  auto *d = dyn_cast_or_null<DefinedRegular>(sym);
  SectionChunk *sc = d ? d->getChunk() : nullptr;
  if (!sc || !sc->live || !sc->file->forkObject || FunctionOverride::get(d) ||
      (sc->getOutputCharacteristics() &
       (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) != IMAGE_SCN_MEM_EXECUTE)
    return nullptr;
  bool x64 = isX64(*sc);
  uint32_t v = d->getValue();
  ArrayRef<uint8_t> b = sc->getContents();
  ArrayRef<coff_relocation> rels = sc->getRelocs();
  ScannedSection *s = sc->getScanned();
  for (size_t i = llvm::partition_point(rels,
                                        [&](const coff_relocation &r) {
                                          return r.VirtualAddress < v;
                                        }) -
                  rels.begin();
       i != rels.size() && rels[i].VirtualAddress <= v + (x64 ? 3 : 0); ++i) {
    RelExpr expr = s ? s->relocs[i].expr : R_TYPE;
    uint32_t before = rels[i].VirtualAddress - v;
    if (expr == R_LOCAL_JUMP
            ? before != (x64 ? getBytesBefore(rels[i].Type) : 0)
            : !isBranchSite(*sc, rels[i], expr) ||
                  (x64 ? before != 1 ||
                             b[v] != (rels[i].Type == IMAGE_REL_LLVM_JUMP8
                                          ? 0xeb
                                          : 0xe9)
                       : read32le(&b[v]) != 0x14000000))
      continue;
    Symbol *const *to = s ? &s->relocs[i].sym
                          : &sc->file->getSymbols()[rels[i].SymbolTableIndex];
    return isa_and_nonnull<Defined>(*to) && !isa<DefinedAbsolute>(*to) &&
                   !FunctionOverrideThunk::get(*to)
               ? to
               : nullptr;
  }
  return nullptr;
}

// Retargets each direct call or jump of a fork object whose destination
// begins with a jump, as lld/ELF does at the end of its scan. The objects go
// in order, so a cycle of jumps ends at one place in every link.
static void applyBranchToBranch(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Branch to branch");
  auto info = [](Symbol *const &sym, uint64_t) {
    return std::pair<Symbol *const *, uint64_t>(getJumpAt(sym), 0);
  };
  for (ObjFile *file : ctx.objFileInstances) {
    for (Chunk *c :
         file->forkObject ? file->getChunks() : ArrayRef<Chunk *>()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live ||
          !(sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE))
        continue;
      ArrayRef<coff_relocation> rels = sc->getRelocs();
      ScannedSection *s = sc->getScanned();
      for (size_t i = 0, e = rels.size(); i != e; ++i) {
        RelExpr expr = s ? s->relocs[i].expr : R_TYPE;
        if (!isBranchSite(*sc, rels[i], expr))
          continue;
        Symbol *const from = sc->getTarget(i, s);
        Symbol *to = from;
        redirectBranchToBranch(
            from, 0, info, [&](Symbol *const &, Symbol *const &t) { to = t; });
        // A branch reaches a function override's default body only as a site.
        if (to == from || (FunctionOverride::get(to) &&
                           !isFunctionOverrideSite(*sc, rels[i], expr)))
          continue;
        if (!s) {
          s = &file->relocations[sc];
          for (const coff_relocation &rel : rels)
            s->relocs.push_back({rel.VirtualAddress, rel.Type, R_TYPE, false,
                                 sc->getRelocTarget(rel)});
        }
        s->relocs[i].sym = to;
        if (!isX64(*sc))
          ctx.retargets[&s->relocs[i]] = from;
      }
    }
  }
}

// Whether the link deletes r, the jump that ends resizable section c, where
// the next chunk begins at its target: a BRANCH_TAIL with no addend, or a
// MEM_JUMP_TAIL or FUSED_JUMP_TAIL made direct, to a section's start that is
// no function override's body, with no symbol inside its bytes [begin, end).
// A function that is only the jump would take its target's address, so it
// goes only where ICF may fold it.
bool lld::coff::deletesEndJump(COFFLinkerContext &ctx, const SectionChunk &c,
                               const Relocation &r, uint32_t &begin,
                               uint32_t &end,
                               function_ref<bool(uint32_t, uint32_t)> inside) {
  bool x64 = isX64(c);
  auto *d = dyn_cast_or_null<DefinedRegular>(r.sym);
  bool direct = r.type == IMAGE_REL_LLVM_BRANCH_TAIL && r.expr == R_TYPE;
  bool bound =
      r.expr == R_LOCAL_JUMP && (r.type == IMAGE_REL_LLVM_MEM_JUMP_TAIL ||
                                 r.type == IMAGE_REL_LLVM_FUSED_JUMP_TAIL);
  begin = x64 ? r.offset - getBytesBefore(r.type) : r.offset;
  end = r.offset + 4;
  if ((!direct && !bound) || end != c.getContents().size() || !d ||
      d->getValue() || FunctionOverride::get(d) ||
      (direct && (read32(c, r.offset) & (x64 ? ~0u : 0x3ffffffu))) ||
      (r.type == IMAGE_REL_LLVM_MEM_JUMP_TAIL && read32(c, r.offset)) ||
      inside(begin, end))
    return false;
  return begin || (ctx.config.doICF != ICFLevel::None && !c.keepUnique);
}

void lld::coff::forEachBranch(
    COFFLinkerContext &ctx, bool chainsOnly,
    function_ref<void(const SectionChunk &, uint32_t, const SectionChunk &,
                      bool, bool)>
        fn) {
  // The symbols in the last bytes of resizable sections, which an end jump
  // may hold.
  DenseMap<const SectionChunk *, SmallVector<uint32_t, 1>> ends;
  for (ObjFile *file : ctx.objFileInstances)
    for (auto [c, record] : file->movingSymbols)
      if (record->Value + 8 >= c->getSize())
        ends[c->repl].push_back(record->Value);
  for (ObjFile *file : ctx.objFileInstances) {
    for (Chunk *c :
         file->forkObject ? file->getChunks() : ArrayRef<Chunk *>()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live ||
          !(sc->getOutputCharacteristics() & IMAGE_SCN_MEM_EXECUTE) ||
          (chainsOnly && !sc->resizable))
        continue;
      ScannedSection *s = sc->getScanned();
      bool x64 = isX64(*sc);
      ArrayRef<uint8_t> b = sc->getContents();
      ArrayRef<coff_relocation> rels = sc->getRelocs();
      // A jump that chains ends its section, whose relocations are in order.
      size_t i =
          !chainsOnly
              ? 0
              : llvm::partition_point(rels, [&](const coff_relocation &r) {
                  return r.VirtualAddress + 8 < b.size();
                }) - rels.begin();
      for (size_t e = rels.size(); i != e; ++i) {
        const coff_relocation &rel = rels[i];
        RelExpr expr = s ? s->relocs[i].expr : R_TYPE;
        if (!isBranchSite(*sc, rel, expr))
          continue;
        // A function override is its stub, which is no section.
        auto *d = dyn_cast_or_null<Defined>(sc->getTarget(i, s));
        auto *to = d ? dyn_cast_or_null<SectionChunk>(d->getChunk()) : nullptr;
        if (!to || to == sc || !to->live)
          continue;
        uint32_t o = rel.VirtualAddress, begin, end;
        bool local = expr == R_LOCAL_JUMP;
        bool jump = local || (x64 ? (rel.Type == IMAGE_REL_LLVM_JUMP32 ||
                                     rel.Type == IMAGE_REL_LLVM_BRANCH_TAIL) &&
                                        o && b[o - 1] == 0xe9
                                  : rel.Type == IMAGE_REL_LLVM_BRANCH_TAIL);
        auto *entry = dyn_cast<DefinedRegular>(d);
        bool chains =
            jump && sc->resizable && entry && !FunctionOverride::get(entry) &&
            o + (x64 && local ? getBytesFrom(*sc, rel) : 4) == b.size() &&
            (!entry->getValue() || entry->getCOFFSymbol().getComplexType() ==
                                       IMAGE_SYM_DTYPE_FUNCTION) &&
            to->getSectionName() == sc->getSectionName() &&
            to->getOutputCharacteristics() == sc->getOutputCharacteristics();
        auto inside = [&](uint32_t from, uint32_t until) {
          auto it = ends.find(sc);
          return it != ends.end() && llvm::any_of(it->second, [&](uint32_t x) {
                   return x > from && x < until;
                 });
        };
        fn(*sc, o, *to, chains,
           chains &&
               deletesEndJump(ctx, *sc, s->relocs[i], begin, end, inside));
      }
    }
  }
}

bool lld::coff::continuesRun(const ImportSlot *prev, const ImportSlot &s,
                             bool across) {
  if (!prev || !prev->sym->getDLLName().equals_insensitive(s.sym->getDLLName()))
    return false;
  SectionChunk *last = prev->chunk;
  if (last == s.chunk)
    return s.offset == prev->offset + 8;
  return across && !s.offset && prev->offset + 8 == last->getSize() &&
         s.chunk->getAlignment() <= last->getAlignment() &&
         last->getSize() % s.chunk->getAlignment() == 0;
}

void lld::coff::scanRelocations(COFFLinkerContext &ctx) {
  bool foreign = false;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    foreign |= !symtab.localImportChunks.empty() ||
               llvm::any_of(symtab.functionOverrides,
                            [](FunctionOverride *fo) { return fo->stub; });
  });
  if (!ctx.hasForkObject && !foreign)
    return;
  llvm::TimeTraceScope timeScope("Scan relocations");
  classifyImports(ctx);
  findDelayBound(ctx);
  // A foreign object is scanned for what it refers to: a local import
  // pointer, a function override's stub, or in an image holding a fork
  // object, an import or what the image reaches through a cell.
  auto refers = [&](ObjFile *file) {
    return llvm::any_of(file->getSymbols(), [&](Symbol *s) {
      return isa_and_nonnull<DefinedLocalImport>(s) ||
             FunctionOverrideThunk::get(s) ||
             (ctx.hasForkObject && s &&
              (isa<DefinedImportData>(s) || isa<DefinedImportThunk>(s) ||
               s->addressImported || file->symtab.findOwnDefault(s)));
    });
  };
  // An ARM64 movz gives a register an offset in the TLS groups, whose
  // sections bound it; a section merged into them is not counted.
  uint64_t tls = 0;
  for (ObjFile *file : ctx.objFileInstances)
    for (Chunk *c : file->symtab.tlsRecords.empty() ? ArrayRef<Chunk *>()
                                                    : file->getChunks())
      if (auto *sc = dyn_cast<SectionChunk>(c);
          sc && sc->live && sc->getSectionName().starts_with(".tls"))
        tls += sc->getSize() + sc->getAlignment();
  ctx.tlsFitsMovz =
      tls <= 0x10000 && llvm::none_of(ctx.config.merge, [](const auto &m) {
        return m.second == ".tls";
      });
  // A record chain relaxed into a DLL's or a wide image's form reads the
  // image's place word.
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    if (!ctx.tlsRuntime().empty() && !symtab.tlsRecords.empty() &&
        (ctx.config.dll || ctx.tlsWide))
      symtab.getPlaceWord();
  });
  // The runtime's fill is no part of the image until a fact needs it.
  if (Symbol *fill = ctx.symtab.fill)
    for (StringRef form : {"", "_imports"})
      dropBuiltin(ctx.symtab.find((fill->getName() + form).str()));
  auto scan = [&](ObjFile *file, ArrayRef<Chunk *> chunks, ImportSites &found) {
    if (!file->forkObject && !refers(file))
      return;
    // A function's code is its section and those associative to it; the
    // code in no COMDAT is one where its functions' cold parts share a
    // section.
    LocalImportUses uses;
    SmallVector<SectionChunk *, 0> plain;
    bool split = llvm::any_of(chunks, [](Chunk *c) {
      auto *sc = dyn_cast<SectionChunk>(c);
      return sc && !sc->isCOMDAT() && sc->getSectionName() == ".text$split";
    });
    for (Chunk *c : chunks) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || sc->selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE)
        continue;
      // A section associative to one the link left out may stay.
      SmallVector<SectionChunk *, 2> parts;
      if (sc->live)
        parts.push_back(sc);
      for (SectionChunk &child : sc->children())
        if (child.live)
          parts.push_back(&child);
      if (split && isCode(*sc) && !sc->isCOMDAT())
        llvm::append_range(plain, parts);
      else
        scanFunction(parts, uses, found);
    }
    scanFunction(plain, uses, found);
    for (auto [li, use] : uses)
      li->getChunk()->uses |= use;
    llvm::append_range(file->localImportUses, uses);
  };
  std::vector<ImportSites> sites(ctx.objFileInstances.size());
  parallelFor(0, sites.size(), [&](size_t i) {
    scan(ctx.objFileInstances[i], ctx.objFileInstances[i]->getChunks(),
         sites[i]);
  });
  ImportBases bases;
  bool bound = bindImportSlots(ctx, sites, bases);
  // The fill's sections are scanned once the scan's results decide that the
  // image has the fill: a scan's uses of a pointer are not undone.
  SmallVector<SectionChunk *, 8> kept;
  selectFill(ctx, kept);
  ImportSites more;
  for (Chunk *c : kept)
    scan(cast<SectionChunk>(c)->file, c, more);
  bindImportSlots(ctx, more, bases);
  postScan(ctx);
  if (bound)
    selectImports(ctx, bases);
  if (ctx.hasForkObject)
    applyBranchToBranch(ctx);
}
