//===- Relax.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A change the linker makes to an instruction is an edit of its chunk, decided
// when the reference it serves is bound. A covered section's object also
// describes every value that depends on where its code lies (alignments, branch
// displacements, label differences, including in unwind, exception and debug
// data); the linker may then resize it as RISC-V relaxation does, laying the
// code out again and rewriting those values, or keep its size where one would
// not fit its field.
//
//===----------------------------------------------------------------------===//

#include "Relax.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "ImportSlots.h"
#include "Symbols.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
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
    return e.newOffset + std::min<uint32_t>(offset - e.offset, e.newSize);
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

static void reportOverlap(const SectionChunk *sc, uint32_t offset) {
  Err(sc->file->symtab.ctx)
      << sc->file << ": the instructions that the link-only records of "
      << sc->getSectionName() << " describe at offset 0x"
      << Twine::utohexstr(offset) << " overlap";
}

void addEdit(SectionChunk *sc, RelaxEdit e) {
  std::vector<RelaxEdit> &edits = sc->file->symtab.ctx.relaxations[sc].edits;
  auto it = llvm::partition_point(
      edits, [&](const RelaxEdit &x) { return x.offset <= e.offset; });
  if ((it != edits.begin() &&
       std::prev(it)->offset + std::prev(it)->oldSize > e.offset) ||
      (it != edits.end() && e.offset + e.oldSize > it->offset)) {
    reportOverlap(sc, e.offset);
    return;
  }
  e.newOffset = e.offset;
  e.newSize = e.oldSize;
  edits.insert(it, e);
  sc->edited = true;
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

// The relaxation whose layout section target of file takes, if resized. A
// folded section takes the layout of the replacement that holds its code.
static const Relaxation *getTargetRelaxation(const ObjFile *file,
                                             uint32_t target) {
  const SectionChunk *sc = file->getSectionChunk(target);
  return sc ? getRelaxation(sc->repl) : nullptr;
}

// The length of the 32-bit-displacement jump the linker makes from an 8-bit
// one: jmp rel32, or the conditional jump of the same condition.
static unsigned getPromotedSize(uint8_t opcode) {
  return opcode == 0xEB ? 5 : 6;
}

// The value coverage entry c takes in r's new layout: a difference of two
// places, one the field's own for a PC-relative value. A lengthened or
// shortened jump measures from the end of the new jump.
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
            offsetToAlignment(e.newOffset, Align(1ULL << e.coverage()->kind));
        e.newSize = padding <= e.coverage()->b ? padding : 0;
      }
      delta += int64_t(e.newSize) - int64_t(e.oldSize);
    }
    r.size = data.size() + delta;
    bool changed = false;
    for (RelaxEdit &e : r.edits) {
      // A jump starts out with 8 bits, and once lengthened stays so.
      if (e.kind != RelaxEdit::Branch || e.newSize != 2)
        continue;
      int64_t value = int64_t(r.getOutputOffset(e.coverage()->a)) -
                      (e.newOffset + e.newSize);
      if (!isInt<8>(value)) {
        e.newSize =
            e.oldSize == 2 ? getPromotedSize(data[e.offset]) : e.oldSize;
        changed = true;
      }
    }
    if (!changed)
      return;
  }
}

// Whether e is part of how the linker lays out resized code rather than a
// change to an instruction that a reference's binding made.
static bool isLayoutEdit(const RelaxEdit &e) {
  return e.kind == RelaxEdit::Align || e.kind == RelaxEdit::Branch ||
         e.kind == RelaxEdit::TailJump;
}

// Whether e changes a fused access or index, which only resized code does.
static bool isAccessEdit(const RelaxEdit &e) {
  return e.kind == RelaxEdit::Access || e.kind == RelaxEdit::AccessAddress ||
         e.kind == RelaxEdit::AccessOffset || e.kind == RelaxEdit::Index;
}

