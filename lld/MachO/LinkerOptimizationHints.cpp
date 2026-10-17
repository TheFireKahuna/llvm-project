//===- LinkerOptimizationHints.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LinkerOptimizationHints.h"

#include "Arch/ARM64Common.h"
#include "lld/Common/AArch64Instructions.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::aarch64;
using namespace lld::macho;

// Transforms a pair of adrp+add instructions into an adr instruction if the
// target is within the +/- 1 MiB range allowed by the adr's 21 bit signed
// immediate offset.
//
//   adrp xN, _foo@PAGE
//   add  xM, xN, _foo@PAGEOFF
// ->
//   adr  xM, _foo
//   nop
static bool applyAdrpAdd(uint8_t *buf, const ConcatInputSection *isec,
                         uint64_t offset1, uint64_t offset2) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  Adrp adrp;
  Add add;
  if (!parseAdrp(ins1, adrp) || !parseAdd(ins2, add))
    return false;
  if (adrp.destRegister != add.srcRegister)
    return false;

  uint64_t addr1 = isec->getVA() + offset1;
  uint64_t referent = lld::macho::pageBits(addr1) + adrp.addend + add.addend;
  int64_t delta = referent - addr1;
  if (!isValidAdrOffset(delta))
    return false;

  writeAdr(buf + offset1, add.destRegister, delta);
  writeNop(buf + offset2);
  return true;
}

// Transforms two adrp instructions into a single adrp if their referent
// addresses are located on the same 4096 byte page.
//
//   adrp xN, _foo@PAGE
//   adrp xN, _bar@PAGE
// ->
//   adrp xN, _foo@PAGE
//   nop
static void applyAdrpAdrp(uint8_t *buf, const ConcatInputSection *isec,
                          uint64_t offset1, uint64_t offset2) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  Adrp adrp1, adrp2;
  if (!parseAdrp(ins1, adrp1) || !parseAdrp(ins2, adrp2))
    return;
  if (adrp1.destRegister != adrp2.destRegister)
    return;

  uint64_t page1 = pageBits(offset1 + isec->getVA()) + adrp1.addend;
  uint64_t page2 = pageBits(offset2 + isec->getVA()) + adrp2.addend;
  if (page1 != page2)
    return;

  writeNop(buf + offset2);
}

// Transforms a pair of adrp+ldr (immediate) instructions into an ldr (literal)
// load from a PC-relative address if it is 4-byte aligned and within +/- 1 MiB,
// as ldr can encode a signed 19-bit offset that gets multiplied by 4.
//
//   adrp xN, _foo@PAGE
//   ldr  xM, [xN, _foo@PAGEOFF]
// ->
//   nop
//   ldr  xM, _foo
static void applyAdrpLdr(uint8_t *buf, const ConcatInputSection *isec,
                         uint64_t offset1, uint64_t offset2) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  Adrp adrp;
  Ldr ldr;
  if (!parseAdrp(ins1, adrp) || !parseLdr(ins2, ldr))
    return;
  if (adrp.destRegister != ldr.baseRegister)
    return;

  uint64_t addr1 = isec->getVA() + offset1;
  uint64_t addr2 = isec->getVA() + offset2;
  uint64_t referent = pageBits(addr1) + adrp.addend + ldr.offset;
  ldr.offset = referent - addr2;
  if (!isLiteralLdrEligible(ldr))
    return;

  writeNop(buf + offset1);
  writeLiteralLdr(buf + offset2, ldr);
}

// GOT loads are emitted by the compiler as a pair of adrp and ldr instructions,
// but they may be changed to adrp+add by relaxGotLoad(). This hint performs
// the AdrpLdr or AdrpAdd transformation depending on whether it was relaxed.
static void applyAdrpLdrGot(uint8_t *buf, const ConcatInputSection *isec,
                            uint64_t offset1, uint64_t offset2) {
  uint32_t ins2 = read32le(buf + offset2);
  Add add;
  Ldr ldr;
  if (parseAdd(ins2, add))
    applyAdrpAdd(buf, isec, offset1, offset2);
  else if (parseLdr(ins2, ldr))
    applyAdrpLdr(buf, isec, offset1, offset2);
}

