//===- LocalImports.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A reference to the import pointer of a symbol the image defines reads a local
// import pointer the linker makes. Where the linker knows the instruction
// holding the reference, it rewrites it to reach the symbol directly and leaves
// out a pointer only rewritten instructions read.
//
// A compiler may also describe fused calls through the register a pointer load
// fills, and the load. A call becomes direct into the image, stays through the
// register out of it or for a delay-loaded import, and a load no call reads is
// disposed of. Described data accesses and the add of a constant to such a
// register are handled likewise: where the data is in the image and the code
// may be resized, each access reaches it RIP-relative and the load, taking any
// add's sum, goes.
//
//===----------------------------------------------------------------------===//

#include "LocalImports.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "InputFiles.h"
#include "Relax.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/AArch64Instructions.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld::aarch64;

namespace lld::coff {
bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off, LinkSiteForm form) {
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
            is_contained(
                {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF2, 0xF3},
                prefix));
  case LinkSiteLoad:
  case LinkSiteLoadREX2:
  case LinkSiteAddress: {
    // mov r64, [rip+d] or lea r64, [rip+d], with REX.W, or with REX2 and its
    // W bit. A load's form says which.
    bool rex = (prefix & 0xF8) == 0x48;
    bool rex2 = off >= 4 && data[off - 4] == 0xD5 && (prefix & 0x88) == 0x08;
    return off >= 3 && opcode == (form == LinkSiteAddress ? 0x8D : 0x8B) &&
           (modrm & 0xC7) == 0x05 &&
           (form == LinkSiteLoad       ? rex
            : form == LinkSiteLoadREX2 ? rex2
                                       : rex || rex2);
  }
  default:
    return false;
  }
}

// The form that the object of sc describes for rel, a REL32 field.
static std::optional<LinkSiteForm> getSiteForm(const SectionChunk *sc,
                                               const coff_relocation &rel) {
  if (!sc->file->describesSites() || rel.Type != IMAGE_REL_AMD64_REL32)
    return std::nullopt;
  const ObjFile::RelocFact *f = sc->file->relocFact(sc, rel);
  if (!f || !f->hasSite)
    return std::nullopt;
  return f->form;
}

// Whether a local import pointer holds zero, the default of an absent weak
// reference, which an instruction can materialise instead of loading.
static bool isZeroPointer(DefinedLocalImport *li) {
  auto *abs = dyn_cast<DefinedAbsolute>(li->getTarget());
  return abs && abs->getVA() == 0;
}

// Whether a reference to a local import pointer can reach its symbol directly.
// An absolute symbol has no image-relative address, but zero works.
static bool canBypass(ObjFile *file, const coff_relocation &rel) {
  auto *li = dyn_cast_or_null<DefinedLocalImport>(
      file->getSymbol(rel.SymbolTableIndex));
  return li && !li->getChunk()->isCell() &&
         (!isa<DefinedAbsolute>(li->getTarget()) || isZeroPointer(li));
}

std::optional<LinkSiteForm> getDescribedSite(const SectionChunk *sc,
                                             const coff_relocation &rel) {
  std::optional<LinkSiteForm> form = getSiteForm(sc, rel);
  if (form && isSiteForm(sc->getContents(), rel.VirtualAddress, *form))
    return form;
  return std::nullopt;
}

// A 64-bit integer load with an unsigned offset, ldr xt, [xn, #imm].
static bool isLdrX(const Ldr &ldr) {
  return ldr.p2Size == 3 && !ldr.isFloat &&
         ldr.extendType == ZeroExtend;
}

bool isArm64AddressPair(const SectionChunk *sc, const coff_relocation &adrp,
                        const coff_relocation &add) {
  ArrayRef<uint8_t> data = sc->getContents();
  if (adrp.Type != IMAGE_REL_ARM64_PAGEBASE_REL21 ||
      add.Type != IMAGE_REL_ARM64_PAGEOFFSET_12A ||
      adrp.SymbolTableIndex != add.SymbolTableIndex ||
      uint64_t(adrp.VirtualAddress) + 4 != add.VirtualAddress ||
      uint64_t(add.VirtualAddress) + 4 > data.size())
    return false;
  // adrp xd with no addend, then add xd, xd, #0.
  Adrp first;
  Add second;
  return parseAdrp(read32le(&data[adrp.VirtualAddress]), first) &&
         first.addend == 0 &&
         parseAdd(read32le(&data[add.VirtualAddress]), second) &&
         second.addend == 0 && second.destRegister == first.destRegister &&
         second.srcRegister == first.destRegister;
}

bool isArm64PointerLoad(const SectionChunk *sc, const coff_relocation &rel) {
  ArrayRef<uint8_t> data = sc->getContents();
  if (uint64_t(rel.VirtualAddress) + 4 > data.size())
    return false;
  uint32_t insn = read32le(&data[rel.VirtualAddress]);
  switch (rel.Type) {
  case IMAGE_REL_ARM64_PAGEBASE_REL21: {
    Adrp adrp;
    return parseAdrp(insn, adrp) && adrp.addend == 0;
  }
  case IMAGE_REL_ARM64_PAGEOFFSET_12L: {
    Ldr ldr;
    return parseLdr(insn, ldr) && isLdrX(ldr) && ldr.offset == 0;
  }
  default:
    return false;
  }
}