// Leaves sc at its size, not resizing it (yet): each edit takes its old-length
// form. When final, a fused access keeps its register -- as does the load that
// fills it, revived and rewritten as if it had not been dead, or as it was
// where the load is an access itself -- since only resized code changes it.
static void keepSize(COFFLinkerContext &ctx, SectionChunk *sc, bool final) {
  sc->relaxed = false;
  auto it = ctx.relaxations.find(sc);
  if (it == ctx.relaxations.end())
    return;
  DenseSet<uint32_t> loads;
  if (final)
    for (const ObjFile::LinkFusion &f : sc->file->getLinkFusions(sc))
      if (f.use == ObjFile::LinkFusion::Access ||
          f.use == ObjFile::LinkFusion::Index)
        loads.insert(f.load);
  std::vector<RelaxEdit> &edits = it->second.edits;
  auto revived = [&](const RelaxEdit &e) {
    return e.kind == RelaxEdit::Dispose && e.fusion()->isLoad &&
           loads.contains(e.fusion()->offset);
  };
  for (RelaxEdit &e : edits)
    if (revived(e) && e.target) {
      e.kind = RelaxEdit::LoadAddress;
      e.entry = nullptr;
    }
  llvm::erase_if(edits, [&](const RelaxEdit &e) {
    return isLayoutEdit(e) || (final && isAccessEdit(e)) || revived(e);
  });
  for (RelaxEdit &e : edits) {
    e.newOffset = e.offset;
    e.newSize = e.oldSize;
  }
  if (edits.empty()) {
    ctx.relaxations.erase(it);
    sc->edited = false;
  }
}

// Lays out covered chunk sc with its edits in their shorter forms, leaving out
// the function-ending jump where tailJump is set. values measure sc's code.
// Fails where nothing changes size, or where the new layout puts a described
// value out of its field's range or into bytes the linker rewrites; sets
// *misfit in the latter case.
static bool layOut(COFFLinkerContext &ctx, SectionChunk *sc, Relaxation &r,
                   ArrayRef<const ObjFile::LinkCoverage *> values,
                   bool tailJump, bool *misfit) {
  ObjFile *file = sc->file;
  ArrayRef<uint8_t> data = sc->getContents();
  ArrayRef<ObjFile::LinkCoverage> coverage = file->getLinkCoverage(sc);
  r.edits.reserve(r.edits.size() + coverage.size() + tailJump);
  if (tailJump)
    r.edits.push_back({uint32_t(data.size() - 5), 5, 0, RelaxEdit::TailJump});
  for (const ObjFile::LinkCoverage &c : coverage) {
    if (c.type == LinkCoverageAlign) {
      RelaxEdit e{c.offset, uint16_t(c.a), uint16_t(c.a), RelaxEdit::Align};
      e.entry = &c;
      r.edits.push_back(e);
    } else if (c.type == LinkCoveragePCRelative && c.kind == LinkFieldJump) {
      // The opcode precedes the displacement. A 32-bit jump starts 8-bit and
      // takes its form back only where 8 bits do not hold the value, so the
      // layout is the smallest the jumps fit.
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
      RelaxEdit e{start, uint16_t(c.offset + c.size - start), 2,
                  RelaxEdit::Branch};
      e.entry = &c;
      r.edits.push_back(e);
    }
  }
  for (RelaxEdit &e : r.edits)
    e.newSize = e.relaxedSize;
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
      reportOverlap(sc, e.offset);
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
  return true;
}

// Resizes sc, a covered chunk, as layOut decides, or leaves it at its size. A
// chunk with no edits yet has them only if it is resized.
static bool relax(COFFLinkerContext &ctx, SectionChunk *sc,
                  ArrayRef<const ObjFile::LinkCoverage *> values, bool tailJump,
                  bool *misfit) {
  keepSize(ctx, sc, /*final=*/false);
  auto it = ctx.relaxations.find(sc);
  Relaxation added;
  Relaxation &r = it == ctx.relaxations.end() ? added : it->second;
  if (!layOut(ctx, sc, r, values, tailJump, misfit)) {
    keepSize(ctx, sc, /*final=*/false);
    return false;
  }
  if (&r == &added)
    ctx.relaxations[sc] = std::move(added);
  sc->relaxed = true;
  sc->edited = true;
  return true;
}

