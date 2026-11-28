//===- Relaxation.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The forms the scan's sites take, and the relaxation of the sections the
// object lists as resizable, on the core lld/ELF's RISC-V and LoongArch
// share: each site is an edit whose size the passes choose, starting at its
// smallest and only growing, and a section a value does not fit keeps its
// size, its sites written in their same-size forms.
//
//===----------------------------------------------------------------------===//

#include "Relocations.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "DLL.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Walker.h"
#include "Writer.h"
#include "lld/Common/AArch64Instructions.h"
#include "lld/Common/Relaxation.h"
#include "lld/Common/X86Instructions.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include <atomic>

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::coff;

//===----------------------------------------------------------------------===//
// The forms sites take.
//===----------------------------------------------------------------------===//

// The offset in the page an ARM64 site of the type already adds.
static uint32_t getSiteOffset(uint32_t insn, uint16_t type) {
  return ((insn >> 10) & 0xfff)
         << (type == IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL
                 ? getArm64LdrScale(insn)
                 : 0);
}

// Adds the offset of s in its page to what the ARM64 site insn of the type
// adds to its register, or with own, where the site has its adrp to itself,
// makes the offset of s plus its own the offset in the page that adrp gave.
// Returns false where the instruction cannot hold the sum.
static bool addPageOffset(uint32_t &insn, uint16_t type, uint64_t s, bool own) {
  // A pair's offset has seven bits and a sign, scaled by its registers' size.
  if (type == IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL &&
      (insn & 0x3b800000) == 0x29000000) {
    unsigned scale = 2 + (insn & 0x04000000 ? insn >> 30 : insn >> 31);
    int64_t v = SignExtend64<7>(insn >> 15) * (1 << scale) + (s & 0xfff);
    if ((v & ((1 << scale) - 1)) || !isInt<7>(v >> scale))
      return false;
    insn = (insn & ~(0x7f << 15)) | ((v >> scale) & 0x7f) << 15;
    return true;
  }
  unsigned scale = type == IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL
                       ? getArm64LdrScale(insn)
                       : 0;
  uint64_t v = (getSiteOffset(insn, type) + (own ? s : s & 0xfff)) &
               (own ? 0xfff : ~0ull);
  if ((v & ((1 << scale) - 1)) || v >> scale > 0xfff)
    return false;
  insn = (insn & ~(0xfff << 10)) | (v >> scale) << 10;
  return true;
}

// The ARM64 insn, which adds two registers into a third or an address, with
// v in place of the second, or with first of the first, where v fits.
static std::optional<uint32_t> addArm64Offset(uint32_t insn, bool first,
                                              uint64_t v) {
  uint32_t base = (insn >> (first ? 16 : 5)) & 31, rt = insn & 31;
  if (base == 31)
    return std::nullopt;
  // add xd, xn, #v, or #(v >> 12), lsl #12 where v's low bits are clear.
  if ((insn & 0xffe0fc00) == 0x8b000000) {
    bool high = v > 0xfff && !(v & 0xfff);
    if (v >> (high ? 24 : 12))
      return std::nullopt;
    return 0x91000000 | high << 22 | (v >> (high ? 12 : 0)) << 10 | base << 5 |
           rt;
  }
  unsigned scale = getArm64LdrScale(insn);
  if ((v & ((1 << scale) - 1)) || v >> scale > 0xfff)
    return std::nullopt;
  return (insn & 0xc4c0001f) | 0x39000000 | (v >> scale) << 10 | base << 5;
}

// Whether the ARM64 site at relocation i has the adrp of its page to itself.
static bool ownsPage(const Relocation *relocs, size_t i) {
  return i >= 2 && relocs[i - 2].expr == R_SITE_PAGE;
}

static uint64_t getTlsOffset(const SectionChunk &sc, Symbol *sym) {
  COFFLinkerContext &ctx = sc.file->symtab.ctx;
  std::optional<uint64_t> offset =
      ctx.tlsOffset(cast<Defined>(sym)->getRVA(), ctx.tlsRuntime().empty());
  if (!offset)
    Err(ctx) << sc.file << ": " << toString(ctx, *sym)
             << ", which a thread-local variable's record states, is not in "
                "the image's thread-local data";
  return offset.value_or(0);
}

// Whether a resize of c deletes its dead site at relocation i: an ARM64 one
// is a no-operation in place where the assembler kept no size there.
static bool deletesDead(const SectionChunk &c, size_t i) {
  const coff_relocation &rel = c.getRelocs()[i];
  Symbol *t = c.getRelocTarget(rel);
  ArrayRef<uint8_t> b = c.getContents();
  return isX64(c) || relocationMayResizeCode(
                         c.getMachine(), rel.Type, t ? t->getName() : "",
                         rel.VirtualAddress + 8 > b.size()
                             ? 0
                             : read64le(&b[rel.VirtualAddress]));
}

// Calls fn with each scanned part of the function's code of which c is one,
// c first.
template <class Fn> static void forEachPart(const SectionChunk &c, Fn fn) {
  const SectionChunk *p = &c;
  do {
    ScannedSection &s = *p->getScanned();
    fn(*p, s);
    p = s.next;
  } while (p && p != &c);
}

// The x86-64 instruction of the site r, relocation i of sc, at RVA p, into
// code; returns the size of the object's. A select is a mov of the arm the
// image's form keeps, an access of a $tpoff word adds it to the thread
// pointer's register, a chain's index takes a link-time place, H or zero,
// and an access the variable's offset, as its displacement; a word's load
// becomes lea of the place word, mov of an offset, or mov of 64 bits from a
// $tpoff word.
static unsigned encodeTlsSite(const SectionChunk &sc, size_t i,
                              const Relocation &r, uint64_t p,
                              SmallVectorImpl<char> &code) {
  ArrayRef<uint8_t> b = sc.getContents().drop_front(r.offset);
  X86Decoder *dec = X86Decoder::get();
  ObjFile *file = sc.file;
  FusedInsn insn;
  unsigned field;
  int64_t v;
  bool second = !(file->getSiteFact(&sc, i, SiteFactIndex).value_or(0) &
                  SiteFactViaFirst);
  // An importer's chain, which reads $tpoff words, has no place.
  bool tpoff = isa<DefinedImportData>(r.sym);
  if (r.expr == R_TLS_MOVE) {
    auto [dest, kept] =
        getSelect(sc, i, tpoff ? TlsForm::Place : getTlsForm(sc));
    code.append({char(0x48 | (dest & 8) >> 1 | kept >> 3), char(0x8b),
                 char(0xc0 | (dest & 7) << 3 | (kept & 7))});
    return 4;
  }
  if (r.type == IMAGE_REL_LLVM_FUSED_INDEX) {
    dec->decodeFused(b, insn, &code, &field, second,
                     r.expr == R_TLS_TP
                         ? *file->getSiteFact(&sc, i, SiteFactThreadPointer)
                         : -1);
    if (r.expr == R_TLS_TP)
      return insn.size;
    ChainSite site = getChainSite(sc, i);
    v = site == ChainSite::Base   ? (tpoff ? 0 : *file->symtab.ctx.tlsPlace)
        : site == ChainSite::Wide ? TP_H
                                  : getTlsOffset(sc, r.sym) + insn.disp;
  } else {
    dec->decodeFused(b, insn);
    tpoff = isa<DefinedLocalImport>(r.sym);
    if (tpoff || insn.loads == 8)
      code.append({char(0x48 | (insn.dest & 8) >> 1), char(tpoff ? 0x8b : 0x8d),
                   char(0x05 | (insn.dest & 7) << 3)});
    else if (insn.dest & 8)
      code.append({char(0x41), char(0xb8 | (insn.dest & 7))});
    else
      code.push_back(char(0xb8 | insn.dest));
    field = code.size();
    code.append(4, 0);
    v = !tpoff && insn.disp
            ? getTlsOffset(sc, r.sym)
            : cast<Defined>(r.sym)->getRVA() - (p + code.size());
  }
  if (p && !isInt<32>(v))
    Err(file->symtab.ctx) << sc.file << ": a thread-local site's value of "
                          << sc.getSectionName() << " is out of range";
  write32le(code.data() + field, v);
  return insn.size;
}

TlsForm lld::coff::getTlsForm(const SectionChunk &sc) {
  COFFLinkerContext &ctx = sc.file->symtab.ctx;
  if (ctx.tlsWide)
    return ctx.config.dll ? TlsForm::Wide : TlsForm::ExeWide;
  std::optional<uint64_t> c = ctx.tlsPlace;
  // ARM64's add takes 12 bits, shifted or not.
  return c && (isX64(sc) ? isUInt<31>(*c)
                         : !(*c & ~0xfffull) || !(*c & ~0xfff000ull))
             ? TlsForm::Place
             : TlsForm::Direct;
}

// The site's size: the wide DLL's form, 22 bytes on x86-64 where its second
// load needs no displacement and 23 where it does, and six instructions on
// ARM64.
static unsigned getTlsSiteSize(const SectionChunk &sc, const uint8_t *b) {
  return isX64(sc) ? 22 + (b[9] >> 6 == 1) : 24;
}

// The bytes at the end of a site that its form keeps as compiled: a wide
// executable's key, its xor or its adrp, ldr and eor.
static unsigned getTlsKept(const SectionChunk &sc, TlsForm form) {
  return form == TlsForm::ExeWide ? (isX64(sc) ? 7 : 12) : 0;
}

// The ARM64 add of the thread pointer to the place that the site at b loads.
static uint32_t getTlsAdd(const uint8_t *b) {
  uint32_t t = read32le(b) & 31, tp = read32le(b + 8) >> 5 & 31;
  return 0x8b000000 | t << 16 | tp << 5 | t;
}

// Writes what the image's form makes of the base site whose object bytes are
// in, up to what it keeps, at out of RVA rva, and returns its size: compact,
// the form alone; else at the site's size, the bytes it leaves dead disposed
// of as section 9.4 does, segment overrides on the site's last instruction
// where the two fit 15 bytes and else no-operations before it on x86-64,
// no-operations after the form on ARM64.
static unsigned writeTlsBase(const SectionChunk &sc, const uint8_t *in,
                             uint8_t *out, uint64_t rva, bool compact) {
  TlsForm form = getTlsForm(sc);
  // Only a direct site loads the place word, which an image with a
  // link-time place may not have.
  uint64_t c = sc.file->symtab.ctx.tlsPlace.value_or(0),
           place = form == TlsForm::Direct
                       ? sc.file->symtab.placeWord->getRVA()
                       : 0;
  unsigned kept = getTlsKept(sc, form);
  unsigned size = getTlsSiteSize(sc, in) - kept, n = 0, last = 0;
  if (isX64(sc)) {
    unsigned t = (in[0] & 4) << 1 | (in[2] >> 3 & 7);
    unsigned b = (in[7] & 1) << 3 | (in[10] & 7);
    unsigned tp = b == t ? (in[7] & 2) << 2 | (in[10] >> 3 & 7) : b;
    // An instruction whose register operand is t and whose other is tp for
    // mod 3, tp plus a displacement for 2, or rip plus one for 0.
    uint8_t code[16];
    auto insn = [&](uint8_t op, unsigned mod, uint32_t disp, bool rorx) {
      last = n;
      unsigned rexB = mod ? tp >> 3 : 0;
      if (rorx) { // VEX.LZ.F2.0F3A.W1
        code[n++] = 0xc4;
        code[n++] = (~t & 8) << 4 | 0x40 | (~rexB & 1) << 5 | 3;
        code[n++] = 0xfb;
      } else {
        code[n++] = 0x48 | (t & 8) >> 1 | rexB;
      }
      code[n++] = op;
      code[n++] = mod << 6 | (t & 7) << 3 | (mod ? tp & 7 : 5);
      if (mod == 2 && (tp & 7) == 4)
        code[n++] = 0x24;
      if (mod != 3)
        write32le(code + n, disp), n += 4;
      if (rorx)
        code[n++] = TP_ROTATE;
    };
    if (form == TlsForm::Direct) // mov t, [rip + place]; add t, tp
      insn(0x8b, 0, place - (rva + 7), false), insn(0x03, 3, 0, false);
    else if (form == TlsForm::Place) // lea t, [tp + C]
      insn(0x8d, 2, c, false);
    else // rorx t, [tp + H], 17
      insn(0xf0, 2, TP_H, true);
    if (compact) {
      memcpy(out, code, n);
      return n;
    }
    unsigned dead = size - n, at = kept ? n : last;
    memcpy(out, code, at);
    if (dead + (kept ? kept : n - at) <= 15)
      memset(out + at, 0x2e, dead);
    else
      x86::writeNops(out + at, dead, 15);
    memcpy(out + at + dead, code + at, n - at);
    return size;
  }
  uint32_t t = read32le(in) & 31, tp = read32le(in + 8) >> 5 & 31;
  if (form == TlsForm::Direct) { // adrp and ldr of the place; add t, tp, t
    write32le(out, 0x90000000 | t);
    applyArm64Addr(out, place, rva, 12);
    write32le(out + 4, 0xf9400000 | t << 5 | t);
    applyArm64Ldr(out + 4, place & 0xfff);
    write32le(out + 8, getTlsAdd(in));
    n = 12;
  } else { // add t, tp, #C; ldr t, [tp, #H]
    write32le(out, form == TlsForm::Place
                       ? 0x91000000 | (c > 0xfff) << 22 |
                             (c > 0xfff ? c >> 12 : c) << 10 | tp << 5 | t
                       : 0xf9400000 | TP_H / 8 << 10 | tp << 5 | t);
    n = 4;
  }
  if (compact)
    return n;
  // A load in reach of a literal is one: the place's, or the kept key's.
  if (form != TlsForm::Place)
    aarch64::applyAdrpLdr(out, rva, kept ? size : 0, kept ? size + 4 : 4);
  for (; n != size; n += 4)
    aarch64::writeNop(out + n);
  return size;
}