bool isValueOnlyLoad(const SectionChunk *sc, size_t i, LinkSiteForm form,
                     uint16_t distance, size_t &page) {
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  const coff_relocation &rel = relocs[i];
  if (sc->getArch() != Triple::aarch64) {
    page = i;
    return rel.Type == IMAGE_REL_AMD64_REL32 &&
           (form == LinkSiteLoad || form == LinkSiteLoadREX2) &&
           isSiteForm(sc->getContents(), rel.VirtualAddress, form);
  }
  // The adrp, before the ldr in its section, writes the register that is
  // the ldr's base, and refers to the same symbol.
  uint64_t dist = uint64_t(distance) * 4;
  if (rel.Type != IMAGE_REL_ARM64_PAGEOFFSET_12L || !distance ||
      dist > rel.VirtualAddress || !isArm64PointerLoad(sc, rel))
    return false;
  page = llvm::partition_point(relocs,
                               [&](const coff_relocation &r) {
                                 return r.VirtualAddress <
                                        rel.VirtualAddress - dist;
                               }) -
         relocs.begin();
  if (page == relocs.size() ||
      relocs[page].VirtualAddress != rel.VirtualAddress - dist ||
      relocs[page].Type != IMAGE_REL_ARM64_PAGEBASE_REL21 ||
      relocs[page].SymbolTableIndex != rel.SymbolTableIndex ||
      !isArm64PointerLoad(sc, relocs[page]))
    return false;
  ArrayRef<uint8_t> data = sc->getContents();
  Adrp adrp;
  Ldr ldr;
  return parseAdrp(read32le(&data[relocs[page].VirtualAddress]), adrp) &&
         parseLdr(read32le(&data[rel.VirtualAddress]), ldr) &&
         adrp.destRegister == ldr.baseRegister;
}

// Rewrites the described instruction of form whose REL32 field is rel to reach
// li's symbol directly in the same length: a load becomes lea, a call addr32
// call rel32, a jump jmp rel32 then int3, or each materialises zero for a
// zero-holding pointer. Where the code is resized, a call or jump drops the
// bytes after the direct one.
static void addSiteEdit(SectionChunk *sc, const coff_relocation &rel,
                        LinkSiteForm form, DefinedLocalImport *li) {
  uint32_t off = rel.VirtualAddress;
  Defined *target = li->getTarget();
  bool zero = isZeroPointer(li);
  switch (form) {
  case LinkSiteCall:
    addEdit(sc, {off - 2, 6, uint16_t(zero ? 6 : 5),
                 zero ? RelaxEdit::ZeroCall : RelaxEdit::DirectCall, target});
    break;
  case LinkSiteJump:
    addEdit(sc, {off - 2, 6, 5, RelaxEdit::DirectJump, target});
    break;
  case LinkSiteJumpOnePrefix:
    addEdit(sc, {off - 3, 7, 5, RelaxEdit::DirectJump, target});
    break;
  default: {
    uint16_t size = form == LinkSiteLoadREX2 ? 8 : 7;
    addEdit(sc, {off + 4 - size, size, size,
                 zero ? RelaxEdit::ZeroLoad : RelaxEdit::LoadAddress, target});
    break;
  }
  }
}

// The register that the described load whose REL32 field is at off fills:
// ModRM.reg, with REX.R, or with REX2's R3 and R4.
static unsigned getLoadRegister(ArrayRef<uint8_t> data, uint32_t off,
                                LinkSiteForm form) {
  uint8_t prefix = data[off - 3];
  unsigned reg = (data[off - 1] >> 3 & 7) | (prefix & 0x04) << 1;
  if (form == LinkSiteLoadREX2)
    reg |= (prefix & 0x40) >> 2;
  return reg;
}

// Whether a fused call stays a call through the register holding its pointer:
// where it cannot bypass the pointer to reach its function directly (the
// pointer leads out of the image, or holds an absent weak function's zero),
// but not through a delay-loaded import's pointer, which holds the loader's
// stub until the first call.
static bool goesThroughRegister(ObjFile *file, const coff_relocation &rel) {
  Symbol *s = file->getSymbol(rel.SymbolTableIndex);
  if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s))
    return !canBypass(file, rel) || isZeroPointer(li);
  if (auto *imp = dyn_cast_or_null<DefinedImportData>(s))
    return !imp->file->isDelayLoaded();
  return isa_and_nonnull<Defined>(s);
}

// Writes at out the bytes of a fused call through register reg and returns how
// many, or zero for a jump through a register the unwinder cannot take. A call
// ends six bytes on, where the memory call does, so it pushes the same return
// address, segment overrides padding the front. A jump begins as the REX.W
// memory jump, since the unwinder takes a register jump ending an epilogue only
// as REX.W jmp through one of the first sixteen; int3 pads to seven bytes.
static size_t getFusedCallBytes(uint8_t *out, bool isJump, unsigned reg) {
  if (isJump) {
    if (reg >= 16)
      return 0;
    out[0] = 0x48 | (reg >> 3 & 1); // REX.W, with B
    out[1] = 0xFF;
    out[2] = 0xE0 | (reg & 7);
    memset(out + 3, 0xCC, 4);
    return 7;
  }
  uint8_t call[4];
  size_t n = 0;
  if (reg >= 16) {
    call[n++] = 0xD5; // REX2, with B4 and B3
    call[n++] = (reg & 16) | (reg >> 3 & 1);
  } else if (reg >= 8) {
    call[n++] = 0x41; // REX.B
  }
  call[n++] = 0xFF;
  call[n++] = 0xD0 | (reg & 7);
  memset(out, 0x2E, 6 - n);
  memcpy(out + 6 - n, call, n);
  return 6;
}

// The REL32 relocation of sc at off, if any.
static const coff_relocation *getRel32At(const SectionChunk *sc,
                                         uint32_t off) {
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  auto it = llvm::partition_point(relocs, [&](const coff_relocation &r) {
    return r.VirtualAddress < off;
  });
  if (it == relocs.end() || it->VirtualAddress != off ||
      it->Type != IMAGE_REL_AMD64_REL32)
    return nullptr;
  return &*it;
}

