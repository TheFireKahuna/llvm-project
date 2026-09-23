//===- ImportInstructions.h - Import instruction checks ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_IMPORTINSTRUCTIONS_H
#define LLD_COFF_IMPORTINSTRUCTIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/TargetParser/Triple.h"
#include <memory>

namespace llvm {
class MCAsmInfo;
class MCContext;
class MCDisassembler;
class MCRegisterInfo;
class MCSubtargetInfo;
namespace object {
struct coff_section;
}
} // namespace llvm

namespace lld::coff {
class SectionChunk;

// One decoder per link, used while reading objects, before parallel output.
// Byte-pattern recognition alone cannot distinguish an instruction from bytes
// inside another instruction or account for a preceding segment/address prefix.
class ImportInstructionDecoder {
  llvm::Triple triple;
  std::unique_ptr<llvm::MCRegisterInfo> registers;
  std::unique_ptr<llvm::MCAsmInfo> asmInfo;
  std::unique_ptr<llvm::MCSubtargetInfo> subtarget;
  std::unique_ptr<llvm::MCContext> context;
  std::unique_ptr<llvm::MCDisassembler> decoder;
  // Sparse link-only results; do not enlarge every ObjFile or SectionChunk.
  // Output views can clone a contribution. Its source header and instruction
  // coordinates stay shared through the current size-preserving rewrites.
  llvm::DenseMap<const llvm::object::coff_section *,
                 llvm::SmallVector<uint32_t, 0>>
      sites;

public:
  ImportInstructionDecoder();
  ~ImportInstructionDecoder();

  static bool isCandidate(llvm::ArrayRef<uint8_t> bytes, uint32_t offset);
  void add(const SectionChunk &chunk, llvm::SmallVector<uint32_t, 0> offsets);
  bool contains(const llvm::object::coff_section *section,
                uint32_t offset) const;
};

} // namespace lld::coff

#endif