// Writes the site of relocation i of sc, r, in its same-size form at off,
// the output of its field, at RVA p.
static void writeSite(const SectionChunk &sc, size_t i, const Relocation &r,
                      uint8_t *off, uint64_t p) {
  ObjFile *file = sc.file;
  COFFLinkerContext &ctx = file->symtab.ctx;
  ArrayRef<uint8_t> c = sc.getContents();
  bool x64 = isX64(sc);
  auto reportRange = [&] {
    Err(ctx) << file << ": "
             << file->getCOFFObj()->getRelocationTypeName(r.type)
             << " relocation against " << toString(ctx, *r.sym)
             << " is out of range";
  };
  auto rva = [&] { return cast<Defined>(r.sym)->getRVA(); };
  unsigned before = x64 ? getBytesBefore(r.type) : 0;
  uint8_t *insn = off - before;
  switch (r.expr) {
  // lea of the target, taking in what the lea of a fused add after it adds.
  case R_LOCAL_LOAD: {
    int64_t v = rva() - p - 4;
    if (i + 1 != sc.getRelocs().size() && (&r)[1].expr == R_DEAD &&
        (&r)[1].type == IMAGE_REL_LLVM_FUSED_ADD)
      v += getFusedAddend(sc.getRelocs()[i + 1]);
    if (!isInt<32>(v))
      return reportRange();
    return x86::relaxGot(off, v, before == 4);
  }
  // A direct branch of the same length: addr32 call, a jump then int3, a
  // call then one no-operation for a load and the call through it, the
  // opposite jcc for a jcc over the jump. An ARM64 bl or b; a cbz or cbnz
  // that names no target keeps its distance.
  case R_LOCAL_CALL:
  case R_LOCAL_JUMP: {
    bool call = r.expr == R_LOCAL_CALL;
    if (!r.sym)
      return write32le(off, 0x14000000 | (SignExtend32<19>(read32le(off) >> 5) &
                                          0x03ffffff));
    if (!x64) {
      write32le(off, call ? 0x94000000 : 0x14000000);
      return applyArm64Branch26(off, rva() - p);
    }
    const coff_relocation &rel = sc.getRelocs()[i];
    unsigned size = before + getBytesFrom(sc, rel);
    int64_t v = rva() - (p - before) - size;
    if (r.type == IMAGE_REL_LLVM_MEM_LOAD_CALL) {
      insn[0] = 0xe8;
      write32le(insn + 1, v + size - 5);
      x86::writeNop(insn + 5, size - 5);
    } else if (call) {
      memcpy(insn, "\x67\xe8", 2);
      write32le(insn + 2, v);
    } else if (r.type == IMAGE_REL_LLVM_JUMP8) {
      if (!isInt<8>(v))
        reportRange();
      insn[0] = 0xeb;
      insn[1] = v;
    } else if (r.type == IMAGE_REL_LLVM_MEM_JUMP_TAIL &&
               (read32(sc, r.offset) & JumpOverJcc)) {
      x86::writeJcc(insn, x86::invertCondition(insn[-2] & 15), true);
      write32le(insn, v + 2);
      x86::writeNop(insn + 4, 2);
    } else {
      x86::relaxGotJmp(insn + size - 4, v, size - 6, 0xcc);
    }
    return;
  }
  case R_TLS:
  case R_TLS_MOVE:
  case R_TLS_TP: {
    if (!x64)
      break;
    SmallString<16> code;
    encodeTlsSite(sc, i, r, p, code);
    memcpy(off, code.data(), code.size());
    return;
  }
  case R_LOCAL_ACCESS: {
    if (!x64)
      break;
    FusedInsn fi;
    SmallString<16> code;
    unsigned field;
    X86Decoder::get()->decodeFused(c.drop_front(r.offset), fi, &code, &field);
    int64_t v = rva() + fi.disp - (p + code.size());
    if (!isInt<32>(v))
      reportRange();
    write32le(code.data() + field, v);
    memcpy(off, code.data(), code.size());
    return;
  }
  case R_DEAD: {
    if (!x64)
      return aarch64::writeNop(off);
    unsigned from = getBytesFrom(sc, sc.getRelocs()[i]), size = before + from;
    // A dead test is one no-operation, and so is the je or jne after it
    // that the assembler resolved where it falls through; else that is jmp.
    if (!isRead(r.type)) {
      x86::writeNop(insn, size);
      if ((r.type != IMAGE_REL_LLVM_FUSED_TEST &&
           r.type != IMAGE_REL_LLVM_FUSED_TEST_REG) ||
          getTestJump(sc, i))
        return;
      uint8_t *jump = off + from;
      bool wide = *jump == 0x0f;
      if (bool(jump[wide] & 1) ==
          isAbsoluteZero(cast<DefinedLocalImport>(r.sym)->getTarget()))
        return x86::writeNop(jump, wide ? 6 : 2);
      if (!wide)
        return void(*jump = 0xeb);
      // jmp rel32 is a byte shorter than jcc rel32; int3 follows it.
      uint32_t v = read32le(jump + 2) + 1;
      jump[0] = 0xe9;
      write32le(jump + 1, v);
      jump[5] = 0xcc;
      return;
    }
    // A dead read becomes segment overrides on the next instruction where
    // the two are at most fifteen bytes, no other dead run abuts it and a
    // transfer of control comes to cross no 32-byte boundary; else one
    // no-operation (section 9.4).
    uint32_t next = read32(sc, r.offset) & ~LoadFlagsDead;
    uint64_t start = p - before;
    bool overrides = next && size + (next >> 1) <= 15 &&
                     !((next & 1) && start >> 5 != (start + size) >> 5);
    const Relocation *rels = &r - i;
    for (size_t j = i + 1, e = sc.getRelocs().size();
         overrides && j != e && rels[j].offset <= r.offset + 8; ++j)
      overrides =
          rels[j].expr == R_DEAD
              ? rels[j].offset - getBytesBefore(rels[j].type) != r.offset + 4
              : rels[j].expr != R_TLS_BASE || rels[j].offset != r.offset + 4;
    for (size_t j = i;
         overrides && j-- && rels[j].offset + 15 + before >= r.offset;)
      overrides = rels[j].expr != R_DEAD ||
                  rels[j].offset + getBytesFrom(sc, sc.getRelocs()[j]) !=
                      r.offset - before;
    if (overrides)
      memset(insn, 0x2e, size);
    else
      x86::writeNop(insn, size);
    return;
  }
  case R_ABS_LOAD:
    x86::relaxMovToImm(off, before == 4);
    return write32le(off, cast<DefinedAbsolute>(r.sym)->getVA());
  case R_ZERO_CALL: // xor r11d, r11d; call r11
    return (void)memcpy(off - 2, "\x45\x31\xdb\x41\xff\xd3", 6);
  // ARM64: where every reference to a local import pointer is a load, the
  // adrp and ldr are adrp and add of its target, or nop and movz of a
  // constant.
  case R_AARCH64_LOCAL: {
    auto *li = cast<DefinedLocalImport>(r.sym);
    Defined *target = li->getTarget();
    bool adrp = r.type == IMAGE_REL_ARM64_PAGEBASE_REL21;
    if (!li->getChunk()->isBypassed())
      sc.applyRelocation(off, sc.getRelocs()[i], p - sc.getRVA(), &r);
    else if (auto *abs = dyn_cast<DefinedAbsolute>(target))
      adrp ? aarch64::writeNop(off)
           : write32le(off,
                       0xd2800000 | abs->getVA() << 5 | (read32le(off) & 31));
    else if (adrp)
      applyArm64Addr(off, target->getRVA(), p, 12);
    else
      write32le(off, 0x91000000 | (read32le(off) & 0x3ff)),
          applyArm64Imm(off, target->getRVA() & 0xfff, 0);
    return;
  }
  case R_PAGEOFF: {
    uint32_t insn = read32le(off);
    if (!addPageOffset(insn, r.type, rva(), ownsPage(&r - i, i)))
      reportRange();
    return write32le(off, insn);
  }
  case R_SITE_PAGE:
    return applyArm64Addr(
        off, rva() + getSiteOffset(read32(sc, r.offset + 8), (&r)[2].type), p,
        12);
  // A literal load of what the access reads, where it has that form in reach.
  case R_LITERAL_ACCESS: {
    aarch64::Ldr l;
    if (!aarch64::parseLdr(read32le(off), l))
      return;
    l.offset += rva() - p;
    if (aarch64::isLiteralLdrEligible(l))
      aarch64::writeLiteralLdr(off, l);
    return;
  }
  case R_IMPORT_SLOT: // the loader finds the import by the entry here
    return write64le(off, cast<DefinedImportData>(r.sym)->getLookupEntry());
  case R_FILL_WORD: // the fill writes it; until then no pointer is there
    return write64le(off, TP_UNFILLED);
  // lea becomes mov from the entry; adrp and add, adrp and ldr.
  case R_IMPORT_LOAD:
    if (x64) {
      off[-2] = 0x8b;
      return write32le(off, rva() - p - 4);
    }
    if (r.type == IMAGE_REL_ARM64_PAGEBASE_REL21)
      return applyArm64Addr(off, rva(), p, 12);
    write32le(off, 0xf9400000 | (read32le(off) & 0x3ff));
    return applyArm64Ldr(off, rva() & 0xfff);
  // A guarded site is a call of its guard's leaf, then padding; an ARM64
  // adrp a b to its guard.
  case R_GUARD_CALL: {
    if (!x64) {
      write32le(off, 0x14000000);
      return applyArm64Branch26(off, rva() - p);
    }
    DelayGuardChunk *g = toDelayGuard(r.sym);
    unsigned before = g->before,
             size = g->insn.empty() ? before + 4 : g->insn.size();
    insn = off - before;
    insn[0] = 0xe8;
    write32le(insn + 1, rva() - (p - before + 5));
    return x86::writeNop(insn + 5, size - 5);
  }
  default:
    break;
  }
  // An ARM64 site of a record or its chain, as encodeTlsSite's for x86-64:
  // a chain's site takes the image's form, or an importer's a place form's
  // with no place; a load of a $tpoff word or of the image's key takes it
  // from the page the adrp before gives; a word's load is adrp of the place
  // word's page or movz of an offset, and an index takes the place word's
  // offset in that page, or the offset as its immediate.
  uint64_t s = rva();
  uint32_t w = read32le(off), d = w & 31, n = w >> 5 & 31, m = w >> 16 & 31;
  bool load = r.type == IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL;
  uint64_t fact = load ? 0 : *file->getSiteFact(&sc, i, SiteFactIndex);
  bool first = fact & SiteFactViaFirst, tpoff = isa<DefinedImportData>(r.sym);
  TlsForm form = tpoff ? TlsForm::Place : getTlsForm(sc);
  // A wide executable's base is the thread pointer plus H, its word's place.
  uint64_t place = tpoff                      ? 0
                   : form == TlsForm::ExeWide ? TP_H
                                              : ctx.tlsPlace.value_or(0);
  if (r.expr == R_TLS_TP) {
    uint32_t tp = *file->getSiteFact(&sc, i, SiteFactThreadPointer);
    return write32le(off, first ? (w & ~(31u << 16)) | tp << 16
                                : (w & ~(31u << 5)) | tp << 5);
  }
  if (r.expr == R_TLS_MOVE) { // mov dest, kept
    auto [dest, kept] = getSelect(sc, i, form);
    return write32le(off, 0xaa0003e0 | kept << 16 | dest);
  }
  switch (load ? ChainSite::None : getChainSite(sc, i)) {
  case ChainSite::Place: // ldr place, [p, :lo12:place]
    write32le(off, 0xf9400000 | n << 5 | d);
    return applyArm64Ldr(off, s & 0xfff);
  case ChainSite::Base: // add t, tp, #C
    return write32le(off, 0x91000000 | (place > 0xfff) << 22 |
                              (place > 0xfff ? place >> 12 : place) << 10 |
                              (first ? m : n) << 5 | d);
  default:
    break;
  }
  if (load &&
      (r.sym == file->symtab.keySym || isa<DefinedLocalImport>(r.sym))) {
    write32le(off, 0xf9400000 | (w & 0x3ff));
    return applyArm64Ldr(off, s & 0xfff);
  }
  if (load ? w >> 30 == 3 : !(fact & 12)) {
    if (load)
      write32le(off, 0x90000000 | (w & 31)), applyArm64Addr(off, s, p, 12);
    else if (addPageOffset(w, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, s, false))
      write32le(off, w);
    else
      reportRange();
    return;
  }
  uint64_t v = getTlsOffset(sc, r.sym);
  if (!load)
    return write32le(
        off, addArm64Offset(w, fact & SiteFactViaFirst, v).value_or(w));
  if (v > 0xffff)
    reportRange();
  write32le(off, 0x52800000 | (v & 0xffff) << 5 | (w & 31));
}

