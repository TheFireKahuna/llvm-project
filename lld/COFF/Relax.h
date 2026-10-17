//===- Relax.h --------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_RELAX_H
#define LLD_COFF_RELAX_H

#include "InputFiles.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include <cstdint>
#include <tuple>
#include <vector>

namespace lld::coff {
class COFFLinkerContext;
class Defined;
class SectionChunk;

// A change the linker makes to the code of a covered section chunk: the
// bytes from offset, oldSize of them, become newSize bytes at newOffset in
// the chunk as written.
struct RelaxEdit {
  enum Kind : uint8_t {
    // call rel32 or jmp rel32 to target.
    DirectCall,
    DirectJump,
    // call [rip+d] or jmp [rip+d] with REX.W, through target, a pointer.
    PointerCall,
    PointerJump,
    // The last, or the first, newSize bytes of the old ones.
    KeepTail,
    KeepHead,
    // Nothing.
    Delete,
    // The access that fusion describes, its memory operand made RIP-relative
    // to target plus its displacement.
    Access,
    // No-operations that align what follows, as coverage describes.
    Align,
    // The jump whose displacement coverage describes: an 8-bit one, with a
    // 32-bit displacement once promoted; or a 32-bit one, with 8 bits until
    // promoted back.
    Branch,
  };

  uint32_t offset;
  uint32_t oldSize;
  uint32_t newSize;
  uint32_t newOffset = 0;
  Kind kind;
  bool promoted = false;
  Defined *target = nullptr;
  const ObjFile::LinkFusion *fusion = nullptr;
  const ObjFile::LinkCoverage *coverage = nullptr;
};

// How the linker lays out the code of a covered section chunk in which it
// changes the size of instructions.
struct Relaxation {
  // The changes, by offset, none overlapping another.
  std::vector<RelaxEdit> edits;
  // The chunk's size as written.
  uint32_t size = 0;

  // Where the place at offset in the chunk's contents is in the chunk as
  // written. A place inside a change maps into the bytes that replace it.
  uint32_t getOutputOffset(uint32_t offset) const;
  // The change whose bytes include offset, if any.
  const RelaxEdit *findEdit(uint32_t offset) const;
};

// Writes n bytes of no-operations at buf, each at most maxNop bytes long.
void writeNops(uint8_t *buf, size_t n, unsigned maxNop);

// Decides, after identical code folding, which covered section chunks the
// linker resizes, and how.
void relaxSections(COFFLinkerContext &ctx);

// Leaves out each jump that ends its function, its chunk with the offset of
// its displacement and its target, where the target begins right after the
// jump's chunk in the layout that layOut gives, so that the jump's function
// runs into it.
void leaveOutTailJumps(
    COFFLinkerContext &ctx,
    llvm::ArrayRef<std::tuple<SectionChunk *, uint32_t, Defined *>> jumps,
    llvm::function_ref<void()> layOut);

// Writes sc, whose code the linker resizes, at buf.
void writeRelaxed(const SectionChunk *sc, uint8_t *buf);

// Writes into buf, the contents of sc from its offset begin, each value of
// sc's coverage between begin and end that depends on code the linker
// resizes.
void writeCoverageValues(const SectionChunk *sc, uint8_t *buf, uint32_t begin,
                         uint32_t end);

// How far the place that a relocation in sc at offset reaches moves relative
// to the relocation's symbol, a definition at value in the chunk target,
// whose code the linker resizes, which the relocation's addend, addend, moves
// by.
int64_t getAddendShift(const SectionChunk *sc, uint32_t offset,
                       const SectionChunk *target, uint32_t value,
                       int64_t addend);
} // namespace lld::coff

#endif