// Optimizes an adrp+add+ldr sequence used for loading from a local symbol's
// address by loading directly if it's close enough, or to an adrp(p)+ldr
// sequence if it's not.
//
//   adrp x0, _foo@PAGE
//   add  x1, x0, _foo@PAGEOFF
//   ldr  x2, [x1, #off]
static void applyAdrpAddLdr(uint8_t *buf, const ConcatInputSection *isec,
                            uint64_t offset1, uint64_t offset2,
                            uint64_t offset3) {
  uint32_t ins1 = read32le(buf + offset1);
  uint32_t ins2 = read32le(buf + offset2);
  uint32_t ins3 = read32le(buf + offset3);
  Adrp adrp;
  Add add;
  Ldr ldr;
  if (!parseAdrp(ins1, adrp) || !parseAdd(ins2, add) || !parseLdr(ins3, ldr))
    return;
  if (adrp.destRegister != add.srcRegister)
    return;
  if (add.destRegister != ldr.baseRegister)
    return;

  // Load from the target address directly.
  //   nop
  //   nop
  //   ldr x2, [_foo + #off]
  uint64_t addr1 = isec->getVA() + offset1;
  uint64_t addr3 = isec->getVA() + offset3;
  uint64_t referent = pageBits(addr1) + adrp.addend + add.addend;
  Ldr literalLdr = ldr;
  literalLdr.offset += referent - addr3;
  if (isLiteralLdrEligible(literalLdr)) {
    writeNop(buf + offset1);
    writeNop(buf + offset2);
    writeLiteralLdr(buf + offset3, literalLdr);
    return;
  }

  if (applyAdrpAdd(buf, isec, offset1, offset2))
    return;

  // Move the target's page offset into the ldr's immediate offset.
  //   adrp x0, _foo@PAGE
  //   nop
  //   ldr x2, [x0, _foo@PAGEOFF + #off]
  Ldr immediateLdr = ldr;
  immediateLdr.baseRegister = adrp.destRegister;
  immediateLdr.offset += add.addend;
  if (isImmediateLdrEligible(immediateLdr)) {
    writeNop(buf + offset2);
    writeImmediateLdr(buf + offset3, immediateLdr);
    return;
  }
}

// Relaxes a GOT-indirect load.
// If the referenced symbol is external and its GOT entry is within +/- 1 MiB,
// the GOT entry can be loaded with a single literal ldr instruction.
// If the referenced symbol is local and thus has been relaxed to adrp+add+ldr,
// we perform the AdrpAddLdr transformation.
static void applyAdrpLdrGotLdr(uint8_t *buf, const ConcatInputSection *isec,
                               uint64_t offset1, uint64_t offset2,
                               uint64_t offset3) {
  uint32_t ins2 = read32le(buf + offset2);
  Add add;
  Ldr ldr2;

  if (parseAdd(ins2, add)) {
    applyAdrpAddLdr(buf, isec, offset1, offset2, offset3);
  } else if (parseLdr(ins2, ldr2)) {
    // adrp x1, _foo@GOTPAGE
    // ldr  x2, [x1, _foo@GOTPAGEOFF]
    // ldr  x3, [x2, #off]
    uint32_t ins3 = read32le(buf + offset3);
    Ldr ldr3;
    if (!parseLdr(ins3, ldr3))
      return;
    if (ldr3.baseRegister != ldr2.destRegister)
      return;
    applyAdrpLdr(buf, isec, offset1, offset2);
  }
}

template <typename Callback>
static void forEachHint(ArrayRef<uint8_t> data, Callback callback) {
  std::array<uint64_t, 3> args;

  auto readNext = [&]() -> uint64_t {
    unsigned int n = 0;
    uint64_t value = decodeULEB128(data.data(), &n, data.end());
    data = data.drop_front(n);
    return value;
  };

  while (!data.empty()) {
    uint64_t type = readNext();
    if (type == 0)
      break;

    uint64_t argCount = readNext();
    for (unsigned i = 0; i < argCount; ++i) {
      uint64_t arg = readNext();
      if (i < 3)
        args[i] = arg;
    }
    // All known LOH types as of 2022-09 have 3 or fewer arguments; skip others.
    if (argCount > 3)
      continue;
    callback(type, ArrayRef(args.data(), argCount));
  }
}