//===----------------------------------------------------------------------===//
// Relaxation: the edits of resizable sections, on the core lld/ELF shares.
//===----------------------------------------------------------------------===//

// Returns sym where it has an address: a value against a discarded symbol is
// reported where it is written.
static Defined *getDefined(Symbol *sym) {
  auto *d = dyn_cast_or_null<Defined>(sym);
  auto *dr = dyn_cast_or_null<DefinedRegular>(d);
  return dr && !dr->getChunk()->live ? nullptr : d;
}

namespace {
// The x86-64 jump of an edit as the object has it. One the scan made direct
// from a pointer or a fused test's je or jne is a jmp with no addend.
struct Jump {
  bool jcc, wasLong;
  uint8_t cc;
  int64_t addend;
};
} // namespace

static Jump getJump(ArrayRef<uint8_t> contents, const Relocation *r,
                    uint32_t offset) {
  bool wasLong = !r || r->type != IMAGE_REL_LLVM_JUMP8;
  if (r && r->expr == R_LOCAL_JUMP)
    return {false, wasLong, 0, 0};
  const uint8_t *field = &contents[offset];
  return {field[-1] != (wasLong ? 0xe9 : 0xeb), wasLong,
          uint8_t(field[-1] & 15),
          wasLong ? int32_t(read32le(field)) : int8_t(*field)};
}
static Jump getJump(const SectionChunk &c, const ScannedSection &s,
                    const Edit &e) {
  ArrayRef<uint8_t> b = c.getContents();
  if (e.fold) // the jcc over a tail jump, folded with it
    return {true, false, uint8_t(b[e.offset - e.before] & 15), 0};
  return getJump(b, e.index == -1u ? nullptr : &s.relocs[e.index], e.offset);
}

// The jump's bytes but those of the jmp a jcc took.
static unsigned getJumpSize(const Edit &e) { return e.size - e.skip; }

// The RVA a jump to sym with the addend goes to; an addend points into a
// function override's body, not its stub.
static int64_t getJumpTarget(Symbol *sym, int64_t addend) {
  auto *d = cast<Defined>(sym);
  if (FunctionOverrideThunk *t =
          addend ? FunctionOverrideThunk::get(d) : nullptr)
    d = t->fo.body;
  return d->getRVA() + addend;
}

// Whether the field of a folded ARM64 pair, adr or a literal load, holds v.
static bool pairFits(bool adr, int64_t v) {
  return adr ? isInt<21>(v) : isShiftedInt<19, 2>(v);
}

// What the one instruction of a folded ARM64 edit reaches, and whether it is
// an adr: the target of a pair, or that of the site after a dead read plus
// the site's own offset.
static std::pair<int64_t, bool>
getFoldTarget(const SectionChunk &c, const ScannedSection &s, const Edit &e) {
  const Relocation &r = s.relocs[e.index];
  if (e.kind == EditKind::Dead) {
    const Relocation &site = (&r)[1];
    return {cast<Defined>(site.sym)->getRVA() +
                getSiteOffset(read32(c, r.offset + 4), site.type),
            site.type == IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL};
  }
  auto *li = dyn_cast<DefinedLocalImport>(r.sym);
  // A local import pointer's ldr that reaches its target folds to the adr of
  // the target, and one that reads the pointer to a literal load of it, as of
  // an import's entry; a pair reaches its symbol plus the adrp's addend. An
  // add that loads the word holding the address instead is a load of it.
  bool local = li && r.expr == R_AARCH64_LOCAL && li->getChunk()->isBypassed();
  return {(local ? li->getTarget() : cast<Defined>(r.sym))->getRVA() +
              getPageOffsetImm(read32(c, r.offset - 4),
                               IMAGE_REL_ARM64_PAGEBASE_REL21),
          local || (r.type == IMAGE_REL_ARM64_PAGEOFFSET_12A &&
                    r.expr != R_IMPORT_LOAD)};
}

// Writes at p the one instruction of a folded ARM64 edit, at RVA rva: adr,
// or a literal load, of what it reaches, into the register its last
// instruction writes.
static void writeFold(const SectionChunk &c, const ScannedSection &s,
                      const Edit &e, uint8_t *p, uint64_t rva) {
  auto [target, adr] = getFoldTarget(c, s, e);
  const Relocation &r = s.relocs[e.index];
  uint32_t insn = read32(c, e.kind == EditKind::Dead ? r.offset + 4 : r.offset);
  int64_t v = target - rva;
  if (!pairFits(adr, v)) {
    errAt(c, r.offset) << "a folded address is out of range";
    v = 0;
  }
  aarch64::Ldr l;
  if (adr)
    return aarch64::writeAdr(p, insn & 31, v);
  if (r.expr == R_IMPORT_LOAD)
    insn = 0xf9400000 | (insn & 0x3ff);
  aarch64::parseLdr(insn, l);
  l.offset = v;
  aarch64::writeLiteralLdr(p, l);
}

// Writes edit e of c at p, at RVA rva: its newSize bytes.
static void writeEdit(const SectionChunk &c, const ScannedSection &s,
                      const Edit &e, uint8_t *p, uint64_t rva) {
  ArrayRef<uint8_t> old = c.getContents();
  const uint8_t *in = &old[e.offset - e.before];
  bool x64 = isX64(c);
  switch (e.kind) {
  case EditKind::Align:
    if (x64)
      return x86::writeNops(
          p, e.newSize, (c.getRelocs()[e.index].SymbolTableIndex >> 5) & 15);
    for (unsigned k = 0; k != e.newSize; k += 4)
      aarch64::writeNop(p + k);
    return;
  // A jcc's opcode is a byte longer where its field has 32 bits; one that
  // took a jmp's target has the opposite condition.
  case EditKind::Jump: {
    if (!e.newSize)
      return;
    Jump j = getJump(c, s, e);
    bool wide = e.newSize > 2;
    uint8_t *field = p + 1 + (j.jcc && wide);
    if (j.jcc)
      x86::writeJcc(field, e.skip || e.fold ? x86::invertCondition(j.cc) : j.cc,
                    wide);
    else
      field[-1] = wide ? 0xe9 : 0xeb;
    uint64_t end = rva + e.newSize;
    int64_t v;
    if (e.index == -1u) // a jump a span states, within its section
      v = getOutputOffset(c, s, e.offset + 4 + j.addend) - (end - c.getRVA());
    else
      v = getJumpTarget(s.relocs[e.index + (e.skip != 0)].sym, j.addend) - end;
    if (!isIntN(wide ? 32 : 8, v)) {
      errAt(c, e.offset) << "a jump is out of range";
      v = 0;
    }
    return wide ? write32le(field, v) : void(*field = v);
  }
  case EditKind::Call:
    p[0] = 0xe8;
    return write32le(p + 1, cast<Defined>(s.relocs[e.index].sym)->getRVA() -
                                (rva + 5));
  case EditKind::Dead:
    if (e.fold)
      writeFold(c, s, e, p, rva);
    return;
  // xor r32, r32, behind the REX or REX2 prefix of a register from r8 on.
  case EditKind::Zero: {
    unsigned reg = getLoadRegister(c, e.offset, e.before == 4);
    if (reg >= 16)
      *p++ = 0xd5, *p++ = 0x50 | (reg & 8 ? 5 : 0);
    else if (reg >= 8)
      *p++ = 0x45;
    *p++ = 0x31;
    *p = 0xc0 | (reg & 7) * 9;
    return;
  }
  case EditKind::Grow:
    return;
  // For each gate, lea r, [thunk]; cmp [gate], r; jne over the call of the
  // ensure stub; then the value's load or lea, each behind the load's own
  // prefix. ARM64: for each gate, adrp and ldr of it into the adrp's
  // register, tbnz of its bit 0 to its block of the guard or tbz over a b
  // to it; then the adrp.
  case EditKind::Guard: {
    if (!x64) {
      auto *g = static_cast<DelayGuardA64Chunk *>(
          cast<DefinedSynthetic>(s.relocs[e.index].sym)->getChunk());
      uint32_t reg = g->reg;
      for (auto [i, gate] : llvm::enumerate(g->gates)) {
        write32le(p, 0x90000000 | reg);
        applyArm64Addr(p, gate.gate->getRVA(), rva, 12);
        write32le(p + 4, 0xf9400000 | reg * 33);
        applyArm64Ldr(p + 4, gate.gate->getRVA() & 0xfff);
        int64_t v =
            g->getRVA() + i * g->getBlock() + 12 - (rva + 8 + 4 * g->far);
        if (g->far) {
          write32le(p + 8, 0x36000000 | 2 << 5 | reg);
          write32le(p + 12, 0x14000000);
          applyArm64Branch26(p + 12, v);
        } else {
          if (!isInt<16>(v))
            errAt(c, e.offset) << "a delay-load guard is out of range";
          write32le(p + 8, 0x37000000 | (v >> 2 & 0x3fff) << 5 | reg);
        }
        p += 12 + 4 * g->far, rva += 12 + 4 * g->far;
      }
      memcpy(p, in, 4);
      return applyArm64Addr(p, g->target->getRVA(), rva, 12);
    }
    DelayGuardChunk *g = toDelayGuard(s.relocs[e.index].sym);
    unsigned n = e.before + 4;
    auto put = [&](uint8_t opcode, Defined *to, int32_t addend = 0) {
      memcpy(p, in, n);
      p[e.before - 2] = opcode;
      write32le(p + e.before, to->getRVA() + addend - (rva + n));
      p += n, rva += n;
    };
    for (const DelayGuardChunk::Gate &gate : g->gates) {
      put(0x8d, gate.thunk);
      put(0x39, gate.gate);
      memcpy(p, "\x75\x05\xe8", 3);
      write32le(p + 3, gate.ensure->getRVA() - (rva + 7));
      p += 7, rva += 7;
    }
    return put(g->address ? 0x8d : 0x8b, g->value, g->addend);
  }
  case EditKind::Scale:
    writeTlsBase(c, in, p, rva, true);
    return;
  // The last instruction, on the base of the first, an add.
  case EditKind::Rebase:
    write32le(p,
              (read32le(in + e.size - 4) & ~0x3e0u) | (read32le(in) & 0x3e0));
    return;
  // A pair folded or as assembled, then a direct base site's add.
  case EditKind::Pair:
    if (e.fold)
      writeFold(c, s, e, p, rva);
    else
      memcpy(p, in, 8);
    if (e.size > 8)
      write32le(p + (e.fold ? 4 : 8), getTlsAdd(in));
    return;
  }
}

// Whether edit e writes its own relocation's field.
static bool writesOwn(const Edit &e) {
  return e.kind == EditKind::Jump || e.kind == EditKind::Call ||
         e.kind == EditKind::Zero || e.kind == EditKind::Guard ||
         (e.kind == EditKind::Pair && e.fold);
}

// Whether the bytes of e stay as the object has them, its relocations
// applied.
static bool isTransparent(const Edit &e) {
  return e.newSize == e.size && e.kind != EditKind::Jump && !writesOwn(e) &&
         e.kind != EditKind::Grow;
}

// The output offset of offset of c, it being the first edit at or after it.
// A point at an ALIGN lies after its padding, unless it ends what precedes
// it; one at the jmp a jcc took lies after the jcc.
static uint32_t mapOffset(const SectionChunk &c, const ScannedSection &s,
                          uint32_t offset, bool end, const Edit *it) {
  ArrayRef<Edit> edits = s.aux->edits;
  int64_t delta = it == edits.begin() ? 0 : it[-1].delta;
  if (it != edits.begin() && it[-1].skip &&
      offset == it[-1].offset + getJumpSize(it[-1]) - it[-1].before)
    delta -= it[-1].skip;
  for (; !end && it != edits.end() && it->offset == offset &&
         it->kind == EditKind::Align;
       ++it)
    delta = it->delta - it->size;
  return offset - delta;
}

static const Edit *findEdit(ArrayRef<Edit> edits, uint32_t offset) {
  return llvm::partition_point(
      edits, [&](const Edit &e) { return e.offset < offset; });
}

uint32_t lld::coff::getOutputOffset(const SectionChunk &sc,
                                    const ScannedSection &s, uint32_t offset,
                                    bool end) {
  return mapOffset(sc, s, offset, end, findEdit(s.aux->edits, offset));
}