// Whether a fused call, access or add may reach the data or function its
// load's pointer leads to directly: a local import pointer into the image.
static bool reachesDirectly(ObjFile *file, const coff_relocation &rel) {
  auto *li = dyn_cast_or_null<DefinedLocalImport>(
      file->getSymbol(rel.SymbolTableIndex));
  return li && canBypass(file, rel) && !isZeroPointer(li);
}

// Whether the bytes at f are the access it describes: the prefix it names (REX
// without X, or three-byte VEX/EVEX with ~X set, holding the base register's
// high bit) and a ModRM addressing memory through reg, the load's register,
// with the given displacement and SIB byte, no index, within f's length.
static bool isFusedAccess(ArrayRef<uint8_t> data, const ObjFile::LinkFusion &f,
                          unsigned reg) {
  if (!f.length || uint64_t(f.offset) + f.length > data.size() ||
      f.modrm >= f.length || (f.prefix != 15 && f.prefix + 1u >= f.modrm) ||
      f.disp == 3 || f.prefixKind > LinkAccessEVEX ||
      (f.prefixKind != LinkAccessREX && f.prefix == 15))
    return false;
  const uint8_t *inst = &data[f.offset];
  unsigned high = 0;
  if (f.prefix != 15) {
    uint8_t p = inst[f.prefix];
    switch (f.prefixKind) {
    case LinkAccessREX:
      if ((p & 0xF2) != 0x40)
        return false;
      high = p & 1;
      break;
    case LinkAccessVEX:
    case LinkAccessEVEX:
      if (p != (f.prefixKind == LinkAccessVEX ? 0xC4 : 0x62) ||
          !(inst[f.prefix + 1] & 0x40))
        return false;
      high = !(inst[f.prefix + 1] & 0x20);
      break;
    }
  }
  uint8_t modrm = inst[f.modrm];
  unsigned mod = modrm >> 6, rm = modrm & 7, base = rm;
  if (mod != f.disp || (rm == 4) != f.sib || (mod == 0 && rm == 5))
    return false;
  if (f.sib) {
    if (f.modrm + 1u >= f.length)
      return false;
    uint8_t sib = inst[f.modrm + 1];
    base = sib & 7;
    if ((sib >> 3 & 7) != 4 || (mod == 0 && base == 5))
      return false;
  }
  unsigned dispSize = f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0;
  return (base | high << 3) == reg &&
         f.modrm + 1u + f.sib + dispSize <= f.length;
}

// The length of the add f describes if its bytes are a form it may take: add
// or sub of an 8- or 32-bit immediate with REX.W writing reg (the load's
// register), or lea with REX.W of reg plus an 8- or 32-bit displacement
// writing f's register. Zero otherwise.
static unsigned getFusedAddLength(ArrayRef<uint8_t> data,
                                  const ObjFile::LinkFusion &f, unsigned reg) {
  uint64_t off = f.offset;
  if (off + 3 > data.size() || reg >= 16 || f.reg >= 16)
    return 0;
  uint8_t rex = data[off], op = data[off + 1], modrm = data[off + 2];
  if ((rex & 0xFA) != 0x48)
    return 0;
  auto readImm = [&](uint64_t at, unsigned size) -> std::optional<int64_t> {
    if (at + size > data.size())
      return std::nullopt;
    if (size == 1)
      return int8_t(data[at]);
    return int32_t(read32le(&data[at]));
  };
  if (op == 0x83 || op == 0x81) {
    unsigned ext = modrm >> 3 & 7;
    if (modrm >> 6 != 3 || (ext != 0 && ext != 5) || (rex & 4) ||
        unsigned((modrm & 7) | (rex & 1) << 3) != reg || f.reg != reg)
      return 0;
    unsigned size = op == 0x83 ? 1 : 4;
    std::optional<int64_t> v = readImm(off + 3, size);
    if (!v || (ext == 5 ? -*v : *v) != f.addend)
      return 0;
    return 3 + size;
  }
  if (op != 0x8D)
    return 0;
  unsigned mod = modrm >> 6, base = modrm & 7;
  unsigned dest = (modrm >> 3 & 7) | (rex & 4) << 1;
  if ((mod != 1 && mod != 2) || dest != f.reg)
    return 0;
  uint64_t at = off + 3;
  if (base == 4) {
    if (at >= data.size() || (data[at] >> 3 & 7) != 4)
      return 0;
    base = data[at++] & 7;
  }
  if ((base | (rex & 1) << 3) != reg)
    return 0;
  unsigned size = mod == 1 ? 1 : 4;
  std::optional<int64_t> v = readImm(at, size);
  if (!v || *v != f.addend)
    return 0;
  return at + size - off;
}

// The PAGEOFFSET_12L relocation of sc at off, if any: an ARM64 fused load's.
static const coff_relocation *getPageOffsetAt(const SectionChunk *sc,
                                              uint32_t off) {
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  auto it = llvm::partition_point(relocs, [&](const coff_relocation &r) {
    return r.VirtualAddress < off;
  });
  if (it == relocs.end() || it->VirtualAddress != off ||
      it->Type != IMAGE_REL_ARM64_PAGEOFFSET_12L)
    return nullptr;
  return &*it;
}

// The immediate offset of an ARM64 access through a base register: a load,
// store or prefetch at an unsigned 12-bit offset, or a pair of loads or stores
// at a signed 7-bit one, each scaled by the size of one register it accesses.
struct Arm64Access {
  unsigned scale = 0;
  bool pair = false;
  bool store = false;

