//===- ImportInstructions.cpp - Import instruction checks -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ImportInstructions.h"
#include "Chunks.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lld::coff;

ImportInstructionDecoder::ImportInstructionDecoder()
    : triple("x86_64-pc-windows") {
  InitializeAllDisassemblers();
  std::string error;
  const Target *target = TargetRegistry::lookupTarget(triple, error);
  if (!target)
    return;
  registers.reset(target->createMCRegInfo(triple));
  if (!registers)
    return;
  asmInfo.reset(target->createMCAsmInfo(*registers, triple, MCTargetOptions()));
  subtarget.reset(target->createMCSubtargetInfo(triple, "", ""));
  if (!asmInfo || !subtarget)
    return;
  context = std::make_unique<MCContext>(triple, asmInfo.get(), registers.get(),
                                        subtarget.get());
  decoder.reset(target->createMCDisassembler(*subtarget, *context));
}

ImportInstructionDecoder::~ImportInstructionDecoder() = default;

bool ImportInstructionDecoder::isCandidate(ArrayRef<uint8_t> bytes,
                                           uint32_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4)
    return false;
  if (offset >= 2 && bytes[offset - 2] == 0xff &&
      (bytes[offset - 1] == 0x15 || bytes[offset - 1] == 0x25))
    return true;
  return offset >= 3 &&
         (bytes[offset - 3] == 0x48 || bytes[offset - 3] == 0x4c) &&
         (bytes[offset - 2] == 0x8b || bytes[offset - 2] == 0x8d) &&
         (bytes[offset - 1] & 0xc7) == 0x05;
}

void ImportInstructionDecoder::add(const SectionChunk &chunk,
                                   SmallVector<uint32_t, 0> offsets) {
  if (!decoder)
    return;
  ArrayRef<uint8_t> bytes = chunk.getContents();
  SmallVector<std::pair<uint32_t, uint64_t>, 0> relocations;
  for (const object::coff_relocation &rel : chunk.getRelocs())
    if (unsigned width = chunk.getRelocationWidth(rel))
      relocations.emplace_back(rel.VirtualAddress,
                               uint64_t(rel.VirtualAddress) + width);
  llvm::sort(relocations);
  llvm::sort(offsets);
  offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
  uint64_t position = 0;
  size_t next = 0, kept = 0;
  size_t nextRelocation = 0;
  uint64_t previousEnd = 0;
  while (next < offsets.size() && position < bytes.size()) {
    MCInst instruction;
    uint64_t size = 0;
    if (decoder->getInstruction(instruction, size, bytes.drop_front(position),
                                position, nulls()) != MCDisassembler::Success ||
        !size || size > bytes.size() - position)
      break;
    // The complete instruction must be owned by its one displacement fixup.
    // A second fixup touching an opcode, prefix or operand could invalidate
    // the decoded shape when relocations are applied during parallel output.
    bool overlap = previousEnd > position;
    unsigned count = 0;
    uint32_t relocationOffset = 0;
    while (nextRelocation < relocations.size() &&
           relocations[nextRelocation].first < position + size) {
      auto [start, end] = relocations[nextRelocation++];
      overlap |= start < position || end > position + size;
      previousEnd = std::max(previousEnd, end);
      relocationOffset = start;
      ++count;
    }
    // Only complete six/seven-byte forms are eligible, including LLVM's
    // redundant REX.W on a 64-bit indirect branch. Segment/address prefixes
    // remain ineligible. Do not resynchronize after an invalid instruction:
    // an opaque byte region gives no evidence of a later instruction boundary.
    bool eligible =
        (size == 6 && bytes[position] == 0xff) ||
        (size == 7 && bytes[position] == 0x48 && bytes[position + 1] == 0xff) ||
        (size == 7 && (bytes[position] == 0x48 || bytes[position] == 0x4c) &&
         (bytes[position + 1] == 0x8b || bytes[position + 1] == 0x8d));
    while (next < offsets.size() && offsets[next] < position + size) {
      uint32_t offset = offsets[next++];
      if (eligible && !overlap && count == 1 && relocationOffset == offset &&
          uint64_t(offset) + 4 == position + size &&
          isCandidate(bytes.slice(position, size), size - 4))
        offsets[kept++] = offset;
    }
    position += size;
  }
  offsets.resize(kept);
  if (!offsets.empty())
    sites.try_emplace(chunk.header, std::move(offsets));
}

bool ImportInstructionDecoder::contains(const object::coff_section *section,
                                        uint32_t offset) const {
  auto it = sites.find(section);
  return it != sites.end() && llvm::binary_search(it->second, offset);
}