// On RISC architectures like arm64, materializing a memory address generally
// takes multiple instructions. If the referenced symbol is located close enough
// in memory, fewer instructions are needed.
//
// Linker optimization hints record where addresses are computed. After
// addresses have been assigned, if possible, we change them to a shorter
// sequence of instructions. The size of the binary is not modified; the
// eliminated instructions are replaced with NOPs. This still leads to faster
// code as the CPU can skip over NOPs quickly.
//
// LOHs are specified by the LC_LINKER_OPTIMIZATION_HINTS load command, which
// points to a sequence of ULEB128-encoded numbers. Each entry specifies a
// transformation kind, and 2 or 3 addresses where the instructions are located.
void macho::applyOptimizationHints(uint8_t *outBuf, const ObjFile &obj) {
  ArrayRef<uint8_t> data = obj.getOptimizationHints();
  if (data.empty())
    return;

  const ConcatInputSection *section = nullptr;
  uint64_t sectionAddr = 0;
  uint8_t *buf = nullptr;

  auto findSection = [&](uint64_t addr) {
    if (section && addr >= sectionAddr &&
        addr < sectionAddr + section->getSize())
      return true;

    if (obj.sections.empty())
      return false;
    auto secIt = std::prev(llvm::upper_bound(
        obj.sections, addr,
        [](uint64_t off, const Section *sec) { return off < sec->addr; }));
    const Section *sec = *secIt;

    if (sec->subsections.empty())
      return false;
    auto subsecIt = std::prev(llvm::upper_bound(
        sec->subsections, addr - sec->addr,
        [](uint64_t off, Subsection subsec) { return off < subsec.offset; }));
    const Subsection &subsec = *subsecIt;
    const ConcatInputSection *isec =
        dyn_cast_or_null<ConcatInputSection>(subsec.isec);
    if (!isec || isec->shouldOmitFromOutput())
      return false;

    section = isec;
    sectionAddr = subsec.offset + sec->addr;
    buf = outBuf + section->outSecOff + section->parent->fileOff;
    return true;
  };

  auto isValidOffset = [&](uint64_t offset) {
    if (offset < sectionAddr || offset >= sectionAddr + section->getSize()) {
      error(toString(&obj) +
            ": linker optimization hint spans multiple sections");
      return false;
    }
    return true;
  };

  bool hasAdrpAdrp = false;
  forEachHint(data, [&](uint64_t kind, ArrayRef<uint64_t> args) {
    if (kind == LOH_ARM64_ADRP_ADRP) {
      hasAdrpAdrp = true;
      return;
    }

    if (!findSection(args[0]))
      return;
    switch (kind) {
    case LOH_ARM64_ADRP_ADD:
      if (isValidOffset(args[1]))
        applyAdrpAdd(buf, section, args[0] - sectionAddr,
                     args[1] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_LDR:
      if (isValidOffset(args[1]))
        applyAdrpLdr(buf, section, args[0] - sectionAddr,
                     args[1] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_LDR_GOT:
      if (isValidOffset(args[1]))
        applyAdrpLdrGot(buf, section, args[0] - sectionAddr,
                        args[1] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_ADD_LDR:
      if (isValidOffset(args[1]) && isValidOffset(args[2]))
        applyAdrpAddLdr(buf, section, args[0] - sectionAddr,
                        args[1] - sectionAddr, args[2] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_LDR_GOT_LDR:
      if (isValidOffset(args[1]) && isValidOffset(args[2]))
        applyAdrpLdrGotLdr(buf, section, args[0] - sectionAddr,
                           args[1] - sectionAddr, args[2] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_ADD_STR:
    case LOH_ARM64_ADRP_LDR_GOT_STR:
      // TODO: Implement these
      break;
    }
  });

  if (!hasAdrpAdrp)
    return;

  // AdrpAdrp optimization hints are performed in a second pass because they
  // might interfere with other transformations. For instance, consider the
  // following input:
  //
  //   adrp x0, _foo@PAGE
  //   add  x1, x0, _foo@PAGEOFF
  //   adrp x0, _bar@PAGE
  //   add  x2, x0, _bar@PAGEOFF
  //
  // If we perform the AdrpAdrp relaxation first, we get:
  //
  //   adrp x0, _foo@PAGE
  //   add  x1, x0, _foo@PAGEOFF
  //   nop
  //   add x2, x0, _bar@PAGEOFF
  //
  // If we then apply AdrpAdd to the first two instructions, the add will have a
  // garbage value in x0:
  //
  //   adr  x1, _foo
  //   nop
  //   nop
  //   add  x2, x0, _bar@PAGEOFF
  forEachHint(data, [&](uint64_t kind, ArrayRef<uint64_t> args) {
    if (kind != LOH_ARM64_ADRP_ADRP)
      return;
    if (!findSection(args[0]))
      return;
    if (isValidOffset(args[1]))
      applyAdrpAdrp(buf, section, args[0] - sectionAddr, args[1] - sectionAddr);
  });
}