  explicit Arm64Access(uint32_t insn) {
    bool simd = insn & 0x04000000;
    unsigned opc = insn >> 30;
    if ((insn & 0x3B000000) == 0x39000000) {
      // A SIMD and floating-point access with opc bit 1 set is 128-bit.
      scale = simd && (insn & 0x00800000) ? 16 : 1u << opc;
      store = simd ? !(insn & 0x00400000) : !(insn & 0x00C00000);
    } else if ((insn & 0x3B800000) == 0x29000000 && opc != 3 &&
               !(opc == 1 && !simd && !(insn & 0x00400000))) {
      // ldpsw, opc 01, has no store.
      pair = true;
      scale = simd ? 4u << opc : (opc == 2 ? 8 : 4);
      store = !(insn & 0x00400000);
    }
  }
  // The offset in bytes that insn's field holds.
  int64_t getOffset(uint32_t insn) const {
    if (pair)
      return SignExtend64<7>(insn >> 15) * scale;
    return int64_t(insn >> 10 & 0xFFF) * scale;
  }
  // Whether offset fits the field, a multiple of the scale.
  bool fits(int64_t offset) const {
    if (offset % scale)
      return false;
    return pair ? isInt<7>(offset / scale) : isUInt<12>(offset / scale);
  }
  uint32_t setOffset(uint32_t insn, int64_t offset) const {
    if (pair)
      return (insn & ~0x003F8000) | uint32_t(offset / scale & 0x7F) << 15;
    return (insn & ~0x003FFC00) | uint32_t(offset / scale) << 10;
  }
};

// Whether the bytes at off in data are those of the ARM64 fused access or add
// f, given the register reg that its load, ldr xT, [xT, #:lo12:p], fills.
static bool isArm64FusedUse(ArrayRef<uint8_t> data,
                            const ObjFile::LinkFusion &f, unsigned reg) {
  if (uint64_t(f.offset) + 4 > data.size())
    return false;
  uint32_t insn = read32le(&data[f.offset]);
  unsigned rn = insn >> 5 & 31, rt = insn & 31;
  if (f.use == ObjFile::LinkFusion::Access) {
    Arm64Access access(insn);
    unsigned rt2 = insn >> 10 & 31;
    return access.scale && rn == reg &&
           !(access.store && (rt == reg || (access.pair && rt2 == reg)));
  }
  // add or sub of an immediate, shifted by 0 or 12, to xT.
  bool sub = (insn & 0xFF800000) == 0xD1000000;
  if ((insn & 0xFF800000) != 0x91000000 && !sub)
    return false;
  int64_t imm = int64_t(insn >> 10 & 0xFFF) << (insn & 0x00400000 ? 12 : 0);
  return rn == reg && rt == f.reg && (sub ? -imm : imm) == f.addend;
}

// Binds the ARM64 fused accesses and adds of sc: each names a load of an import
// pointer into its base register, with the adrp the pointer's page fills. Where
// the pointer is a local import every reference bypasses, the adrp computes the
// data's page and each reaches it directly; the load is then dead.
static void bindFusedArm64(SectionChunk *sc,
                           MutableArrayRef<ObjFile::LinkFusion> fusions,
                           function_ref<void(uint32_t)> mismatch,
                           size_t &accesses, size_t &adds, size_t &direct,
                           size_t &loads, size_t &dead) {
  ObjFile *file = sc->file;
  ArrayRef<uint8_t> data = sc->getContents();
  DenseSet<uint32_t> keptLoads;
  for (ObjFile::LinkFusion &f : fusions) {
    if (f.isLoad || f.use == ObjFile::LinkFusion::Call)
      continue;
    const coff_relocation *loadRel = getPageOffsetAt(sc, f.load);
    if (!loadRel)
      continue;
    // The accesses take the data's offset in the page adrp gives the load's
    // register; an access that only reads may read the copy and take nothing.
    uint32_t load = read32le(&data[f.load]);
    unsigned reg = load & 31;
    if (!isArm64PointerLoad(sc, *loadRel) ||
        (!f.valueRead && (load >> 5 & 31) != reg)) {
      mismatch(f.load);
      continue;
    }
    if (!isArm64FusedUse(data, f, reg)) {
      mismatch(f.offset);
      continue;
    }
    auto *li = dyn_cast_or_null<DefinedLocalImport>(
        file->getSymbol(loadRel->SymbolTableIndex));
    // A bypassed pointer's adrp and load compute the data's page and address,
    // as relaxLocalImport rewrites them. Where the load stays, an access that
    // only reads immutable data may read the image's copy.
    if (li && li->getChunk()->bypassed && !isZeroPointer(li)) {
      f.form = ObjFile::LinkFusion::Direct;
    } else {
      keptLoads.insert(f.load);
      if (f.use == ObjFile::LinkFusion::Access && f.valueRead &&
          file->symtab.immutableReads.contains(
              file->getSymbol(loadRel->SymbolTableIndex)))
        f.form = ObjFile::LinkFusion::Direct;
    }
    ++(f.use == ObjFile::LinkFusion::Access ? accesses : adds);
    direct += f.form == ObjFile::LinkFusion::Direct;
  }
  for (ObjFile::LinkFusion &f : fusions) {
    if (!f.isLoad)
      continue;
    ++loads;
    f.dead = !keptLoads.contains(f.offset) && getPageOffsetAt(sc, f.offset);
    dead += f.dead;
  }
  // writeFusedArm64 writes what reaches the data directly, as the chunk is.
  sc->edited |= llvm::any_of(fusions, [](const ObjFile::LinkFusion &f) {
    return f.dead || f.form == ObjFile::LinkFusion::Direct;
  });
}