// The values that measure the code of each covered section of file, by
// section number.
static DenseMap<uint32_t, SmallVector<const ObjFile::LinkCoverage *, 0>>
getCoverageValues(ObjFile *file) {
  DenseMap<uint32_t, SmallVector<const ObjFile::LinkCoverage *, 0>> values;
  for (const ObjFile::LinkCoverage &c : file->getAllLinkCoverage())
    if (c.type == LinkCoverageDifference || c.type == LinkCoveragePCRelative)
      values[c.target].push_back(&c);
  return values;
}

void leaveOutTailJumps(COFFLinkerContext &ctx,
                       ArrayRef<std::pair<SectionChunk *, Defined *>> jumps,
                       function_ref<void()> layOut) {
  if (jumps.empty())
    return;
  llvm::TimeTraceScope timeScope("Leave out tail jumps");
  DenseMap<ObjFile *,
           DenseMap<uint32_t, SmallVector<const ObjFile::LinkCoverage *, 0>>>
      fileValues;
  // Lays out sc, a covered chunk, with or without its tail jump, or leaves it
  // as it is where nothing changes size or a value would not fit.
  auto relaxAgain = [&](SectionChunk *sc, bool tailJump) {
    auto [it, inserted] = fileValues.try_emplace(sc->file);
    if (inserted)
      it->second = getCoverageValues(sc->file);
    ArrayRef<const ObjFile::LinkCoverage *> values;
    if (auto v = it->second.find(sc->getSectionNumber()); v != it->second.end())
      values = v->second;
    bool misfit = false;
    return relax(ctx, sc, values, tailJump, &misfit);
  };
  SmallVector<std::pair<SectionChunk *, Defined *>, 0> left;
  for (auto [sc, target] : jumps) {
    bool wasRelaxed = sc->relaxed;
    if (relaxAgain(sc, true))
      left.push_back({sc, target});
    else if (wasRelaxed)
      relaxAgain(sc, false);
  }
  if (left.empty())
    return;
  // Padding or prefix placement may still separate a jump's chunk from its
  // target's. Such a jump comes back, which only moves what follows, so the
  // loop ends.
  for (bool restored = true; restored;) {
    layOut();
    size_t before = left.size();
    llvm::erase_if(left, [&](const auto &jump) {
      auto [sc, target] = jump;
      if (target->getRVA() == sc->getRVA() + sc->getSize())
        return false;
      if (!relaxAgain(sc, false))
        keepSize(ctx, sc, /*final=*/true);
      return true;
    });
    restored = left.size() != before;
  }
  for (auto [sc, target] : left)
    ctx.tailJumpTargets.insert(target->getChunk());
  Log(ctx) << "Left out " << left.size() << " of " << jumps.size()
           << " jumps that end their functions before their targets";
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
    DenseMap<uint32_t, SmallVector<const ObjFile::LinkCoverage *, 0>> values =
        getCoverageValues(file);
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast<SectionChunk>(c);
      if (!sc || !sc->live || sc->repl != sc || !file->isCovered(sc))
        continue;
      ++covered;
      ArrayRef<const ObjFile::LinkCoverage *> chunkValues;
      if (auto it = values.find(sc->getSectionNumber()); it != values.end())
        chunkValues = it->second;
      bool misfit = false;
      if (relax(ctx, sc, chunkValues, false, &misfit)) {
        ++relaxed;
        saved += int64_t(sc->getContents().size()) - ctx.relaxations[sc].size;
      } else {
        misfits += misfit;
        keepSize(ctx, sc, /*final=*/true);
      }
    }
  }
  Log(ctx) << "Relaxed " << relaxed << " of " << covered
           << " covered sections, " << saved << " bytes smaller; " << misfits
           << " kept their size where a value would not fit";
}