uint32_t SectionChunk::getOutputOffset(uint32_t offset, bool end) const {
  ScannedSection *s = resizable ? getScanned() : nullptr;
  if (!s || !s->aux || !s->aux->resized)
    return offset;
  return coff::getOutputOffset(*this, *s, offset, end);
}

ScannedSection *SectionChunk::getScanned() const {
  if (!file)
    return nullptr;
  auto it = file->relocations.find(this);
  return it == file->relocations.end() ? nullptr : &it->second;
}

size_t SectionChunk::getResizedSize() const {
  ScannedSection *s = getScanned();
  return s && s->aux ? s->aux->size : header->SizeOfRawData;
}

uint32_t lld::coff::getInputOffset(const ScannedSection &s, DefinedRegular *d) {
  auto *record = d->getCOFFSymbol().getGeneric();
  uint32_t value = d->getValue();
  ArrayRef<SymbolAnchor> a = s.aux->anchors;
  for (auto it = llvm::partition_point(
           a, [&](const SymbolAnchor &x) { return x.record->Value < value; });
       it != a.end() && it->record->Value == value; ++it)
    if (it->record == record)
      return it->offset;
  // Only a symbol at the section's start has no anchor.
  if (value)
    Fatal(d->file->symtab.ctx)
        << d->file << ": " << toString(d->file->symtab.ctx, *d)
        << " in a resized section has no anchor";
  return 0;
}

// The edit of relocation i, at offset, of a resized section, or null.
static const Edit *getEdit(const ScannedSection *s, uint32_t offset, size_t i) {
  ArrayRef<Edit> edits =
      s && s->aux && s->aux->resized ? s->aux->edits : ArrayRef<Edit>();
  for (const Edit *e = findEdit(edits, offset);
       e != edits.end() && e->offset == offset; ++e)
    if (e->index == i)
      return e;
  return nullptr;
}

uint32_t lld::coff::getRelaxedSite(const SectionChunk &sc, size_t i,
                                   uint16_t &type, Symbol *&sym) {
  const coff_relocation &rel = sc.getRelocs()[i];
  ScannedSection *s = sc.getScanned();
  type = rel.Type;
  sym = sc.getTarget(i, s);
  uint32_t offset = sc.getOutputOffset(rel.VirtualAddress);
  const Edit *e = getEdit(s, rel.VirtualAddress, i);
  if (!e ||
      (e->kind != EditKind::Jump && !(e->kind == EditKind::Pair && e->fold)))
    return offset;
  uint32_t begin = offset - e->before;
  if (e->kind == EditKind::Pair) {
    type = getFoldTarget(sc, *s, *e).second ? IMAGE_REL_ARM64_REL21
                                            : IMAGE_REL_ARM64_BRANCH19;
    return begin;
  }
  Jump j = getJump(sc, *s, *e);
  const Relocation &r = s->relocs[i];
  type = e->newSize == 2                       ? IMAGE_REL_LLVM_JUMP8
         : j.wasLong && r.expr != R_LOCAL_JUMP ? r.type
                                               : IMAGE_REL_LLVM_JUMP32;
  sym = s->relocs[i + (e->skip != 0)].sym;
  return begin + 1 + (j.jcc && e->newSize > 2);
}

uint32_t lld::coff::getFunctionOverrideField(const SectionChunk &sc,
                                             size_t i) {
  const coff_relocation &rel = sc.getRelocs()[i];
  uint16_t type;
  Symbol *sym;
  uint32_t offset = getRelaxedSite(sc, i, type, sym);
  ScannedSection *s = sc.getScanned();
  RelExpr expr = s ? s->relocs[i].expr : R_TYPE;
  // A branch's field is where its relocation is, or an edited jump's where
  // getRelaxedSite puts it, but for an x86-64 call or jump made direct: e8
  // where an edit wrote the call or it took a load's place, else addr32 e8;
  // e9, or the jcc of a jcc over the jump, which then has its opcode before.
  if (!isX64(sc) || type != rel.Type ||
      (expr != R_LOCAL_CALL && expr != R_LOCAL_JUMP))
    return offset;
  unsigned before = getBytesBefore(rel.Type);
  if (expr == R_LOCAL_CALL)
    return getEdit(s, rel.VirtualAddress, i) ||
                   rel.Type == IMAGE_REL_LLVM_MEM_LOAD_CALL
               ? offset - before + 1
               : offset;
  return offset - before + !(read32(sc, rel.VirtualAddress) & JumpOverJcc);
}

// One pass of the shared core over c from its edits' forms. Returns whether
// a size or an offset changed.
static bool relaxChunk(const SectionChunk &c, ScannedSection &s) {
  RelaxAux &aux = *s.aux;
  ArrayRef<coff_relocation> rels = c.getRelocs();
  MutableArrayRef<Edit> edits = aux.edits;
  // Bars c from the places within a line at which a loop the pass leaves at
  // begin would cross one, by its assembled size, behind the least padding of
  // at most most that leaves c a place. Returns the padding.
  unsigned barred = aux.places;
  auto keep = [&](uint64_t begin, unsigned size, unsigned most) {
    for (unsigned pad = aux.fixed ? most : 0;; ++pad) {
      unsigned places = barred;
      for (uint64_t i = 0, at = begin + pad; i != 4; ++i, at += 16)
        places |= (at / 64 != (at + size - 1) / 64) << i;
      if (places != 15 || pad == most)
        return barred = places, pad;
    }
  };
  // The listed loops that begin before offset, delta bytes having gone
  // before them, take no padding.
  const uint8_t *loop = aux.loops.bytes_begin();
  uint32_t begin = 0;
  auto keepLoops = [&](uint32_t offset, int64_t delta) {
    for (unsigned n; loop != aux.loops.bytes_end(); loop += n) {
      uint64_t head = decodeULEB128(loop, &n);
      if (begin + (head >> 5) >= offset)
        break;
      begin += head >> 5;
      keep(begin - delta, (head & 31) + 1, 0);
    }
  };
  auto remove = [&](size_t i, Edit &e, int64_t delta) -> int64_t {
    keepLoops(e.offset, delta);
    if (e.kind != EditKind::Align)
      return aux.fixed ? 0 : e.size - e.newSize;
    uint32_t index = rels[e.index].SymbolTableIndex;
    if (index >> 27 & 1) {
      e.newSize = keep(e.offset - delta, e.size + 1, e.size);
    } else if (aux.fixed) {
      e.newSize = e.size;
    } else {
      // In boundary mode the span follows the jumps in it whose width
      // changed.
      uint32_t end =
          index >> 21 & 1 ? e.offset + e.size + ((index >> 22) & 31) : 0;
      int shrink = 0;
      for (size_t k = i + 1; k != edits.size() && edits[k].offset < end; ++k)
        if (edits[k].kind == EditKind::Jump && edits[k].newSize)
          shrink += getJumpSize(edits[k]) - edits[k].newSize;
      e.newSize = getAlignPadding(index, e.offset - delta, shrink);
    }
    return e.size - e.newSize;
  };
  int64_t delta;
  bool changed = relaxSection(aux, edits, delta, remove);
  keepLoops(UINT32_MAX, delta);
  c.linePlacesBarred = barred == 15 ? 0 : barred;
  aux.size = c.header->SizeOfRawData - delta;
  aux.resized = llvm::any_of(edits, [](const Edit &e) { return e.delta; });
  // A symbol at an ALIGN lies after its padding, and one at the jmp a jcc
  // took after the jcc.
  for (const Edit &e : aux.resized ? edits : MutableArrayRef<Edit>()) {
    uint32_t at = e.offset, gone = e.size;
    if (e.skip)
      at += getJumpSize(e) - e.before, gone = e.skip;
    else if (e.kind != EditKind::Align)
      continue;
    for (auto it = llvm::partition_point(
             aux.anchors, [&](const SymbolAnchor &x) { return x.offset < at; });
         it != aux.anchors.end() && it->offset == at; ++it)
      it->apply(at + gone - e.delta);
  }
  return changed;
}

namespace {
// The symbols of one object that control may reach or whose address
// something may hold: the external ones, and those a relocation outside
// debug information, a guard table or a switch table names. A label only
// debug information names marks a place nothing arrives at, so code is the
// same with debug information and without.
struct ReachedSymbols {
  explicit ReachedSymbols(ObjFile *f) : f(f) {}
  ObjFile *f;
  DenseSet<const coff_symbol_generic *> records;
  bool built = false;

  bool contains(const coff_symbol_generic *record) {
    if (!std::exchange(built, true)) {
      ArrayRef<Symbol *> syms = f->getSymbols();
      auto add = [&](Symbol *sym) {
        if (auto *d = dyn_cast_or_null<DefinedRegular>(sym))
          records.insert(d->getCOFFSymbol().getGeneric());
      };
      auto addIndex = [&](uint32_t i) { add(i < syms.size() ? syms[i] : 0); };
      for (Symbol *sym : syms)
        if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
            d && d->getCOFFSymbol().isExternal())
          add(d);
      for (Chunk *chunk : f->getChunks())
        if (auto *sc = dyn_cast<SectionChunk>(chunk);
            sc && !sc->getSectionName().starts_with(".debug"))
          for (const coff_relocation &rel : sc->getRelocs())
            if (!relocationNamesNoSymbol(rel.Type))
              addIndex(rel.SymbolTableIndex);
      for (ArrayRef<SectionChunk *> tables :
           {f->getGuardFidChunks(), f->getGuardIATChunks(),
            f->getGuardLJmpChunks(), f->getGuardEHContChunks()})
        for (SectionChunk *sc : tables)
          for (size_t i = 0, e = sc->getContents().size() / 4; i != e; ++i)
            addIndex(read32le(sc->getContents().data() + i * 4));
      for (const SwitchTable &t : f->switchTables)
        for (DefinedRegular *d : {t.branch, t.table, t.base, t.load})
          add(d);
    }
    return records.contains(record);
  }
};
} // namespace

static Edit makeEdit(uint32_t offset, uint32_t index, EditKind kind,
                     unsigned before, unsigned size, unsigned newSize) {
  Edit e;
  e.offset = offset, e.index = index, e.kind = kind, e.before = before;
  e.size = size, e.newSize = newSize;
  return e;
}

// Whether r is an x86-64 jump to its symbol that a span may state for the
// link to narrow: a direct one, or one through a pointer made direct.
static bool isFarJump(const Relocation &r) {
  return r.expr == R_TYPE ? r.type == IMAGE_REL_LLVM_JUMP32 ||
                                r.type == IMAGE_REL_LLVM_BRANCH_TAIL
                          : r.expr == R_LOCAL_JUMP && isMemJump(r.type);
}

namespace {
// Builds the edits of a resizable section, each at its smallest: a jump
// short, an ARM64 pair one instruction, a dead instruction gone. A section
// whose code stays has only its end jump, and where it states its spans its
// padding, the bytes a direct call frees and the jumps its spans state.
struct EditBuilder {
  EditBuilder(COFFLinkerContext &ctx, const SectionChunk &c, ScannedSection &s,
              ReachedSymbols &reached)
      : ctx(ctx), c(c), s(s), reached(reached) {}
  COFFLinkerContext &ctx;
  const SectionChunk &c;
  ScannedSection &s;
  ReachedSymbols &reached;
  RelaxAux &aux = *s.aux;
  bool x64 = isX64(c);
  ArrayRef<uint8_t> contents = c.getContents();
  ArrayRef<coff_relocation> rels = c.getRelocs();
  MutableArrayRef<Relocation> relocs = s.relocs;
  // The bytes each rewrite may change.
  SmallVector<std::pair<uint32_t, uint32_t>, 0> extents;

  Edit &add(size_t i, EditKind kind, unsigned before, unsigned size,
            unsigned newSize, bool extent = true) {
    uint32_t begin = relocs[i].offset - before;
    // An ARM64 pair is listed by its words, a direct base site's by its site.
    assert((!c.movable ||
            relocationMayResizeCode(
                c.getMachine(), relocs[i].type,
                c.getRelocTarget(rels[i]) ? c.getRelocTarget(rels[i])->getName()
                                          : "",
                begin + 8 > contents.size() ? 0 : read64le(&contents[begin])) ||
            (kind == EditKind::Pair && size > 8)) &&
           "the link resizes code at a relocation the assembler did not keep");
    if (extent && c.movable)
      extents.emplace_back(begin, begin + size);
    aux.edits.push_back(
        makeEdit(relocs[i].offset, i, kind, before, size, newSize));
    return aux.edits.back();
  }
  // Padding as the object has it, which the passes redo.
  void addAlign(size_t i) {
    uint32_t pad = getAlignPadding(rels[i].SymbolTableIndex, relocs[i].offset);
    add(i, EditKind::Align, 0, pad, pad);
  }
  // Whether a symbol, or with reachedOnly one control may reach, lies in
  // [begin, end).
  bool anchored(uint32_t begin, uint32_t end, bool reachedOnly = false) {
    for (auto it = llvm::partition_point(
             aux.anchors,
             [&](const SymbolAnchor &x) { return x.offset < begin; });
         it != aux.anchors.end() && it->offset < end; ++it)
      if (!reachedOnly || reached.contains(it->record))
        return true;
    return false;
  }
  // A tail jump made direct that a jcc jumps over: the two are one jcc of
  // the opposite condition.
  bool foldsJcc(const Relocation &r) {
    return r.type == IMAGE_REL_LLVM_MEM_JUMP_TAIL && r.expr == R_LOCAL_JUMP &&
           (read32(c, r.offset) & JumpOverJcc);
  }
  bool isJump(const Relocation &r) {
    return r.expr == R_JUMP ||
           (x64 && r.type == IMAGE_REL_LLVM_BRANCH_TAIL && r.expr == R_TYPE &&
            r.offset && r.offset + 4 <= contents.size() &&
            contents[r.offset - 1] == 0xe9);
  }