// A fused call names the load that fills its register. Each call that stays
// through the register keeps its load; a fused load no call keeps is dead. A
// fact whose bytes are not the described code, call form, or load register is
// an error.
void bindFusedCalls(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  if ((symtab.machine != AMD64 && symtab.machine != ARM64) ||
      ctx.hybridSymtab)
    return;
  llvm::TimeTraceScope timeScope("Bind fused calls");
  size_t calls = 0, throughRegister = 0, loads = 0, dead = 0, accesses = 0,
         adds = 0, direct = 0;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &symtab || !file->hasLinkFusions())
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      MutableArrayRef<ObjFile::LinkFusion> fusions = file->getLinkFusions(sc);
      if (fusions.empty())
        continue;
      ArrayRef<uint8_t> data = sc->getContents();
      auto mismatch = [&](uint32_t off) {
        Err(ctx) << file << ": the instruction at offset 0x"
                 << Twine::utohexstr(off) << " in " << sc->getSectionName()
                 << " is not the one its link-only record describes";
      };
      auto isLoad = [](std::optional<LinkSiteForm> form) {
        return form == LinkSiteLoad || form == LinkSiteLoadREX2;
      };
      if (symtab.machine == ARM64) {
        bindFusedArm64(sc, fusions, mismatch, accesses, adds, direct, loads,
                       dead);
        continue;
      }

      // An access reaches the data directly only where the code may be resized,
      // since a RIP-relative operand is longer; one that only reads immutable
      // data reads the image's copy while the load stays for whatever else
      // reads it.
      bool covered = file->isCovered(sc);
      Relaxation *edits = nullptr;
      if (auto it = ctx.relaxations.find(sc); it != ctx.relaxations.end())
        edits = &it->second;
      DenseSet<uint32_t> keptLoads;
      for (ObjFile::LinkFusion &f : fusions) {
        if (f.isLoad)
          continue;
        const coff_relocation *loadRel = getRel32At(sc, f.load);
        if (!loadRel)
          continue;
        std::optional<LinkSiteForm> loadForm = getDescribedSite(sc, *loadRel);
        Symbol *s = file->getSymbol(loadRel->SymbolTableIndex);
        if (!isLoad(loadForm) || !s) {
          mismatch(f.load);
          continue;
        }
        unsigned reg = getLoadRegister(data, f.load, *loadForm);
        auto *li = dyn_cast<DefinedLocalImport>(s);
        bool reaches = reachesDirectly(file, *loadRel);

        if (f.use == ObjFile::LinkFusion::Access) {
          if (!isFusedAccess(data, f, reg)) {
            mismatch(f.offset);
            continue;
          }
          if (!covered || !reaches)
            keptLoads.insert(f.load);
          if (covered &&
              (reaches || (f.valueRead && symtab.immutableReads.contains(s)))) {
            unsigned dispSize = f.disp == 1 ? 1 : f.disp == 2 ? 4 : 0;
            addEdit(sc, {f.offset, f.length,
                         uint16_t(f.length - dispSize - f.sib + 4),
                         RelaxEdit::Access,
                         li ? li->getTarget() : symtab.immutableReads.lookup(s),
                         &f});
            ++direct;
          }
          ++accesses;
          continue;
        }
        // The load takes the sum of an add, which then goes.
        if (f.use == ObjFile::LinkFusion::Add) {
          unsigned length = getFusedAddLength(data, f, reg);
          if (!length) {
            mismatch(f.offset);
            continue;
          }
          RelaxEdit *load = edits ? edits->findEdit(f.load) : nullptr;
          if (reaches && load && load->kind == RelaxEdit::LoadAddress) {
            load->entry = &f;
            addEdit(sc, {f.offset, uint16_t(length), 0, RelaxEdit::Dispose,
                         nullptr, &f});
            ++direct;
          }
          ++adds;
          continue;
        }

        uint8_t bytes[7];
        size_t size = getFusedCallBytes(bytes, f.isJump, f.reg);
        if (!size || uint64_t(f.offset) + size > data.size() ||
            memcmp(&data[f.offset], bytes, size) != 0) {
          mismatch(f.offset);
          continue;
        }
        if (reg != f.reg) {
          mismatch(f.load);
          continue;
        }
        ++calls;
        bool through = goesThroughRegister(file, *loadRel);
        if (!through && li) {
          addEdit(sc, {f.offset, uint16_t(size), 5,
                       f.isJump ? RelaxEdit::DirectJump : RelaxEdit::DirectCall,
                       li->getTarget()});
        } else if (!through && isa<Defined>(s)) {
          addEdit(sc,
                  {f.offset, uint16_t(size), uint16_t(size),
                   f.isJump ? RelaxEdit::PointerJump : RelaxEdit::PointerCall,
                   cast<Defined>(s)});
        } else {
          // Resized, a jump keeps its first three bytes (REX.W jmp through the
          // register) and drops the int3; a call keeps its last bytes (the
          // register call) and drops the segment overrides before them.
          keptLoads.insert(f.load);
          ++throughRegister;
          if (covered && f.isJump)
            addEdit(sc, {f.offset, 7, 3, RelaxEdit::KeepHead});
          else if (covered)
            addEdit(sc, {f.offset, 6,
                         uint16_t(f.reg >= 16  ? 4
                                  : f.reg >= 8 ? 3
                                               : 2),
                         RelaxEdit::KeepTail});
        }
      }
      // A load nothing reads goes. One rewritten to reach its data directly
      // keeps what it becomes if the code keeps its size, and its accesses
      // their register.
      for (ObjFile::LinkFusion &f : fusions) {
        if (!f.isLoad)
          continue;
        ++loads;
        const coff_relocation *rel = getRel32At(sc, f.offset);
        if (keptLoads.contains(f.offset) || !rel)
          continue;
        std::optional<LinkSiteForm> form = getDescribedSite(sc, *rel);
        if (!isLoad(form)) {
          mismatch(f.offset);
          continue;
        }
        uint16_t size = form == LinkSiteLoadREX2 ? 8 : 7;
        if (RelaxEdit *e = edits ? edits->findEdit(f.offset) : nullptr) {
          e->kind = RelaxEdit::Dispose;
          e->relaxedSize = 0;
          e->entry = &f;
        } else {
          addEdit(sc, {f.offset + 4 - size, size, 0, RelaxEdit::Dispose,
                       nullptr, &f});
        }
        ++dead;
      }
    }
  }
  if (calls || loads)
    Log(ctx) << "Fused calls: " << throughRegister << " of " << calls
             << " through their register; fused loads: " << dead << " of "
             << loads << " dead";
  if (accesses || adds)
    Log(ctx) << "Fused accesses and adds: " << direct << " of "
             << accesses + adds << " direct";
}

