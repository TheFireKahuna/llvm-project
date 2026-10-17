//===- Relax.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A compiler may describe every value in an object that depends on where the
// code of a section lies, as it does for the sections in which the linker may
// change the size of instructions the object's sites describe: the
// alignments, branch displacements and label differences, including those of
// unwind, exception and debug data, that an assembler would otherwise fold
// into numbers. Such a section is covered, and the linker changes sizes there
// as RISC-V's linker relaxation does: it decides each instruction's new
// form, lays the code out again, lengthening a jump with an 8-bit
// displacement that no longer reaches, and writes each value the object
// describes from the new layout. A section whose new layout leaves a value
// out of its field's range keeps its size, and its instructions the forms of
// the same length that fit any layout.
//
//===----------------------------------------------------------------------===//

#include "Relax.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "LocalImports.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::support::endian;

namespace lld::coff {
uint32_t Relaxation::getOutputOffset(uint32_t offset) const {
  auto it = llvm::partition_point(
      edits, [&](const RelaxEdit &e) { return e.offset <= offset; });
  if (it == edits.begin())
    return offset;
  const RelaxEdit &e = *std::prev(it);
  if (offset < e.offset + e.oldSize)
    return e.newOffset + std::min(offset - e.offset, e.newSize);
  return offset - (e.offset + e.oldSize) + e.newOffset + e.newSize;
}

const RelaxEdit *Relaxation::findEdit(uint32_t offset) const {
  auto it = llvm::partition_point(
      edits, [&](const RelaxEdit &e) { return e.offset <= offset; });
  if (it == edits.begin())
    return nullptr;
  const RelaxEdit &e = *std::prev(it);
  return offset < e.offset + e.oldSize ? &e : nullptr;
}

// The no-operations of each length up to ten that x86-64 assemblers emit,
// longer ones taking 0x66 prefixes.
void writeNops(uint8_t *buf, size_t n, unsigned maxNop) {
  static const uint8_t nops[10][10] = {
      {0x90},
      {0x66, 0x90},
      {0x0F, 0x1F, 0x00},
      {0x0F, 0x1F, 0x40, 0x00},
      {0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  maxNop = std::clamp(maxNop, 1u, 15u);
  while (n) {
    size_t length = std::min<size_t>(n, maxNop);
    size_t prefixes = length > 10 ? length - 10 : 0;
    memset(buf, 0x66, prefixes);
    memcpy(buf + prefixes, nops[length - prefixes - 1], length - prefixes);
    buf += length;
    n -= length;
  }
}

static const Relaxation *getRelaxation(const SectionChunk *sc) {
  if (!sc->relaxed)
    return nullptr;
  return &sc->file->symtab.ctx.relaxations.find(sc)->second;
}

// The relaxation whose layout the code of the section numbered target, in
// file, takes, if the linker resizes it.
static const Relaxation *getTargetRelaxation(const ObjFile *file,
                                             uint32_t target) {
  COFFLinkerContext &ctx = file->symtab.ctx;
  const SectionChunk *sc = ctx.relaxedSections.lookup({file, target});
  return sc ? getRelaxation(sc) : nullptr;
}

// The length of the jump with a 32-bit displacement that the linker makes of
// one with an 8-bit displacement and opcode: jmp rel32, or the conditional
// jump of the same condition.
static unsigned getPromotedSize(uint8_t opcode) {
  return opcode == 0xEB ? 5 : 6;
}

// The value that c, an entry of sc's file that measures the code of r's
// chunk, takes in the new layout: the difference of two places, one of them
// the field's own for a PC-relative value. A jump that the linker lengthened
// or shortened measures from the end of the new jump.
static int64_t getValue(const ObjFile::LinkCoverage &c, const Relaxation &r) {
  int64_t a = r.getOutputOffset(c.a);
  if (c.type == LinkCoverageDifference)
    return a - r.getOutputOffset(c.b) + c.constant;
  if (c.kind == LinkFieldJump)
    if (const RelaxEdit *e = r.findEdit(c.offset);
        e && e->newSize != e->oldSize)
      return a - (e->newOffset + e->newSize);
  return a - r.getOutputOffset(c.offset) + c.constant;
}

// Whether value fits a field of kind and size.
static bool fits(int64_t value, unsigned kind, unsigned size) {
  switch (kind) {
  case LinkFieldData:
    return size == 8 || (value < (int64_t(1) << (size * 8)) &&
                         value > -(int64_t(1) << (size * 8)));
  case LinkFieldSigned:
  case LinkFieldJump:
    return size == 8 || isIntN(size * 8, value);
  case LinkFieldULEB128:
    return value >= 0 && (size * 7 >= 64 || value < (int64_t(1) << size * 7));
  case LinkFieldSLEB128:
    return size * 7 >= 64 || isIntN(size * 7, value);
  case LinkFieldCVAnnotation:
    return isUInt<29>(value);
  default:
    return false;
  }
}

static void writeField(uint8_t *loc, int64_t value, unsigned kind,
                       unsigned size) {
  switch (kind) {
  case LinkFieldULEB128:
    encodeULEB128(value, loc, size);
    break;
  case LinkFieldSLEB128:
    encodeSLEB128(value, loc, size);
    break;
  case LinkFieldCVAnnotation:
    write32be(loc, 0xC0000000 | uint32_t(value));
    break;
  default:
    for (unsigned i = 0; i != size; ++i)
      loc[i] = uint8_t(uint64_t(value) >> (i * 8));
    break;
  }
}

// Lays the code of r out, recomputing each alignment's padding from where it
// now begins, and lengthening each jump whose 8-bit displacement no longer
// reaches. A jump once lengthened stays so, which bounds the solve.
static void solve(Relaxation &r, ArrayRef<uint8_t> data) {
  for (;;) {
    int64_t delta = 0;
    for (RelaxEdit &e : r.edits) {
      e.newOffset = e.offset + delta;
      if (e.kind == RelaxEdit::Align) {
        uint64_t padding =
            offsetToAlignment(e.newOffset, Align(1ULL << e.coverage->kind));
        e.newSize = padding <= e.coverage->b ? padding : 0;
      }
      delta += int64_t(e.newSize) - int64_t(e.oldSize);
    }
    r.size = data.size() + delta;
    bool changed = false;
    for (RelaxEdit &e : r.edits) {
      if (e.kind != RelaxEdit::Branch || e.promoted)
        continue;
      int64_t value =
          int64_t(r.getOutputOffset(e.coverage->a)) - (e.newOffset + e.newSize);
      if (!isInt<8>(value)) {
        e.promoted = true;
        e.newSize =
            e.oldSize == 2 ? getPromotedSize(data[e.offset]) : e.oldSize;
        changed = true;
      }
    }
    if (!changed)
      return;
  }
}

// Decides how to lay out the code of sc, a covered chunk, or leaves it as it
// is: where nothing changes size, or where the new layout leaves a value the
// object describes out of its field's range, or in bytes the linker
// rewrites. values are the values that measure sc's code. Sets *misfit where
// a value leaves sc as it is.
static bool relax(COFFLinkerContext &ctx, SectionChunk *sc,
                  ArrayRef<const ObjFile::LinkCoverage *> values,
                  bool *misfit) {
  ObjFile *file = sc->file;
  ArrayRef<uint8_t> data = sc->getContents();
  Relaxation r;
  addSiteEdits(sc, r.edits);
  for (const ObjFile::LinkCoverage &c : file->getLinkCoverage(sc)) {
    if (c.type == LinkCoverageAlign) {
      RelaxEdit e{c.offset, c.a, c.a, 0, RelaxEdit::Align};
      e.coverage = &c;
      r.edits.push_back(e);
    } else if (c.type == LinkCoveragePCRelative && c.kind == LinkFieldJump) {
      // The opcode of jmp or of a conditional jump precedes its
      // displacement. A jump with a 32-bit one starts out with 8 bits, and
      // takes its own form back only where those do not hold the value, so
      // that the layout is the smallest the jumps fit.
      uint8_t opcode = c.offset ? data[c.offset - 1] : 0;
      uint32_t start = c.offset - 1;
      if (c.size == 4 && opcode != 0xE9) {
        start = c.offset - 2;
        opcode = c.offset > 1 && data[start] == 0x0F ? opcode : 0;
      }
      bool isJump = c.size == 1
                        ? opcode == 0xEB || (opcode & 0xF0) == 0x70
                        : c.size == 4 &&
                              (opcode == 0xE9 || (opcode & 0xF0) == 0x80);
      if (!isJump) {
        Err(ctx) << file << ": the instruction at offset 0x"
                 << Twine::utohexstr(start) << " in " << sc->getSectionName()
                 << " is not the jump its link-only record describes";
        return false;
      }
      RelaxEdit e{start, c.offset + c.size - start, 2, 0, RelaxEdit::Branch};
      e.coverage = &c;
      r.edits.push_back(e);
    }
  }
  // Code where no instruction may change its size, a jump that the assembler
  // lengthened included, keeps its layout.
  if (llvm::all_of(r.edits,
                   [](const RelaxEdit &e) { return e.newSize == e.oldSize; }))
    return false;
  // Padding of no length comes before the instruction where it ends.
  llvm::sort(r.edits, [](const RelaxEdit &a, const RelaxEdit &b) {
    return std::make_tuple(a.offset, a.oldSize != 0) <
           std::make_tuple(b.offset, b.oldSize != 0);
  });
  for (size_t i = 0; i != r.edits.size(); ++i) {
    const RelaxEdit &e = r.edits[i];
    if (uint64_t(e.offset) + e.oldSize > data.size() ||
        (i && r.edits[i - 1].offset + r.edits[i - 1].oldSize > e.offset)) {
      Err(ctx) << file << ": the instructions that the link-only records of "
               << sc->getSectionName() << " describe at offset 0x"
               << Twine::utohexstr(e.offset) << " overlap";
      return false;
    }
  }
  solve(r, data);

  // Each value must fit its field, and lie outside every rewritten
  // instruction but a jump that keeps its form, which it is a part of.
  for (const ObjFile::LinkCoverage *c : values) {
    SectionChunk *field = file->getSectionChunk(c->field);
    if (!field || !field->live)
      continue;
    if (field == sc) {
      const RelaxEdit *e = r.findEdit(c->offset);
      if (e && e->kind != RelaxEdit::Branch) {
        *misfit = true;
        return false;
      }
      if (e && e->newSize != e->oldSize)
        continue;
    }
    if (!fits(getValue(*c, r), c->kind, c->size)) {
      *misfit = true;
      return false;
    }
  }
  ctx.relaxations[sc] = std::move(r);
  sc->relaxed = true;
  return true;
}

void relaxSections(COFFLinkerContext &ctx) {
  if (llvm::none_of(ctx.objFileInstances,
                    [](ObjFile *f) { return f->hasLinkCoverage(); }))
    return;
  llvm::TimeTraceScope timeScope("Relax covered sections");
  size_t covered = 0, relaxed = 0, misfits = 0;
  int64_t saved = 0;
  for (ObjFile *file : ctx.objFileInstances) {
    if (!file->hasLinkCoverage())
      continue;
    DenseMap<uint32_t, SmallVector<const ObjFile::LinkCoverage *, 0>> values;
    for (const ObjFile::LinkCoverage &c : file->getAllLinkCoverage())
      if (c.type == LinkCoverageDifference || c.type == LinkCoveragePCRelative)
        values[c.target].push_back(&c);
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live || sc->repl != sc || !file->isCovered(sc))
        continue;
      ++covered;
      ArrayRef<const ObjFile::LinkCoverage *> chunkValues;
      if (auto it = values.find(sc->getSectionNumber()); it != values.end())
        chunkValues = it->second;
      bool misfit = false;
      if (relax(ctx, sc, chunkValues, &misfit)) {
        ++relaxed;
        saved += int64_t(sc->getContents().size()) - ctx.relaxations[sc].size;
      } else {
        misfits += misfit;
        keepFusedAccesses(sc);
      }
    }
  }
  // A chunk that identical code folding replaced takes its replacement's
  // layout, which is the same code's.
  for (ObjFile *file : ctx.objFileInstances) {
    if (!file->hasLinkCoverage())
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (sc && sc->repl->relaxed)
        ctx.relaxedSections[{file, sc->getSectionNumber()}] = sc->repl;
    }
  }
  Log(ctx) << "Relaxed " << relaxed << " of " << covered
           << " covered sections, " << saved << " bytes smaller; " << misfits
           << " kept their size where a value would not fit";
}