// Disposes of the dead load of size bytes at start (RVA rva) without moving
// anything: its bytes become segment-override prefixes of the next instruction,
// which then runs alone, when the two together are at most fifteen bytes (the
// decode limit) and, for a control transfer, cross no new 32-byte boundary
// (which a branch pays for). Otherwise the load becomes one no-operation.
static void disposeOfLoad(uint8_t *start, uint64_t rva, unsigned size,
                          const ObjFile::LinkFusion &f) {
  if (f.nextSize && size + f.nextSize <= 15 &&
      (!f.nextTransfers || rva / 32 == (rva + size) / 32)) {
    memset(start, 0x2E, size);
    return;
  }
  writeNops(start, size, size);
}

uint32_t getSectionOffset(const SectionChunk *sc, Defined *d) {
  OutputSection *os = sc->file->symtab.ctx.getOutputSection(d->getChunk());
  return os ? d->getRVA() - os->getRVA() : 0;
}

// Writes the instruction or padding that e makes of the bytes it changes, at
// buf in sc's contents as written, whose original contents are data.
static void writeEdit(const SectionChunk *sc, const RelaxEdit &e, uint8_t *buf,
                      ArrayRef<uint8_t> data, const Relaxation &r) {
  uint64_t p = sc->getRVA() + e.newOffset;
  const uint8_t *old = data.data() + e.offset;
  switch (e.kind) {
  case RelaxEdit::DirectCall:
  case RelaxEdit::DirectJump: {
    uint8_t *inst = buf;
    if (e.kind == RelaxEdit::DirectCall && e.newSize > 5)
      *inst++ = 0x67;
    inst[0] = e.kind == RelaxEdit::DirectCall ? 0xE8 : 0xE9;
    write32le(inst + 1, e.target->getRVA() - (p + (inst - buf) + 5));
    memset(inst + 5, 0xCC, buf + e.newSize - (inst + 5));
    break;
  }
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
  case RelaxEdit::LoadPointer:
  case RelaxEdit::LoadAddress: {
    // The load of an add's sum writes the add's register: ModRM.reg, and its
    // high bits, REX.R, or REX2's R3 with R4 clear.
    memcpy(buf, old, e.newSize);
    uint8_t *off = buf + e.newSize - 4;
    off[-2] = e.kind == RelaxEdit::LoadPointer ? 0x8B : 0x8D;
    int64_t addend = 0;
    if (const ObjFile::LinkFusion *f = e.fusion()) {
      off[-1] = (off[-1] & 0xC7) | (f->reg & 7) << 3;
      off[-3] =
          (off[-3] & (e.newSize == 8 ? ~0x44 : ~0x04)) | (f->reg & 8) >> 1;
      addend = f->addend;
    }
    write32le(off, e.target->getRVA() + addend - (p + e.newSize));
    break;
  }
  case RelaxEdit::ZeroLoad: {
    // mov r/m64, imm32, moving the register from ModRM.reg to ModRM.rm, and so
    // its high bits from R to B in the prefix, as lld/ELF's relaxGotNoPic does.
    // REX is 0100WRXB, REX2's payload M R4 X4 B4 W R3 X3 B3; the X and B bits,
    // ignored by a RIP-relative operand, are cleared.
    memcpy(buf, old, e.newSize);
    uint8_t *off = buf + e.newSize - 4;
    uint8_t rex = off[-3];
    off[-3] = e.newSize == 8 ? (rex & 0x08) | (rex & 0x44) >> 2
                             : (rex & 0xF8) | (rex & 0x4) >> 2;
    off[-2] = 0xC7;
    off[-1] = 0xC0 | (off[-1] & 0x38) >> 3;
    write32le(off, 0);
    break;
  }
  case RelaxEdit::ZeroCall: {
    // xor r11d, r11d; call r11: pushes the same return address and faults at
    // the same address. R11 carries no argument and is the linker's scratch
    // register at a call.
    static const uint8_t call[] = {0x45, 0x31, 0xDB, 0x41, 0xFF, 0xD3};
    memcpy(buf, call, sizeof(call));
    break;
  }
  case RelaxEdit::KeepTail:
    memcpy(buf, old + e.oldSize - e.newSize, e.newSize);
    break;
  case RelaxEdit::KeepHead:
    memcpy(buf, old, e.newSize);
    break;
  case RelaxEdit::Dispose:
    if (!e.newSize)
      break;
    if (e.fusion()->isLoad)
      disposeOfLoad(buf, p, e.newSize, *e.fusion());
    else
      writeNops(buf, e.newSize, e.newSize);
    break;
  case RelaxEdit::TailJump:
    break;
  case RelaxEdit::Align:
    writeNops(buf, e.newSize, e.coverage()->size);
    break;
  case RelaxEdit::Branch: {
    // A jump that keeps its form holds the value written with the others.
    int64_t value =
        r.getOutputOffset(e.coverage()->a) - (e.newOffset + e.newSize);
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
  case RelaxEdit::Access:
  case RelaxEdit::AccessAddress: {
    // The bytes before ModRM, with the base register's high bit cleared (REX.B,
    // or VEX/EVEX's inverted B set); then ModRM addressing RIP plus a 32-bit
    // displacement, no SIB; then what followed the old displacement. A load
    // of an address becomes lea of it.
    const ObjFile::LinkFusion &f = *e.fusion();
    unsigned dispSize = f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0;
    int64_t disp = 0;
    if (e.kind == RelaxEdit::Access && dispSize == 1)
      disp = int64_t(int8_t(old[f.modrm + 1 + f.sib])) * (1 << f.scale);
    else if (e.kind == RelaxEdit::Access && dispSize == 4)
      disp = int32_t(read32le(old + f.modrm + 1 + f.sib));
    memcpy(buf, old, f.modrm);
    if (e.kind == RelaxEdit::AccessAddress)
      buf[f.modrm - 1] = 0x8D;
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
  case RelaxEdit::AccessOffset: {
    // mov r32, imm32, the register moving from ModRM.reg, and its high bit
    // from REX.R to REX.B.
    const ObjFile::LinkFusion &f = *e.fusion();
    uint8_t *inst = buf;
    if (f.prefix != 15 && (old[f.prefix] & 4))
      *inst++ = 0x41;
    *inst++ = 0xB8 | (old[f.modrm] >> 3 & 7);
    write32le(inst, getSectionOffset(sc, e.target));
    break;
  }
  case RelaxEdit::Index: {
    // The bytes before ModRM; then ModRM addressing the register kept plus a
    // 32-bit displacement, with a SIB byte only where that register is RSP or
    // R12; then what followed the old displacement. The kept register's high
    // bit moves to REX.B, or VEX's inverted B, and the index's is cleared.
    const ObjFile::LinkFusion &f = *e.fusion();
    unsigned dispSize = f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0;
    uint8_t sib = old[f.modrm + 1];
    int64_t disp = dispSize == 1   ? int8_t(old[f.modrm + 2])
                   : dispSize == 4 ? int32_t(read32le(old + f.modrm + 2))
                                   : 0;
    bool vex = f.prefixKind == LinkAccessVEX;
    // The X and B bits, X above B.
    unsigned xb = f.prefix == 15 ? 0
                  : vex          ? ~old[f.prefix + 1] >> 5 & 3
                                 : old[f.prefix] & 3;
    unsigned reg =
        f.base ? (sib >> 3 & 7) | (xb & 2) << 2 : (sib & 7) | (xb & 1) << 3;
    memcpy(buf, old, f.modrm);
    if (f.prefix != 15) {
      if (vex)
        buf[f.prefix + 1] =
            (old[f.prefix + 1] & ~0x20) | 0x40 | (reg & 8 ? 0 : 0x20);
      else
        buf[f.prefix] = (old[f.prefix] & ~3) | reg >> 3;
    }
    uint8_t *inst = buf + f.modrm;
    *inst++ = (old[f.modrm] & 0x38) | 0x80 | (reg & 7);
    if ((reg & 7) == 4)
      *inst++ = 0x24;
    write32le(inst, disp + getSectionOffset(sc, e.target));
    unsigned tail = f.modrm + 2 + dispSize;
    memcpy(inst + 4, old + tail, f.length - tail);
    break;
  }
  case RelaxEdit::Arm64Page:
    memcpy(buf, old, 4);
    applyArm64Addr(buf, e.target->getRVA(), p, 12);
    break;
  case RelaxEdit::Arm64Add:
  case RelaxEdit::Arm64Load:
  case RelaxEdit::Arm64Zero:
    memcpy(buf, old, 4);
    setArm64Opcode(buf, e.kind == RelaxEdit::Arm64Add    ? 0x91000000
                        : e.kind == RelaxEdit::Arm64Load ? 0xF9400000
                                                         : 0xD2800000);
    if (e.kind == RelaxEdit::Arm64Add)
      applyArm64Imm(buf, e.target->getRVA() & 0xFFF, 0);
    else if (e.kind == RelaxEdit::Arm64Load)
      applyArm64Ldr(buf, e.target->getRVA() & 0xFFF);
    break;
  }
}

void writeEdited(const SectionChunk *sc, uint8_t *buf) {
  // An ARM64 chunk may have only fused accesses and adds to write.
  static const Relaxation none;
  auto it = sc->file->symtab.ctx.relaxations.find(sc);
  const Relaxation &r =
      it == sc->file->symtab.ctx.relaxations.end() ? none : it->second;
  ArrayRef<uint8_t> data = sc->getContents();
  // The bytes the image leaves out overlap what precedes the chunk: padding and
  // a prefix before the entry, which no change reaches.
  uint32_t prev = sc->getElidedSize();
  assert(r.edits.empty() || r.edits.front().offset >= prev);
  for (const RelaxEdit &e : r.edits) {
    memcpy(buf + r.getOutputOffset(prev), data.data() + prev, e.offset - prev);
    prev = e.offset + e.oldSize;
  }
  memcpy(buf + r.getOutputOffset(prev), data.data() + prev, data.size() - prev);

  for (const coff_relocation &rel : sc->getRelocs()) {
    if (rel.VirtualAddress >= data.size()) {
      Err(sc->file->symtab.ctx)
          << "relocation points beyond the end of its parent section";
      continue;
    }
    if (!r.findEdit(rel.VirtualAddress))
      sc->applyRelocation(buf + r.getOutputOffset(rel.VirtualAddress), rel);
  }
  for (const RelaxEdit &e : r.edits)
    writeEdit(sc, e, buf + e.newOffset, data, r);
  if (sc->getArch() == Triple::aarch64 && sc->file->hasLinkFusions())
    writeFusedArm64(sc, buf);
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

int64_t getAddendShift(const SectionChunk *sc, const coff_relocation &rel,
                       const SectionChunk *target, uint32_t value,
                       int64_t addend) {
  const Relaxation &r = *getRelaxation(target);
  uint32_t place = value + addend;
  if (const ObjFile::RelocFact *f = sc->file->relocFact(sc, rel);
      f && f->hasInterior)
    place = f->interior;
  return (int64_t(r.getOutputOffset(place)) - place) -
         (int64_t(r.getOutputOffset(value)) - value);
}
} // namespace lld::coff