void writeFusedArm64(const SectionChunk *sc, uint8_t *buf) {
  const uint32_t nop = 0xD503201F;
  MutableArrayRef<ObjFile::LinkFusion> fusions = sc->file->getLinkFusions(sc);
  auto getData = [&](uint32_t load) -> Defined * {
    const coff_relocation *rel = getPageOffsetAt(sc, load);
    auto *li = dyn_cast<DefinedLocalImport>(
        sc->file->getSymbol(rel->SymbolTableIndex));
    return li->getTarget();
  };
  for (const ObjFile::LinkFusion &f : fusions) {
    // Where the data's page offset plus an add's constant fits 12 bits, the
    // load (which relaxLocalImport made an add of the offset) adds the sum into
    // the add's register, and the add becomes a no-op.
    if (f.isLoad || f.form != ObjFile::LinkFusion::Direct)
      continue;
    if (f.use == ObjFile::LinkFusion::Add) {
      int64_t sum = (getData(f.load)->getRVA() & 0xFFF) + f.addend;
      if (sum < 0 || sum > 0xFFF)
        continue;
      uint32_t load = read32le(buf + f.load);
      write32le(buf + f.load,
                0x91000000 | uint32_t(sum) << 10 | (load & 0x3E0) | f.reg);
      write32le(buf + f.offset, nop);
    }
  }
  // An access that only reads immutable data, whose load stays, reads the
  // image's copy at an offset from its own address where the copy is near
  // enough and aligned; otherwise it reads through its register.
  for (const ObjFile::LinkFusion &f : fusions) {
    if (f.isLoad || f.use != ObjFile::LinkFusion::Access || !f.valueRead ||
        f.form != ObjFile::LinkFusion::Direct)
      continue;
    const coff_relocation *rel = getPageOffsetAt(sc, f.load);
    Defined *copy = sc->file->symtab.immutableReads.lookup(
        sc->file->getSymbol(rel->SymbolTableIndex));
    Ldr ldr;
    if (!copy || !parseLdr(read32le(buf + f.offset), ldr))
      continue;
    ldr.offset += int64_t(copy->getRVA()) - int64_t(sc->getRVA() + f.offset);
    if (isLiteralLdrEligible(ldr))
      writeLiteralLdr(buf + f.offset, ldr);
  }
  // The accesses of each load, grouped once in fusion order, so a dead load
  // reaches its own without a scan of every fusion.
  DenseMap<uint32_t, SmallVector<const ObjFile::LinkFusion *, 2>> accessesOf;
  for (const ObjFile::LinkFusion &f : fusions)
    if (!f.isLoad && f.use == ObjFile::LinkFusion::Access)
      accessesOf[f.load].push_back(&f);
  // A dead load whose accesses all take the data's page offset, within their
  // fields and aligned to their scale, becomes a no-op, its register keeping
  // the adrp page; otherwise the accesses keep offsets the load then computes.
  for (const ObjFile::LinkFusion &l : fusions) {
    if (!l.isLoad || !l.dead)
      continue;
    uint64_t lo12 = getData(l.offset)->getRVA() & 0xFFF;
    auto it = accessesOf.find(l.offset);
    if (it == accessesOf.end())
      continue;
    ArrayRef<const ObjFile::LinkFusion *> accesses = it->second;
    auto fits = [&](const ObjFile::LinkFusion *f) {
      uint32_t insn = read32le(buf + f->offset);
      Arm64Access access(insn);
      return access.fits(lo12 + access.getOffset(insn));
    };
    if (accesses.empty() || !llvm::all_of(accesses, fits))
      continue;
    for (const ObjFile::LinkFusion *f : accesses) {
      uint32_t insn = read32le(buf + f->offset);
      Arm64Access access(insn);
      write32le(buf + f->offset,
                access.setOffset(insn, lo12 + access.getOffset(insn)));
    }
    write32le(buf + l.offset, nop);
  }
}

// Whether file's references to local import pointers are reported only when
// they still read the pointer after binding, rather than each as link.exe
// reports them. Our compiler reaches every symbol it cannot place in the image
// through its import pointer, which the linker rewrites, so only the references
// that keep the pointer warrant the warning: on x86-64 these objects describe
// their sites, on ARM64 they carry link-only records.
bool defersLocalImportWarning(const ObjFile *file, bool arm64) {
  return file->describesSites() || (arm64 && file->hasLinkRecords());
}