  // ARM64: the pointers each of whose adds and accesses directly follows a
  // dead read of its own, the read's adrp directly before that and nothing
  // arriving at the three, so that each read's register is the one site's and
  // its adrp gives the site its page (section 9.3).
  SmallDenseSet<uint32_t, 4> getOwnPages() {
    struct Count {
      unsigned reads = 0, sites = 0;
      bool apart = false;
    };
    SmallDenseMap<uint32_t, Count, 4> counts;
    for (size_t i = 0, n = x64 ? 0 : relocs.size(); i != n; ++i) {
      const Relocation &r = relocs[i];
      if (r.type != IMAGE_REL_LLVM_FUSED_READ && r.expr != R_PAGEOFF)
        continue;
      Count &k = counts[rels[i].SymbolTableIndex];
      if (r.expr != R_PAGEOFF) {
        ++k.reads;
        continue;
      }
      ++k.sites;
      uint32_t o = r.offset, insn = read32le(&contents[o]);
      k.apart |= i < 2 || relocs[i - 1].expr != R_DEAD ||
                 relocs[i - 1].type != IMAGE_REL_LLVM_FUSED_READ ||
                 relocs[i - 1].offset + 4 != o ||
                 rels[i - 1].SymbolTableIndex != rels[i].SymbolTableIndex ||
                 relocs[i - 2].type != IMAGE_REL_ARM64_PAGEBASE_REL21 ||
                 relocs[i - 2].expr != R_TYPE || relocs[i - 2].sym != r.sym ||
                 relocs[i - 2].offset + 8 != o ||
                 (insn & 0x3b800000) == 0x29000000 ||
                 ((insn >> 5) & 31) != (read32le(&contents[o - 4]) & 31) ||
                 anchored(o - 7, o + 1, true);
    }
    SmallDenseSet<uint32_t, 4> own;
    for (auto &[index, k] : counts)
      if (!k.apart && k.reads == k.sites)
        own.insert(index);
    return own;
  }

  // A dead ARM64 read with its site after it, the pointer's every site so,
  // starts folded with the site and its adrp into one adr, or one literal
  // load where the site is a load of that form.
  void addDeadRead(size_t i, const SmallDenseSet<uint32_t, 4> &own) {
    uint32_t o = relocs[i].offset;
    if (i + 1 == relocs.size() || relocs[i + 1].expr != R_PAGEOFF ||
        relocs[i + 1].offset != o + 4 ||
        !own.contains(rels[i].SymbolTableIndex)) {
      add(i, EditKind::Dead, 0, 4, 0);
      return;
    }
    aarch64::Ldr l;
    bool fold =
        relocs[i + 1].type == IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL ||
        (aarch64::parseLdr(read32le(&contents[o + 4]), l) && l.p2Size > 1 &&
         (l.extendType == aarch64::ZeroExtend ||
          (l.extendType == aarch64::Sign64 && l.p2Size == 2)));
    relocs[i - 1].expr = R_SITE_PAGE;
    if (fold)
      add(i, EditKind::Dead, 4, 12, 4).fold = true;
    else
      add(i, EditKind::Dead, 0, 4, 0);
    relocs[i - 1].gone = relocs[i + 1].gone = fold;
  }

  // A jump starts short, unless a symbol lies inside its displacement or it
  // is a function override's site of 32 bits. A jcc over a jmp, both to this
  // section, neither to a function override and nothing else reaching the
  // jmp, is the opposite jcc to the jmp's target, so the jmp ends no
  // function.
  void addJump(size_t i) {
    Relocation &r = relocs[i];
    uint32_t o = r.offset;
    Jump j = getJump(contents, &r, o);
    uint32_t end = o + (j.wasLong ? 4 : 1), skip = 0;
    Relocation *m = i + 1 != relocs.size() ? &relocs[i + 1] : nullptr;
    // The jcc over a tail jump the jump's edit folds is the edit's.
    if (j.jcc && !j.wasLong && m && m->offset == o + 3 && foldsJcc(*m))
      r.gone = true;
    if (r.gone || !getDefined(r.sym) || anchored(o + 1, end))
      return;
    auto *over = dyn_cast<DefinedRegular>(r.sym);
    bool site = FunctionOverride::get(over);
    if (j.jcc && !j.addend && m && isJump(*m) && m->offset == end + 1 && over &&
        over->getChunk() == &c && !site) {
      Jump mj = getJump(contents, m, m->offset);
      uint32_t mEnd = m->offset + (mj.wasLong ? 4 : 1);
      auto *to = dyn_cast<DefinedRegular>(m->sym);
      if (!mj.jcc && !mj.addend && over->getValue() == mEnd && to &&
          to->getChunk() == &c && !FunctionOverride::get(to) &&
          !anchored(end, mEnd, true))
        skip = mEnd - end, m->gone = true;
    }
    unsigned before = 1 + (j.jcc && j.wasLong), size = before + end - o;
    add(i, EditKind::Jump, before, size + skip, site && j.wasLong ? size : 2)
        .skip = skip;
  }

  // ARM64: an adrp and the add or ldr after it of one symbol, a pair that
  // relocationMayResizeCode lists, start as one adr or literal load; a local
  // import pointer's ldr as the adr of its target, its adrp dead where that
  // is a constant. The place's ldr of a direct base site takes its add, and
  // the rest of the site goes.
  void addPair(size_t i) {
    Relocation &r = relocs[i];
    uint32_t o = r.offset;
    bool load = r.type == IMAGE_REL_ARM64_PAGEOFFSET_12L ||
                r.type == IMAGE_REL_LLVM_FUSED_READ;
    if (!i || o + 4 > contents.size() ||
        (r.type != IMAGE_REL_ARM64_PAGEOFFSET_12A && !load))
      return;
    auto *li = dyn_cast_or_null<DefinedLocalImport>(r.sym);
    bool local = r.expr == R_AARCH64_LOCAL && li->getChunk()->isBypassed();
    Defined *to = local ? li->getTarget() : getDefined(r.sym);
    Relocation *a = &relocs[i - 1], *site = nullptr;
    if (a->expr == R_TLS_BASE && a != relocs.begin())
      site = a--;
    else if (a != relocs.begin() && a[-1].expr == R_TLS_BASE)
      site = a - 1;
    bool dead =
        site && site->offset + 4 == o && getTlsForm(c) == TlsForm::Direct;
    bool fold = a->type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
                a->expr == r.expr && a->offset + 4 == o && a->sym == r.sym &&
                to && !c.getPair(rels[i]) && !anchored(o - 3, o + 1, true) &&
                relocationMayResizeCode(c.getMachine(),
                                        load ? IMAGE_REL_ARM64_PAGEOFFSET_12L
                                             : IMAGE_REL_ARM64_PAGEOFFSET_12A,
                                        "", read64le(&contents[o - 4]));
    if (fold && isa<DefinedAbsolute>(to)) {
      fold = false;
      if (local)
        add(i - 1, EditKind::Dead, 0, 4, 0), a->gone = true;
    }
    if (fold || dead)
      add(i, EditKind::Pair, 4, dead ? 24 : 8, (fold ? 4 : 8) + (dead ? 4 : 0),
          !site)
          .fold = a->gone = fold;
  }

  // ARM64: the add of a thread-local's high part, or of a low part that has
  // none, zero, goes where it is the pair relocationMayResizeCode lists,
  // nothing arrives at the instruction after it and only that instruction
  // reads what it wrote, by fact 15 or by writing its register as ELF proves
  // a pair: that instruction takes the add's base. Where a high part's low
  // part is zero too, in the add after it, and only an access after that
  // reads the add, both go and the access takes the base. A store of the
  // register, or an index, keeps the adds.
  void addTlsPart(size_t i) {
    Relocation &r = relocs[i];
    uint32_t o = r.offset;
    auto *d = dyn_cast_or_null<Defined>(r.sym);
    bool lone = r.type == IMAGE_REL_LLVM_DTPOFF_LOW12A ||
                r.type == IMAGE_REL_LLVM_TPOFF_LOW12A;
    if ((!lone && r.type != IMAGE_REL_ARM64_SECREL_HIGH12A &&
         r.type != IMAGE_REL_LLVM_TPOFF_HIGH12A) ||
        !d || o + 8 > contents.size() ||
        !relocationMayResizeCode(c.getMachine(), r.type, "",
                                 read64le(&contents[o])))
      return;
    // The offset from the image's base, or from the thread pointer.
    std::optional<uint64_t> v = ctx.tlsOffset(d->getRVA());
    if (r.type == IMAGE_REL_LLVM_TPOFF_HIGH12A ||
        r.type == IMAGE_REL_LLVM_TPOFF_LOW12A)
      v = v && ctx.tlsPlace ? std::optional(*v + *ctx.tlsPlace) : std::nullopt;
    uint32_t hi = read32le(&contents[o]), addend = hi >> 10 & 0xfff;
    // Whether the instruction at, reading x as its base, is the only reader
    // of what relocation k's add wrote into x.
    auto onlyReads = [&](uint32_t at, uint32_t x, size_t k) {
      uint32_t insn = read32le(&contents[at]), opc = insn >> 22 & 3;
      bool add = (insn & 0xff800000) == 0x91000000;
      bool gpr = !add && !(insn & 0x4000000);
      bool load = gpr && opc && !(insn >> 30 == 3 && opc == 2);
      bool index = (insn & 0x3b200c00) == 0x38200800;
      return (insn >> 5 & 31) == x && !anchored(at - 3, at + 1, true) &&
             !(gpr && !opc && (insn & 31) == x) &&
             !(index && (insn >> 16 & 31) == x) &&
             (((add || load) && (insn & 31) == x) ||
              c.file->getSiteFact(&c, k, SiteFactOnlyNextReads));
    };
    if (!v || *v + addend > (lone ? 0 : 0xfff) ||
        !onlyReads(o + 4, hi & 31, i))
      return;
    uint32_t low = read32le(&contents[o + 4]), at = o + 8;
    uint32_t access = at + 4 <= contents.size() ? read32le(&contents[at]) : 0;
    bool both = !*v && !addend && !lone &&
                (low & 0xff800000) == 0x91000000 &&
                i + 1 < relocs.size() && relocs[i + 1].offset == o + 4 &&
                ((access & 0x3b000000) == 0x39000000 ||
                 (access & 0x3b200c00) == 0x38200800) &&
                onlyReads(at, low & 31, i + 1);
    r.gone = true;
    if (both)
      relocs[i + 1].gone = true;
    add(i, EditKind::Rebase, 0, both ? 12 : 8, 4);
  }

  // An x86-64 jump a span states for the link to narrow: one in the section,
  // starting short, and one to another chunk where the chunks' order may put
  // its target in reach or one through a pointer made direct, which loses a
  // byte.
  void addSpanJump(uint32_t offset) {
    Relocation *r = llvm::partition_point(
        relocs, [&](const Relocation &r) { return r.offset < offset; });
    bool far = r != relocs.end() && r->offset == offset;
    size_t i = r - relocs.begin();
    int64_t to = int64_t(offset) + 4 + int32_t(read32le(&contents[offset]));
    bool jcc = offset > 1 && contents[offset - 2] == 0x0f &&
               (contents[offset - 1] & 0xf0) == 0x80;
    if (!offset || (!far && (to < 0 || to > int64_t(contents.size()))) ||
        (!far || r->type == IMAGE_REL_LLVM_JUMP32 ||
                 r->type == IMAGE_REL_LLVM_BRANCH_TAIL
             ? contents[offset - 1] != 0xe9 && !jcc
             : !isMemJump(r->type))) {
      errAt(c, offset) << "a span states no jump within the section";
      return;
    }
    if (!far) {
      aux.edits.push_back(
          makeEdit(offset, -1u, EditKind::Jump, 1 + jcc, 5 + jcc, 2));
      return;
    }
    auto it =
        llvm::lower_bound(aux.betweens, std::pair<uint32_t, int8_t>(i, -128));
    int8_t between =
        it != aux.betweens.end() && it->first == i ? it->second : -128;
    if (i == aux.tail || !isFarJump(*r) ||
        (r->expr != R_LOCAL_JUMP && between == -128))
      return;
    bool fold = foldsJcc(*r);
    unsigned before =
        r->expr == R_LOCAL_JUMP ? getBytesBefore(r->type) + 2 * fold : 1 + jcc;
    Edit &e = add(i, EditKind::Jump, before, before + 4,
                  between != -128 ? 2 : 5 + fold, false);
    e.between = between, e.fold = fold;
  }