// Writes the instruction or padding that e makes of the bytes it changes, at
// buf in sc's contents as written, whose original contents are data.
static void writeEdit(const SectionChunk *sc, const RelaxEdit &e, uint8_t *buf,
                      ArrayRef<uint8_t> data, const Relaxation &r) {
  uint64_t p = sc->getRVA() + e.newOffset;
  const uint8_t *old = data.data() + e.offset;
  switch (e.kind) {
  case RelaxEdit::DirectCall:
  case RelaxEdit::DirectJump:
    buf[0] = e.kind == RelaxEdit::DirectCall ? 0xE8 : 0xE9;
    write32le(buf + 1, e.target->getRVA() - (p + 5));
    break;
  case RelaxEdit::PointerCall:
    buf[0] = 0xFF;
    buf[1] = 0x15;
    write32le(buf + 2, e.target->getRVA() - (p + 6));
    break;
  case RelaxEdit::PointerJump:
    buf[0] = 0x48;
    buf[1] = 0xFF;
    buf[2] = 0x25;
    write32le(buf + 3, e.target->getRVA() - (p + 7));
    break;
  case RelaxEdit::KeepTail:
    memcpy(buf, old + e.oldSize - e.newSize, e.newSize);
    break;
  case RelaxEdit::KeepHead:
    memcpy(buf, old, e.newSize);
    break;
  case RelaxEdit::Delete:
    break;
  case RelaxEdit::Align:
    writeNops(buf, e.newSize, e.coverage->size);
    break;
  case RelaxEdit::Branch: {
    // A jump that keeps its form holds the value written with the others.
    int64_t value =
        r.getOutputOffset(e.coverage->a) - (e.newOffset + e.newSize);
    if (e.newSize == e.oldSize) {
      memcpy(buf, old, e.oldSize);
    } else if (e.newSize == 2) {
      buf[0] = old[0] == 0xE9 ? 0xEB : 0x70 | (old[1] & 0x0F);
      buf[1] = uint8_t(value);
    } else {
      if (old[0] == 0xEB) {
        buf[0] = 0xE9;
      } else {
        buf[0] = 0x0F;
        buf[1] = 0x80 | (old[0] & 0x0F);
      }
      write32le(buf + e.newSize - 4, value);
    }
    break;
  }
  case RelaxEdit::Access: {
    // The bytes before ModRM, with the base register's high bit clear: REX.B,
    // or VEX's or EVEX's inverted B set. Then ModRM addressing RIP plus a
    // 32-bit displacement, never scaled, with no SIB byte; then what followed
    // the old displacement, such as an immediate.
    const ObjFile::LinkFusion &f = *e.fusion;
    unsigned dispSize = f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0;
    int64_t disp = 0;
    if (dispSize == 1)
      disp = int64_t(int8_t(old[f.modrm + 1 + f.sib])) * (1 << f.scale);
    else if (dispSize == 4)
      disp = int32_t(read32le(old + f.modrm + 1 + f.sib));
    memcpy(buf, old, f.modrm);
    if (f.prefix != 15) {
      if (f.prefixKind == LinkAccessREX)
        buf[f.prefix] &= ~1;
      else
        buf[f.prefix + 1] |= 0x20;
    }
    buf[f.modrm] = (old[f.modrm] & 0x38) | 0x05;
    unsigned tail = f.modrm + 1 + f.sib + dispSize;
    memcpy(buf + f.modrm + 5, old + tail, f.length - tail);
    write32le(buf + f.modrm + 1, e.target->getRVA() + disp - (p + e.newSize));
    break;
  }
  }
}