// A .refptr.X section holds only a pointer to X, read where X may be outside
// the image or absent. When X is in the image or is an absent weak reference,
// the pointer binds a local import to X or to zero, whose rewritten references
// reach it directly. This pays only where references can be rewritten: ARM64,
// and x86-64 objects that describe their sites. The section stays a pointer
// wherever the image still needs one.
void bindPointerCells(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  if (symtab.pointerCells.empty() || symtab.isEC() || ctx.hybridSymtab)
    return;
  if (symtab.machine != ARM64 &&
      (symtab.machine != AMD64 ||
       llvm::none_of(ctx.objFileInstances,
                     [](ObjFile *f) { return f->describesSites(); })))
    return;
  llvm::TimeTraceScope timeScope("Bind pointer cells");
  for (Symbol *s : symtab.pointerCells) {
    // A symbol bound already, as one that several objects define is.
    auto *cell = dyn_cast<DefinedRegular>(s);
    if (!cell)
      continue;
    SectionChunk *sc = cell->getChunk();
    if (!sc || sc->getSize() != ctx.config.wordsize ||
        sc->getRelocs().size() != 1 || sc->getRelocs()[0].VirtualAddress != 0 ||
        !sc->children().empty())
      continue;
    StringRef name = cell->getName().substr(strlen(".refptr."));
    Symbol *x = sc->file->getSymbol(sc->getRelocs()[0].SymbolTableIndex);
    if (!x || x->getName() != name)
      continue;

    // An unresolved weak reference resolves to its default after mark-live.
    Defined *target = x->getDefined();
    auto *abs = dyn_cast_or_null<DefinedAbsolute>(target);
    bool absentWeak = isa<Undefined>(x) && abs && abs->getVA() == 0;
    if (!target || isa<DefinedImportThunk>(target) ||
        isa<DefinedImportData>(target) || (abs && !absentWeak))
      continue;
    replaceSymbol<DefinedLocalImport>(cell, ctx, cell->getName(), target);
    LocalImportChunk *c = cast<DefinedLocalImport>(cell)->getChunk();
    c->cell = sc;
    // Mark-live keeps the pointer when something live refers to it.
    c->live = !ctx.config.doGC;
    symtab.localImportChunks.push_back(c);
  }
}

// The cell, among those bound to compiler pointers, whose section s is defined
// in.
static LocalImportChunk *
findCell(const DenseMap<SectionChunk *, LocalImportChunk *> &cellOf,
         Symbol *s) {
  auto *d = dyn_cast_or_null<DefinedRegular>(s);
  return d ? cellOf.lookup(d->getChunk()) : nullptr;
}