  void build() {
    if (c.movable) {
      buildMoving();
      addTail();
      return check();
    }
    addTail();
    if (c.compacts)
      buildSpans();
    llvm::stable_sort(aux.edits, [](const Edit &a, const Edit &b) {
      return a.offset < b.offset;
    });
  }

  // The end jump the link deletes goes whole.
  void addTail() {
    if (aux.tail == -1u)
      return;
    unsigned before = x64 ? getBytesBefore(relocs[aux.tail].type) : 0;
    add(aux.tail, EditKind::Jump, before, before + 4, 0);
  }

  // Padding, and the bytes a call made direct frees in a section that states
  // its spans.
  void buildSpans() {
    for (size_t i = 0, n = relocs.size(); i != n; ++i) {
      const Relocation &r = relocs[i];
      if (r.expr == R_ALIGN) {
        addAlign(i);
      } else if (r.expr == R_LOCAL_CALL &&
                 r.type == IMAGE_REL_LLVM_MEM_LOAD_CALL && i != aux.tail) {
        add(i, EditKind::Call, 3, 3 + getBytesFrom(c, rels[i]), 5, false);
      }
    }
    for (const SpanList &l : c.file->getSpans(&c)) {
      uint32_t offset = 0;
      for (const uint8_t *p = l.spans.bytes_begin(), *e = l.spans.bytes_end();
           l.target == &c && l.jumps && p != e;) {
        unsigned n;
        uint64_t head = decodeULEB128(p, &n);
        p += n;
        offset += head >> 4;
        if ((head & 15) >= SpanData)
          decodeULEB128(p, &n), p += n;
        if ((head & 15) == SpanJump32)
          addSpanJump(offset);
      }
    }
    aux.betweens.clear();
    // Padding stands where nothing before it goes, but a loop's in line
    // mode, which the object has at its most.
    if (llvm::all_of(aux.edits, [&](const Edit &e) {
          return e.kind == EditKind::Align &&
                 !(rels[e.index].SymbolTableIndex >> 27 & 1);
        }))
      aux.edits.clear();
  }

  void buildMoving() {
    SmallDenseSet<uint32_t, 4> own = getOwnPages();
    for (size_t i = 0, n = relocs.size(); i != n; ++i) {
      Relocation &r = relocs[i];
      uint32_t o = r.offset;
      unsigned before = x64 ? getBytesBefore(r.type) : 0;
      if (r.gone)
        continue;
      switch (r.expr) {
      case R_ALIGN:
        addAlign(i);
        continue;
      // An x86-64 call made direct is five bytes and a jump starts short; an
      // ARM64 one keeps its place. A dead instruction goes whole.
      case R_LOCAL_CALL:
      case R_LOCAL_JUMP:
      case R_DEAD: {
        unsigned size = x64 ? before + getBytesFrom(c, rels[i]) : 4;
        // A function override's site keeps its 32 bits.
        bool site = FunctionOverride::get(r.sym);
        if (Insn insn; x64 && (r.type == IMAGE_REL_LLVM_FUSED_ACCESS ||
                               r.type == IMAGE_REL_LLVM_FUSED_INDEX)) {
          X86Decoder::get()->decode(contents.drop_front(o), 0, insn);
          size = insn.size;
        }
        if (!x64 && (r.expr != R_DEAD || !deletesDead(c, i)))
          extents.emplace_back(o, o + 4);
        else if (i && relocs[i - 1].gone && relocs[i - 1].offset + 3 == o &&
                 foldsJcc(r))
          add(i, EditKind::Jump, before + 2, size + 2, site ? 6 : 2).fold = true;
        else if (r.expr != R_DEAD)
          add(i, r.expr == R_LOCAL_CALL ? EditKind::Call : EditKind::Jump,
              before, size, r.expr == R_LOCAL_CALL || site ? 5 : 2);
        else if (!x64 && r.type == IMAGE_REL_LLVM_FUSED_READ)
          addDeadRead(i, own);
        else
          add(i, EditKind::Dead, before, size, 0);
        continue;
      }
      // A guard goes before its load or adrp.
      case R_GUARD:
        if (x64)
          add(i, EditKind::Guard, before, before + 4,
              toDelayGuard(r.sym)->gates.size() * (2 * (before + 4) + 7) +
                  before + 4);
        else if (auto *g = static_cast<DelayGuardA64Chunk *>(
                     cast<DefinedSynthetic>(r.sym)->getChunk()))
          add(i, EditKind::Guard, 0, 4,
              g->gates.size() * (12 + 4 * g->far) + 4);
        continue;
      // A load of a zero whose flags are dead is xor of its register.
      case R_ABS_LOAD:
        if (x64 && isLoad(r.type) && !cast<DefinedAbsolute>(r.sym)->getVA() &&
            (read32le(&contents[o]) & LoadFlagsDead))
          add(i, EditKind::Zero, before, before + 4,
              2 + (getLoadRegister(c, o, before == 4) >= 8) +
                  (getLoadRegister(c, o, before == 4) >= 16));
        continue;
      // An access relative to rip has a displacement of 32 bits and no SIB
      // byte; a site taking a record's word has its own length.
      case R_LOCAL_ACCESS:
      case R_TLS:
      case R_TLS_MOVE:
      case R_TLS_TP: {
        FusedInsn insn;
        SmallString<16> direct;
        unsigned field;
        if (!x64)
          continue;
        if (r.expr != R_LOCAL_ACCESS)
          insn.size = encodeTlsSite(c, i, r, 0, direct);
        else
          X86Decoder::get()->decodeFused(contents.drop_front(o), insn, &direct,
                                         &field);
        if (insn.size != direct.size())
          add(i, EditKind::Grow, 0, insn.size, direct.size());
        else
          extents.emplace_back(o, o + insn.size);
        continue;
      }
      // A base site's form replaces it up to what it keeps and writes its own
      // fields, but an ARM64 direct site's place, which its pair writes; a
      // wide DLL's site stays, and its pairs may fold.
      case R_TLS_BASE: {
        TlsForm form = getTlsForm(c);
        if (form == TlsForm::Wide)
          continue;
        unsigned size = getTlsSiteSize(c, &contents[o]);
        uint32_t end = o + size - getTlsKept(c, form);
        bool pair = !x64 && form == TlsForm::Direct;
        // The site's own fields: the x86-64 place's and key's, the ARM64
        // adrp and ldr of each.
        for (size_t k = i; !pair && k-- && relocs[k].offset == o;)
          relocs[k].gone = true;
        for (size_t k = i + 1; k != n && relocs[k].offset < end; ++k)
          if (uint32_t at = relocs[k].offset - o;
              x64 ? at == 3 || at == size - 4
                  : at % 12 < 8 && (!pair || at > 8))
            relocs[k].gone = true;
        uint8_t buf[24];
        if (!pair)
          add(i, EditKind::Scale, 0, end - o,
              writeTlsBase(c, &contents[o], buf, 0, true));
        continue;
      }
      case R_JUMP:
      case R_TYPE:
      case R_AARCH64_LOCAL:
      case R_IMPORT_LOAD:
        if (isJump(r))
          addJump(i);
        else if (!x64)
          addPair(i), addTlsPart(i);
        continue;
      default:
        continue;
      }
    }
  }

  // Rewrites do not overlap, and none lies in a function's type prefix.
  void check() {
    llvm::sort(extents);
    for (size_t i = 1; i < extents.size(); ++i)
      if (extents[i].first < extents[i - 1].second) {
        Err(ctx) << c.file << ": " << c.getSectionName()
                 << ": the rewrites at 0x"
                 << Twine::utohexstr(extents[i - 1].first) << " and 0x"
                 << Twine::utohexstr(extents[i].first) << " overlap";
        break;
      }
    for (auto [pc, p] : c.file->typePrefixes) {
      uint32_t prefix = p;
      auto next =
          llvm::partition_point(aux.anchors, [&](const SymbolAnchor &x) {
            return x.offset <= prefix;
          });
      uint32_t end = next == aux.anchors.end() ? contents.size() : next->offset;
      for (auto [b, e] : pc == &c ? ArrayRef(extents)
                                  : ArrayRef<std::pair<uint32_t, uint32_t>>())
        if (b < end && e > prefix)
          errAt(c, b) << "a rewrite in a function's type prefix";
    }
  }
};
} // namespace

// Folds ARM64 edit e of s, a dead read with its site and the adrp before it
// or a pair, into one instruction, or unfolds it: the dead read is by itself
// again, the adrp giving the page of what the site reaches, and a pair's adrp
// writes again.
static void setFold(ScannedSection &s, Edit &e, bool fold) {
  e.fold = fold, e.unfolded = !fold;
  Relocation *r = &s.relocs[e.index];
  if (e.kind == EditKind::Dead) {
    e.before = 4 * fold, e.size = fold ? 12 : 4, e.newSize = 4 * fold;
    r[-1].gone = r[1].gone = fold;
    return;
  }
  e.newSize = (fold ? 4 : 8) + (e.size > 8 ? 4 : 0);
  (r[-1].expr == R_TLS_BASE ? r[-2] : r[-1]).gone = fold;
}

// Lengthens each short jump of c whose target is out of its reach and
// unfolds each ARM64 fold whose target is, or with refold folds again each
// one that a layout unfolded whose target it reaches. A jump a span states
// reaches by its own section's layout or by the most bytes the chunks' order
// puts between it and its target, so its width depends on no address; any
// other reaches in the last layout. Returns whether it did any.
static bool lengthen(const SectionChunk &c, ScannedSection &s, bool refold) {
  RelaxAux &aux = *s.aux;
  bool changed = false;
  for (size_t i = 0, n = aux.edits.size(); i != n && !aux.fixed; ++i) {
    Edit &e = aux.edits[i];
    int64_t begin =
        c.getRVA() + e.offset - e.before - (i ? aux.edits[i - 1].delta : 0);
    if (refold) {
      if (!e.unfolded)
        continue;
      // A dead read by itself follows its adrp, where the fold goes.
      auto [target, adr] = getFoldTarget(c, s, e);
      if (!pairFits(adr, target - begin + 4 * (e.kind == EditKind::Dead)))
        continue;
      setFold(s, e, true);
    } else if (e.kind == EditKind::Jump && e.newSize == 2) {
      Jump j = getJump(c, s, e);
      int64_t end = getOutputOffset(c, s, e.offset + 4, true);
      if (e.index == -1u
              ? isInt<8>(getOutputOffset(c, s, e.offset + 4 + j.addend) - end)
          : e.between >= 0 ? aux.size - end + e.between <= 127
          : e.between != -128
              ? end + ~e.between <= 128
              : isInt<8>(getJumpTarget(s.relocs[e.index + (e.skip != 0)].sym,
                                       j.addend) -
                         (begin + 2)))
        continue;
      e.newSize = j.jcc ? 6 : 5;
    } else if (e.fold && e.kind != EditKind::Jump) {
      if (auto [target, adr] = getFoldTarget(c, s, e);
          pairFits(adr, target - begin))
        continue;
      setFold(s, e, false);
    } else if (e.kind == EditKind::Guard && !isX64(c)) {
      // An inline guard's tbnz reaches its block, the last the farthest, or
      // each gate takes a tbz over a b.
      auto *g = static_cast<DelayGuardA64Chunk *>(
          cast<DefinedSynthetic>(s.relocs[e.index].sym)->getChunk());
      size_t last = g->gates.size() - 1;
      if (g->far ||
          isInt<16>(g->getRVA() + last * (g->getBlock() - 12) + 4 - begin))
        continue;
      g->far = true;
      e.newSize += 4 * g->gates.size();
    } else {
      continue;
    }
    changed = true;
  }
  return changed;
}

// Gives a resizable section its edits and its smallest size, from what its
// own object states.
static void decide(COFFLinkerContext &ctx, const SectionChunk &c,
                   ScannedSection &s, ReachedSymbols &reached) {
  llvm::sort(s.aux->anchors, [](const SymbolAnchor &a, const SymbolAnchor &b) {
    return a.offset < b.offset;
  });
  EditBuilder(ctx, c, s, reached).build();
  if (!c.movable && s.aux->edits.empty() && s.aux->loops.empty())
    return s.aux.reset();
  for (relaxChunk(c, s); c.compacts && lengthen(c, s, false);)
    relaxChunk(c, s);
  // A section that only has loops to place keeps its bytes.
  if (!c.movable && s.aux->edits.empty())
    s.aux.reset();
}

