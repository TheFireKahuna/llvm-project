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

using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::aarch64;
using namespace lld::macho;

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
        applyAdrpAdd(buf, section->getVA(), args[0] - sectionAddr,
                     args[1] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_LDR:
      if (isValidOffset(args[1]))
        applyAdrpLdr(buf, section->getVA(), args[0] - sectionAddr,
                     args[1] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_LDR_GOT:
      if (isValidOffset(args[1]))
        applyAdrpLdrGot(buf, section->getVA(), args[0] - sectionAddr,
                        args[1] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_ADD_LDR:
      if (isValidOffset(args[1]) && isValidOffset(args[2]))
        applyAdrpAddLdr(buf, section->getVA(), args[0] - sectionAddr,
                        args[1] - sectionAddr, args[2] - sectionAddr);
      break;
    case LOH_ARM64_ADRP_LDR_GOT_LDR:
      if (isValidOffset(args[1]) && isValidOffset(args[2]))
        applyAdrpLdrGotLdr(buf, section->getVA(), args[0] - sectionAddr,
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
      applyAdrpAdrp(buf, section->getVA(), args[0] - sectionAddr,
                    args[1] - sectionAddr);
  });
}