// A reference to a local import pointer is rewritten to reach its symbol
// directly where the linker knows the instruction holding it, and a pointer
// only rewritten references read is left out. On x86-64 that is a described
// call, jump or pointer load, decided per reference; on ARM64 it is every adrp
// and 64-bit ldr of a pointer, decided per symbol, since an adrp may serve
// several loads. Every other reference reads the pointer, as do a GC root and
// references from objects that do not describe their sites. Under -import-slots
// a data reference reads the pointer but blocks no rewrite and is not warned.
void bindLocalImports(SymbolTable &symtab) {
  COFFLinkerContext &ctx = symtab.ctx;
  std::vector<Chunk *> &localImportChunks = symtab.localImportChunks;
  if (localImportChunks.empty())
    return;
  // Where no reference can be rewritten, every one reads its pointer, and the
  // undefined symbols' report has named each.
  bool arm64 = symtab.machine == ARM64 && !ctx.hybridSymtab;
  if (!arm64 && llvm::none_of(ctx.objFileInstances, [&](ObjFile *f) {
        return &f->symtab == &symtab && f->describesSites();
      }))
    return;
  llvm::TimeTraceScope timeScope("Bind local imports");
  SmallPtrSet<DefinedLocalImport *, 8> bypassed, read, readByData;
  for (Symbol *b : ctx.config.gcroot)
    if (auto *li = dyn_cast<DefinedLocalImport>(b))
      read.insert(li);

  // The references to report if they still read the pointer, once per file
  // and symbol, with whether they can be rewritten.
  struct Ref {
    ObjFile *file;
    DefinedLocalImport *li;
    bool rewritable;
  };
  std::vector<Ref> refs;

  // Without mark-live, the pointers bound to compiler pointers, keyed by the
  // file holding each. A scan of that file finds whether another name reaches
  // the pointer's section: a local symbol in it (no other file can refer to
  // one), or any external symbol defined in it.
  DenseMap<ObjFile *, TinyPtrVector<LocalImportChunk *>> cellsByFile;
  if (!ctx.config.doGC)
    for (Chunk *c : localImportChunks)
      if (SectionChunk *cell = cast<LocalImportChunk>(c)->cell)
        cellsByFile[cell->file].push_back(cast<LocalImportChunk>(c));
  SmallPtrSet<LocalImportChunk *, 4> named;

  // On ARM64, the adrp and ldr instructions of each pointer, rewritten once
  // every reference to it is known to be.
  struct Arm64Site {
    SectionChunk *sc;
    const coff_relocation *rel;
    DefinedLocalImport *li;
  };
  std::vector<Arm64Site> arm64Sites;

  SmallVector<DefinedLocalImport *, 0> fileImports;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &symtab)
      continue;
    // An object that does not describe its sites reads every local import
    // pointer it refers to. It is scanned only if it lists a pointer bound to a
    // compiler pointer, which is left out unless something live reads it.
    bool describes = file->describesSites() || arm64;
    if (describes) {
      if (llvm::none_of(file->getSymbols(),
                        IsaAndPresentPred<DefinedLocalImport>))
        continue;
    } else {
      fileImports.clear();
      bool listsCell = false;
      for (Symbol *s : file->getSymbols())
        if (auto *li = dyn_cast_or_null<DefinedLocalImport>(s)) {
          fileImports.push_back(li);
          listsCell |= li->getChunk()->cell != nullptr;
        }
      if (!listsCell) {
        read.insert_range(fileImports);
        continue;
      }
    }

    ArrayRef<LocalImportChunk *> cells;
    if (auto it = cellsByFile.find(file); it != cellsByFile.end())
      cells = it->second;
    // The cells by the section that each holds, so a reference finds its cell
    // without a scan; the first cell of a section wins, as the scan did.
    DenseMap<SectionChunk *, LocalImportChunk *> cellOf;
    for (LocalImportChunk *c : cells)
      cellOf.try_emplace(c->cell, c);
    if (!cells.empty())
      for (Symbol *s : file->getSymbols())
        if (LocalImportChunk *c = findCell(cellOf, s);
            c && cast<DefinedRegular>(s)->getCOFFSymbol().isExternal())
          named.insert(c);

    MapVector<DefinedLocalImport *, bool> seen;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live)
        continue;
      bool fromData = ctx.config.importSlots &&
                      !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE);
      for (const coff_relocation &rel : sc->getRelocs()) {
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        if (!isa_and_nonnull<DefinedLocalImport>(s)) {
          if (LocalImportChunk *c = findCell(cellOf, s))
            named.insert(c);
          continue;
        }
        auto *li = cast<DefinedLocalImport>(s);
        if (fromData) {
          readByData.insert(li);
          continue;
        }
        if (!describes) {
          read.insert(li);
          continue;
        }
        bool mismatch = false;
        bool rewritable;
        if (arm64) {
          rewritable = canBypass(file, rel) && isArm64PointerLoad(sc, rel);
          if (rewritable)
            arm64Sites.push_back({sc, &rel, li});
        } else {
          // A call, jump or pointer load through a bypassable local import,
          // with the form's bytes, is rewritten; a jump through a zero-holding
          // pointer keeps reading it.
          std::optional<LinkSiteForm> form;
          if (canBypass(file, rel)) {
            form = getSiteForm(sc, rel);
            if (!form || *form == LinkSiteOther || *form == LinkSiteAddress)
              form.reset();
            else if (!isSiteForm(sc->getContents(), rel.VirtualAddress, *form)) {
              mismatch = true;
              form.reset();
            } else if (isZeroPointer(li) && (*form == LinkSiteJump ||
                                             *form == LinkSiteJumpOnePrefix))
              form.reset();
          }
          rewritable = form.has_value();
          if (form)
            addSiteEdit(sc, rel, *form, li);
        }
        if (mismatch)
          Err(ctx) << file << ": the instruction at offset 0x"
                   << Twine::utohexstr(rel.VirtualAddress) << " in "
                   << sc->getSectionName()
                   << " is not the one its link-only record describes";
        if (rewritable)
          bypassed.insert(li);
        else
          read.insert(li);
        auto [it, inserted] = seen.try_emplace(li, rewritable);
        it->second &= rewritable;
      }
    }
    // A .refptr pointer is the compiler's, not a dllimport; nor is one to a
    // data copy, and a pointer to an absent weak symbol's zero imports nothing.
    // Other objects' references were reported with the undefined symbols.
    if (!defersLocalImportWarning(file, arm64))
      continue;
    for (auto [li, rewritable] : seen) {
      auto *abs = dyn_cast<DefinedAbsolute>(li->getTarget());
      if (!li->getName().starts_with(".refptr.") &&
          !(abs && abs->getVA() == 0) && !li->getChunk()->isCell() &&
          !li->getChunk()->copy)
        refs.push_back({file, li, rewritable});
    }
  }

  // A compiler-made pointer stays a pointer if something live reads it other
  // than through a rewritten instruction, or another name reaches its section.
  for (auto &[file, cells] : cellsByFile)
    for (LocalImportChunk *c : cells)
      c->cell->live = named.contains(c);
  for (SmallPtrSet<DefinedLocalImport *, 8> *set : {&read, &readByData})
    for (DefinedLocalImport *li : *set)
      if (LocalImportChunk *c = li->getChunk(); c->cell && c->live)
        c->cell->live = true;

  for (DefinedLocalImport *li : bypassed) {
    if (read.contains(li))
      continue;
    li->getChunk()->bypassed = true;
    if (!readByData.contains(li))
      li->getChunk()->live = false;
  }
  // An adrp and ldr of a bypassed pointer become adrp and add of its symbol,
  // or each movz of zero.
  for (auto [sc, rel, li] : arm64Sites)
    if (li->getChunk()->bypassed)
      addEdit(sc, {rel->VirtualAddress, 4, 4,
                   isZeroPointer(li) ? RelaxEdit::Arm64Zero
                   : rel->Type == IMAGE_REL_ARM64_PAGEBASE_REL21
                       ? RelaxEdit::Arm64Page
                       : RelaxEdit::Arm64Add,
                   li->getTarget()});
  llvm::erase_if(localImportChunks, [](Chunk *c) {
    auto *li = cast<LocalImportChunk>(c);
    if (li->cell)
      li->live = li->cell->live;
    return !li->live || li->cell;
  });
  // The pointers were made in symbol-table walk order, which depends on the
  // hash function; the image lays them out by their targets' names instead.
  llvm::stable_sort(localImportChunks, [](Chunk *a, Chunk *b) {
    return cast<LocalImportChunk>(a)->getTarget()->getName() <
           cast<LocalImportChunk>(b)->getTarget()->getName();
  });

  if (!ctx.config.warnLocallyDefinedImported)
    return;
  for (const Ref &r : refs)
    if (!r.rewritable || (arm64 && !r.li->getChunk()->bypassed))
      Warn(ctx) << r.file << ": locally defined symbol imported: "
                << symtab.printSymbol(r.li->getTarget()) << " (defined in "
                << r.li->getTarget()->getFile() << ") [LNK4217]";
}
} // namespace lld::coff