// The function's code of which c is a part keeps its size, and its sites take
// their same-size forms. An x86-64 access goes through its register again,
// which the fused reads of its pointer load with the target's address, and so
// do sites that took a record's word, the image holding the record after all.
// A jump of eight bits goes where it was assembled to go, and a guarded load
// calls its guard's leaf.
static void keepSize(const SectionChunk &c) {
  SmallDenseMap<uint32_t, Defined *, 4> loads;
  forEachPart(c, [&](const SectionChunk &p, ScannedSection &s) {
    if (!s.aux)
      return;
    RelaxAux &aux = *s.aux;
    aux.fixed = true;
    aux.resized = aux.fallsThrough = false;
    aux.size = p.header->SizeOfRawData;
    aux.tail = -1;
    ArrayRef<coff_relocation> rels = p.getRelocs();
    for (size_t i = 0, e = rels.size(); i != e; ++i) {
      Relocation &r = s.relocs[i];
      r.gone = false;
      if (r.type == IMAGE_REL_LLVM_JUMP8)
        r.sym = p.getRelocTarget(rels[i]);
      if (r.expr == R_GUARD && (r.expr = R_GUARD_CALL, isX64(p)))
        toDelayGuard(r.sym)->called = true;
      if (!isX64(p))
        continue;
      if ((r.expr < R_LOCAL_ACCESS || r.expr > R_TLS_TP) &&
          ((r.expr != R_DEAD && r.expr != R_NONE) ||
           (r.type != IMAGE_REL_LLVM_FUSED_ACCESS &&
            r.type != IMAGE_REL_LLVM_FUSED_INDEX)))
        continue;
      // An import's site, or a record chain's that read a $tpoff word,
      // reads the import's pointer again. The key's reads the image's own
      // key word, since the import may be gone, or in a direct image, whose
      // chain reads it only for the arm it does not select, the image base.
      Symbol *to = p.getRelocTarget(rels[i]);
      SymbolTable &symtab = p.file->symtab;
      r.expr = R_TYPE;
      r.sym = to;
      auto *li = dyn_cast<DefinedLocalImport>(to);
      Defined *target =
          li ? li->getTarget()
          : !isKeyImport(to) ? nullptr
          : symtab.keySym
              ? symtab.keySym
              : cast<Defined>(symtab.find(symtab.mangle("__ImageBase")));
      loads[rels[i].SymbolTableIndex] = target;
      if (TlsRecordChunk *rec = TlsRecordChunk::get(target))
        rec->setNamed(true);
    }
  });
  forEachPart(c, [&](const SectionChunk &p, ScannedSection &s) {
    ArrayRef<coff_relocation> rels = p.getRelocs();
    for (size_t j = 0, e = rels.size(); j != e; ++j) {
      auto it = loads.find(rels[j].SymbolTableIndex);
      if (it == loads.end() || s.relocs[j].expr != R_DEAD ||
          !isRead(rels[j].Type))
        continue;
      s.relocs[j] = {rels[j].VirtualAddress, rels[j].Type,
                     it->second ? R_LOCAL_LOAD : R_TYPE, false,
                     it->second ? it->second : p.getRelocTarget(rels[j])};
    }
    // Each symbol is where the object has it again, and each loop.
    if (s.aux)
      relaxChunk(p, s);
  });
}

// An ARM64 load of an offset from a thread-local record, which only fused
// indexes read, is dead where each index of that offset through the record's
// pointer in c's function takes it as its immediate; else it is the offset's
// movz.
static void settleTlsLoads(const SectionChunk &c, ScannedSection &s) {
  COFFLinkerContext &ctx = c.file->symtab.ctx;
  ArrayRef<coff_relocation> rels = c.getRelocs();
  for (size_t i = 0, e = rels.size(); i != e; ++i) {
    Relocation &r = s.relocs[i];
    std::optional<uint64_t> v;
    if (r.expr != R_TLS || r.type != IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL ||
        !c.file->getSiteFact(&c, i, SiteFactIndexedRead) ||
        !(v = ctx.tlsOffset(cast<Defined>(r.sym)->getRVA(),
                            ctx.tlsRuntime().empty())))
      continue;
    bool fit = true;
    forEachPart(c, [&](const SectionChunk &p, ScannedSection &ps) {
      ArrayRef<coff_relocation> prels = p.getRelocs();
      for (size_t j = 0, n = prels.size(); j != n && fit; ++j)
        if (ps.relocs[j].expr == R_TLS && ps.relocs[j].sym == r.sym &&
            prels[j].Type == IMAGE_REL_LLVM_FUSED_INDEX &&
            prels[j].SymbolTableIndex == rels[i].SymbolTableIndex)
          fit = addArm64Offset(read32(p, prels[j].VirtualAddress),
                               *p.file->getSiteFact(&p, j, SiteFactIndex) &
                                   SiteFactViaFirst,
                               *v)
                    .has_value();
    });
    if (fit)
      r.expr = R_DEAD;
  }
}

template <class Fn> static void forEachResizable(ObjFile *file, Fn fn) {
  for (Chunk *chunk : file->getChunks()) {
    auto *c = dyn_cast<SectionChunk>(chunk);
    ScannedSection *s = c && c->resizable ? c->getScanned() : nullptr;
    if (s && s->aux)
      fn(*c, *s);
  }
}
template <class Fn>
static void forEachResizable(COFFLinkerContext &ctx, Fn fn) {
  for (ObjFile *file : ctx.objFileInstances)
    forEachResizable(file, fn);
}

// The decisions that depend on the order of the chunks, before any is
// resized: the jump that ends a resizable chunk is deleted where the next
// chunk begins at its target, and an x86-64 jump of a section that states its
// spans to a chunk near it has two bytes where the chunks' order alone puts
// the target in reach, with the most the layout may put before each chunk
// between and every byte the object has there, of which the link only takes.
// Padding the link redoes may grow behind a point, so none lies between a
// target that precedes the jump and its section's end.
static void placeChunk(COFFLinkerContext &ctx, ArrayRef<Chunk *> chunks,
                       size_t k) {
  auto *c = dyn_cast<SectionChunk>(chunks[k]);
  ScannedSection *s = c && c->resizable ? c->getScanned() : nullptr;
  if (!s || !s->aux)
    return;
  ArrayRef<uint8_t> contents = c->getContents();
  for (Relocation &r : llvm::reverse(s->relocs)) {
    if (r.offset + 8 < contents.size() || k + 1 == chunks.size())
      break;
    uint32_t begin, end;
    if (!deletesEndJump(ctx, *c, r, begin, end,
                        [&](uint32_t from, uint32_t to) {
                          return llvm::any_of(
                              s->aux->anchors, [&](const SymbolAnchor &x) {
                                return x.offset > from && x.offset < to;
                              });
                        }) ||
        cast<DefinedRegular>(r.sym)->getChunk() != chunks[k + 1])
      continue;
    r.gone = true;
    s->aux->tail = &r - s->relocs.data();
    s->aux->fallsThrough = true;
  }
  if (!isX64(*c) || !c->compacts)
    return;
  // The most bytes the layout may put between a chunk and the one before it:
  // a chunk passes the line places it is barred from, three at most where
  // the link has yet to bar it by its loops.
  auto gap = [&](const Chunk *x) {
    auto *sc = dyn_cast<SectionChunk>(x);
    unsigned places = !sc ? 0
                      : sc->resizable && sc->file->lineLoops.count(sc)
                          ? 3
                          : llvm::popcount(sc->linePlacesBarred);
    return x->getAlignment() - 1 + ctx.config.functionPadMin +
           (x->getEntryThunk() ? 4 : 0) +
           places * std::max(x->getAlignment(), 16u);
  };
  auto most = [](const Chunk *x) -> uint64_t {
    auto *sc = dyn_cast<SectionChunk>(x);
    return !sc ? x->getSize() : sc->movable ? 128 : sc->header->SizeOfRawData;
  };
  // A jump to a function override's body keeps the 32 bits Windows writes.
  for (Relocation &r : s->relocs) {
    auto *d = dyn_cast_or_null<DefinedRegular>(r.sym);
    if (!isFarJump(r) || !d || FunctionOverride::get(d) ||
        r.offset + 4 > contents.size())
      continue;
    // The field of a direct jump holds its addend; that of one through a
    // pointer holds none.
    SectionChunk *to = d->getChunk();
    int64_t at =
        d->getValue() +
        (r.expr == R_TYPE ? int32_t(read32le(&contents[r.offset])) : 0);
    if (to == c || !to->live || to->movable || at < 0 ||
        at > to->header->SizeOfRawData ||
        llvm::any_of(s->aux->anchors, [&](const SymbolAnchor &x) {
          return x.offset > r.offset - getBytesBefore(r.type) &&
                 x.offset < r.offset + 4;
        }))
      continue;
    uint32_t index = &r - s->relocs.data();
    uint64_t between = 0;
    size_t i = k;
    while (++i != chunks.size() && between <= 127) {
      between += gap(chunks[i]);
      if (chunks[i] == to)
        break;
      between += most(chunks[i]);
    }
    if (i != chunks.size() && chunks[i] == to) {
      if (between + at <= 127)
        s->aux->betweens.emplace_back(index, between + at);
      continue;
    }
    between = gap(c);
    for (i = k; i-- && between <= 127 && chunks[i] != to;)
      between += most(chunks[i]) + gap(chunks[i]);
    ScannedSection *ts = to->resizable ? to->getScanned() : nullptr;
    between += to->header->SizeOfRawData - at;
    if (i < k && chunks[i] == to && between <= 127 &&
        (!ts || llvm::none_of(
                    ts->relocs,
                    [&](const Relocation &x) {
                      return x.type == IMAGE_REL_LLVM_ALIGN && x.offset >= at;
                    })))
      s->aux->betweens.emplace_back(index, ~between);
  }
}

// Gives the fields of sc that the span list l states their values in buf,
// the output of sc, or with no buf checks them. Returns false where one does
// not fit its field.
static bool writeSpanList(const SectionChunk &sc, const SpanList &l,
                          uint8_t *buf) {
  const SectionChunk &t = *l.target->repl;
  ScannedSection *s = t.getScanned();
  if (!s || !s->aux || !s->aux->resized)
    return true;
  ArrayRef<Edit> edits = s->aux->edits;
  ArrayRef<uint8_t> contents = sc.getContents();
  // Where a point of the code comes to lie; none lies inside the bytes of a
  // rewrite.
  bool inside = false;
  auto in = [](const Edit &e, uint32_t at) {
    uint32_t begin = e.offset - e.before;
    return begin < at && at < begin + e.size - e.skip;
  };
  auto mapAt = [&](uint32_t at, const Edit *e) {
    inside |= (e != edits.begin() && in(e[-1], at)) ||
              (e != edits.end() && in(*e, at));
    return mapOffset(t, *s, at, false, e);
  };
  auto map = [&](int64_t at) -> int64_t {
    if (at < 0 || at > t.header->SizeOfRawData) {
      inside = true;
      return 0;
    }
    return mapAt(at, findEdit(edits, at));
  };
  // The fields of the code's own list are in the order of its edits.
  const Edit *before = edits.begin();
  bool fits = true;
  // The offset of the last version-2 epilogue distance that came to zero.
  uint32_t zero = -1u, offset = 0;
  for (const uint8_t *p = l.spans.bytes_begin(), *e = l.spans.bytes_end();
       p != e;) {
    unsigned n;
    uint64_t head = decodeULEB128(p, &n);
    p += n;
    unsigned kind = head & 15;
    offset += head >> 4;
    int64_t operand = kind == SpanAddend32 ? decodeSLEB128(p, &n)
                      : kind >= SpanData   ? decodeULEB128(p, &n)
                                           : (n = 0);
    p += n;
    uint16_t type = kind == SpanRel8 ? uint16_t(IMAGE_REL_LLVM_SSPAN8)
                    : kind < SpanData || kind == SpanAddend32
                        ? uint16_t(IMAGE_REL_AMD64_SSPAN32)
                        : SpanTypes[kind - SpanData];
    // A field of 32 bits or more holds any distance in a section.
    if (!buf &&
        (type == IMAGE_REL_AMD64_SSPAN32 || type == IMAGE_REL_LLVM_SSPAN64))
      continue;
    ArrayRef<uint8_t> field = contents.drop_front(offset);
    while (&sc == &t && before != edits.end() && before->offset < offset)
      ++before;
    // A field inside a rewrite is the rewrite's.
    if (&sc == &t && kind != SpanJump32 &&
        ((before != edits.end() && in(*before, offset)) ||
         (before != edits.begin() && in(before[-1], offset))))
      continue;
    uint8_t *loc = !buf        ? nullptr
                   : &sc == &t ? buf + mapOffset(t, *s, offset, false, before)
                               : buf + sc.getOutputOffset(offset);
    int64_t v;
    if (kind == SpanAddend32) {
      int64_t a = int32_t(read32le(field.data())) - operand;
      v = map(a) - a;
    } else if (kind == SpanJump32) {
      // A jump the link narrowed is its own, written short; one to another
      // chunk has its relocation.
      if (before == edits.end() || before->offset != offset ||
          before->index != -1u)
        continue;
      v = map(offset + 4 + readSpan(type, field)) -
          (mapOffset(t, *s, offset, false, before) + 4);
      if (!inside && before->newSize == 2)
        continue;
    } else if (kind < SpanData) {
      int64_t b = offset + (kind == SpanRel8 ? 1 : 3 + kind);
      v = map(b + readSpan(type, field)) - (mapAt(offset, before) + b - offset);
    } else {
      v = map(operand + readSpan(type, field)) - map(operand);
    }
    if (inside) {
      errAt(sc, offset) << "a span names no point of " << t.getSectionName()
                        << " that the link keeps";
      return true;
    }
    // A relocated field takes its target's movement. A zero distance ends a
    // version-2 epilogue list, so only its last epilogue may be at the end.
    bool aligned;
    if (kind == SpanAddend32) {
      if (loc)
        write32le(loc, read32le(loc) + v);
    } else if (!spanFits(type, v, field, aligned) ||
               (type == IMAGE_REL_LLVM_SSPAN_EPILOGV2 && zero + 2 == offset)) {
      fits = false;
    } else if (loc) {
      writeSpan(type, loc, v, field);
    }
    if (!v && type == IMAGE_REL_LLVM_SSPAN_EPILOGV2)
      zero = offset;
  }
  return fits;
}