void writeRelaxed(const SectionChunk *sc, uint8_t *buf) {
  const Relaxation &r = *getRelaxation(sc);
  ArrayRef<uint8_t> data = sc->getContents();
  // The bytes the image leaves out overlap what precedes the chunk; they are
  // padding and a prefix before the function's entry, which no change
  // reaches.
  uint32_t prev = sc->getElidedSize();
  assert(r.edits.empty() || r.edits.front().offset >= prev);
  for (const RelaxEdit &e : r.edits) {
    memcpy(buf + r.getOutputOffset(prev), data.data() + prev, e.offset - prev);
    prev = e.offset + e.oldSize;
  }
  memcpy(buf + r.getOutputOffset(prev), data.data() + prev, data.size() - prev);

  // An instruction the linker rewrites takes no relocation of its own.
  for (const coff_relocation &rel : sc->getRelocs()) {
    if (rel.VirtualAddress >= data.size()) {
      Err(sc->file->symtab.ctx)
          << "relocation points beyond the end of its parent section";
      continue;
    }
    if (!r.findEdit(rel.VirtualAddress))
      sc->applyRelocation(buf + r.getOutputOffset(rel.VirtualAddress), rel,
                          /*rewrites=*/true);
  }
  for (const RelaxEdit &e : r.edits)
    writeEdit(sc, e, buf + e.newOffset, data, r);
  writeCoverageValues(sc, buf, 0, data.size());
}