void lld::coff::writeSpans(const SectionChunk &sc, uint8_t *buf) {
  if (sc.file)
    for (const SpanList &l : sc.file->getSpans(&sc))
      writeSpanList(sc, l, buf);
}

void lld::coff::writeScanned(const SectionChunk &sc, uint8_t *buf,
                             const ScannedSection &s) {
  COFFLinkerContext &ctx = sc.file->symtab.ctx;
  ArrayRef<uint8_t> old = sc.getContents();
  ArrayRef<coff_relocation> rels = sc.getRelocs();
  bool resized = s.aux && s.aux->resized;
  ArrayRef<Edit> edits = resized ? ArrayRef(s.aux->edits) : ArrayRef<Edit>();
  // The bytes go straight to the output, each rewrite in place of its own.
  // The words the image leaves out of the section's start are not written.
  uint32_t copied = sc.getLeftOut();
  int64_t delta = 0;
  for (const Edit &e : edits) {
    int64_t prev = std::exchange(delta, e.delta);
    uint32_t begin = e.offset - e.before;
    if (isTransparent(e))
      continue;
    if (begin < copied) {
      Err(ctx) << sc.file << ": " << sc.getSectionName()
               << ": internal error: the rewrite at 0x"
               << Twine::utohexstr(begin) << " lies in bytes already written";
      return;
    }
    memcpy(buf + copied - prev, old.data() + copied, begin - copied);
    writeEdit(sc, s, e, buf + begin - prev, sc.getRVA() + begin - prev);
    copied = begin + e.size;
  }
  memcpy(buf + copied - delta, old.data() + copied, old.size() - copied);

  // Each relocation at its output offset, those of a rewrite's own bytes
  // aside; a thread-local base over its relocated bytes.
  const Edit *at = edits.begin();
  SmallVector<size_t, 0> scales;
  for (size_t i = 0, e = rels.size(); i != e; ++i) {
    const Relocation &r = s.relocs[i];
    while (at != edits.end() && at->offset < r.offset)
      ++at;
    bool own = false;
    for (const Edit *x = at; x != edits.end() && x->offset == r.offset; ++x)
      own |= x->index == i && writesOwn(*x);
    if (r.gone || own || r.expr == R_NONE ||
        (resized && r.expr == R_DEAD && deletesDead(sc, i)))
      continue;
    if (r.offset >= old.size()) {
      Err(ctx) << sc.file << ": relocation points beyond the end of "
               << sc.getSectionName();
      continue;
    }
    uint32_t offset =
        resized ? mapOffset(sc, s, r.offset, false, at) : r.offset;
    uint8_t *loc = buf + offset;
    if (r.expr == R_TLS_BASE)
      scales.push_back(i);
    else if (r.expr >= R_LOCAL_LOAD)
      writeSite(sc, i, r, loc, sc.getRVA() + offset);
    else if (r.expr == R_SHIFT && resized) {
      auto *d = cast<DefinedRegular>(r.sym);
      write32le(loc, read32le(loc) + d->getValue() - getInputOffset(s, d) -
                         (offset - r.offset));
    } else {
      sc.applyRelocation(loc, rels[i], offset, &r);
    }
  }
  // A section that keeps its size has its base sites written over their
  // relocated bytes.
  for (size_t i : scales)
    if (uint32_t o = s.relocs[i].offset;
        !resized && getTlsForm(sc) != TlsForm::Wide)
      writeTlsBase(sc, buf + o, buf + o, sc.getRVA() + o, false);
  writeSpans(sc, buf);
}

bool lld::coff::initRelax(COFFLinkerContext &ctx) {
  if (!ctx.hasForkObject)
    return false;
  llvm::TimeTraceScope timeScope("Relaxation decisions");
  ArrayRef<ObjFile *> files = ctx.objFileInstances;
  std::atomic<bool> any = false, moves = false;
  parallelFor(0, files.size(), [&](size_t i) {
    bool records = !files[i]->symtab.tlsRecords.empty();
    for (auto &[c, s] : files[i]->relocations) {
      if (c->live && records && c->getArch() == Triple::aarch64)
        settleTlsLoads(*c, s);
      if (!c->resizable || !c->live)
        continue;
      s.aux = std::make_unique<RelaxAux>();
      s.aux->size = c->header->SizeOfRawData;
      s.aux->places = c->linePlacesBarred;
      s.aux->loops = files[i]->lineLoops.lookup(c);
      any = true;
      if (c->movable)
        moves = true;
    }
  });
  ctx.hasMovingCode = moves;
  if (!any)
    return false;
  // Symbols move through their records, which ObjFile::parse made their own,
  // so every copy follows; a section ICF folded gives its symbols to the
  // one kept.
  for (ObjFile *file : files)
    for (auto [c, record] : std::exchange(file->movingSymbols, {}))
      if (ScannedSection *s = c->repl->getScanned(); s && s->aux)
        s->aux->anchors.push_back({record->Value, record});
  for (OutputSection *sec : ctx.outputSections)
    parallelFor(0, sec->chunks.size(),
                [&](size_t k) { placeChunk(ctx, sec->chunks, k); });
  parallelFor(0, files.size(), [&](size_t i) {
    ReachedSymbols reached(files[i]);
    forEachResizable(files[i], [&](const SectionChunk &c, ScannedSection &s) {
      decide(ctx, c, s, reached);
    });
  });
  // The spans over a section depend on its own edits alone. A span fits as
  // assembled, so it may not fit only where padding grew or a jump went,
  // which may leave an epilogue none; then the section keeps its size.
  std::vector<SmallVector<const SpanList *, 0>> misfits(files.size());
  parallelFor(0, misfits.size(), [&](size_t i) {
    for (const SpanList &l : files[i]->spanLists) {
      ScannedSection *s = l.target->repl->getScanned();
      if (!l.field->live || l.field->repl != l.field || !s || !s->aux)
        continue;
      int32_t delta = 0;
      bool mayMisfit = false;
      for (const Edit &e : s->aux->edits) {
        mayMisfit |=
            e.delta < delta || (e.kind == EditKind::Jump && !e.newSize);
        delta = e.delta;
      }
      if (mayMisfit && !writeSpanList(*l.field, l, nullptr))
        misfits[i].push_back(&l);
    }
  });
  for (ArrayRef<const SpanList *> m : misfits)
    for (const SpanList *l : m)
      if (const SectionChunk &t = *l->target->repl; !t.getScanned()->aux->fixed)
        keepSize(t);
  return true;
}

bool lld::coff::relaxOnce(COFFLinkerContext &ctx, bool refold) {
  // The sizes of sections whose code stays depend on no address.
  if (!ctx.hasMovingCode)
    return false;
  llvm::TimeTraceScope timeScope("Relax");
  // Every distance is judged in one layout, before any section changes.
  bool decided = false, changed = false;
  forEachResizable(ctx, [&](const SectionChunk &c, ScannedSection &s) {
    decided |= c.movable && lengthen(c, s, refold);
  });
  forEachResizable(ctx, [&](const SectionChunk &c, ScannedSection &s) {
    changed |= c.movable && relaxChunk(c, s);
  });
  // Each edit's form changes once, but for a fold the one refold took back,
  // so the passes end.
  if (changed && !decided)
    Fatal(ctx) << "relaxation of resizable code did not converge";
  return changed;
}

// Returns whether a span's A - B, a narrow branch's displacement, or an ARM64
// site's offset in its target's page fits.
static bool fits(const SectionChunk &sc, const coff_relocation &rel) {
  ScannedSection *ss = sc.getScanned();
  size_t i = &rel - sc.getRelocs().data();
  Defined *a = getDefined(sc.getTarget(i, ss));
  if (!a || rel.VirtualAddress >= sc.getContents().size())
    return true;
  ArrayRef<uint8_t> field = sc.getContents().drop_front(rel.VirtualAddress);
  int64_t p = sc.getRVA() + sc.getOutputOffset(rel.VirtualAddress);
  uint32_t insn = read32le(field.data());
  if (getUse(sc, rel.Type) != FusedUse::None)
    return ss->relocs[i].expr != R_PAGEOFF ||
           addPageOffset(insn, rel.Type, a->getRVA(),
                         ownsPage(ss->relocs.data(), i));
  if (sc.getPair(rel)) {
    Defined *b = getDefined(sc.getTarget(i + 1, ss));
    if (!b)
      return true;
    int64_t v = a->getRVA() - b->getRVA();
    if (rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12A ||
        rel.Type == IMAGE_REL_ARM64_PAGEOFFSET_12L)
      v += getPageOffsetImm(insn, rel.Type);
    bool aligned;
    // A zero distance ends a version-2 epilogue list, so only the last
    // epilogue may be at the function's end.
    return spanFits(rel.Type, v, field, aligned) && aligned &&
           (v || rel.Type != IMAGE_REL_LLVM_SSPAN_EPILOGV2 ||
            llvm::none_of(sc.getRelocs(), [&](const coff_relocation &r) {
              return r.VirtualAddress == rel.VirtualAddress + 2 &&
                     r.Type == IMAGE_REL_LLVM_SSPAN_EPILOGV2;
            }));
  }
  // A branch to absolute zero goes to the next instruction.
  int64_t d = a->getRVA() - p;
  switch (rel.Type) {
  case IMAGE_REL_ARM64_BRANCH19:
    return isAbsoluteZero(a) || isInt<21>(d);
  case IMAGE_REL_ARM64_BRANCH14:
    return isAbsoluteZero(a) || isInt<16>(d);
  default:
    return isInt<21>(d + getPageOffsetImm(insn, rel.Type));
  }
}

// The ARM64 sites of c's function that take an offset in the page of what the
// pointer with the given index names read the pointer again, one of them not
// fitting its sum: they stay as compiled, and the pointer's adrps and fused
// reads there are loads of a local import pointer as any other.
static void unbindPage(const SectionChunk &c, uint32_t index) {
  forEachPart(c, [&](const SectionChunk &p, ScannedSection &s) {
    ArrayRef<coff_relocation> rels = p.getRelocs();
    for (size_t i = 0, e = rels.size(); i != e; ++i) {
      Relocation &r = s.relocs[i];
      bool load = r.type == IMAGE_REL_ARM64_PAGEBASE_REL21 ||
                  r.type == IMAGE_REL_LLVM_FUSED_READ;
      if (rels[i].SymbolTableIndex != index || (!load && r.expr != R_PAGEOFF))
        continue;
      for (Edit &x : r.expr == R_DEAD && s.aux ? MutableArrayRef(s.aux->edits)
                                               : MutableArrayRef<Edit>())
        if (x.index == i)
          x.newSize = x.size, x.fold = false;
      r = {r.offset, r.type, load ? R_AARCH64_LOCAL : R_TYPE, false,
           p.getRelocTarget(rels[i])};
    }
    if (s.aux)
      relaxChunk(p, s);
  });
}

bool lld::coff::fixMisfits(COFFLinkerContext &ctx) {
  bool changed = false;
  auto keep = [&](SectionChunk *c) {
    ScannedSection *s = c && c->resizable ? c->getScanned() : nullptr;
    if (s && s->aux && s->aux->resized && !s->aux->fixed) {
      keepSize(*c);
      changed = true;
    }
  };
  auto chunkOf = [](Symbol *sym) -> SectionChunk * {
    auto *d = dyn_cast_or_null<DefinedRegular>(sym);
    return d && d->data ? *d->data : nullptr;
  };
  for (ObjFile *file : ctx.objFileInstances) {
    for (auto [sc, i] : file->fitChecks) {
      const coff_relocation &rel = sc->getRelocs()[i];
      if (!sc->live || fits(*sc, rel))
        continue;
      ScannedSection *s = sc->getScanned();
      if (s && s->relocs[i].expr == R_PAGEOFF) {
        unbindPage(*sc, rel.SymbolTableIndex);
        changed = true;
        continue;
      }
      keep(sc);
      keep(chunkOf(sc->getTarget(i, s)));
      if (sc->getPair(rel))
        keep(chunkOf(sc->getTarget(i + 1, s)));
    }
  }
  return changed;
}