void writeCoverageValues(const SectionChunk *sc, uint8_t *buf, uint32_t begin,
                         uint32_t end) {
  ArrayRef<ObjFile::LinkCoverage> coverage = sc->file->getLinkCoverage(sc);
  if (coverage.empty())
    return;
  const Relaxation *self = getRelaxation(sc);
  uint32_t outBegin = self ? self->getOutputOffset(begin) : begin;
  for (const ObjFile::LinkCoverage &c : coverage) {
    if (c.offset < begin || c.offset >= end ||
        (c.type != LinkCoverageDifference && c.type != LinkCoveragePCRelative))
      continue;
    const Relaxation *r = getTargetRelaxation(sc->file, c.target);
    if (!r)
      continue;
    uint32_t out = c.offset;
    if (self) {
      // A lengthened or shortened jump holds the displacement it was
      // written with.
      if (const RelaxEdit *e = self->findEdit(c.offset);
          e && e->kind == RelaxEdit::Branch && e->newSize != e->oldSize)
        continue;
      out = self->getOutputOffset(c.offset);
    }
    writeField(buf + (out - outBegin), getValue(c, *r), c.kind, c.size);
  }
}

int64_t getAddendShift(const SectionChunk *sc, uint32_t offset,
                       const SectionChunk *target, uint32_t value,
                       int64_t addend) {
  const Relaxation &r = *getRelaxation(target);
  uint32_t place = value + addend;
  if (std::optional<uint32_t> interior = sc->file->getLinkInterior(sc, offset))
    place = *interior;
  return (int64_t(r.getOutputOffset(place)) - place) -
         (int64_t(r.getOutputOffset(value)) - value);
}
} // namespace lld::coff
