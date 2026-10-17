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
#include <utility>
#include <vector>

namespace lld::coff {
class COFFLinkerContext;
class Defined;
class SectionChunk;

// A change the linker makes to the code of a section chunk: the oldSize bytes
// at offset become newSize bytes at newOffset in the chunk as written. Every
// instruction the linker rewrites is one, decided when its reference is bound,
// and so is every alignment and jump that coverage lets it lay out again. An
// edit keeps its length in code the linker does not resize, and takes
// relaxedSize bytes in code it does. The relocations inside an edit are its
// own to write.
struct RelaxEdit {
  enum Kind : uint8_t {
    // call rel32 or jmp rel32 to target. At its old length, a call takes an
    // addr32 prefix and a jump int3 after it, as a jump ends an epilogue only
    // in the forms the unwinder knows.
    DirectCall,
    DirectJump,
    // call [rip+d] or jmp [rip+d] with REX.W, through target, a pointer.
    PointerCall,
    PointerJump,
    // A RIP-relative load whose opcode becomes opcode, mov or lea, of target,
    // plus the constant of fusion, an add whose sum it then takes into the
    // add's register.
    Load,
    // The load or call through a pointer that holds zero, an absent weak
    // symbol's address, which materialises zero instead in the same length.
    ZeroLoad,
    ZeroCall,
    // The last, or the first, relaxedSize bytes of the old ones.
    KeepTail,
    KeepHead,
    // The fused load or add, fusion, that nothing reads: no-operations of its
    // length, or nothing.
    Dispose,
    // The jump that ends the chunk's function, which its target follows.
    TailJump,
    // The access that fusion describes, its memory operand made RIP-relative
    // to target plus its displacement.
    Access,
    // No-operations that align what follows, as coverage describes.
    Align,
    // The jump whose displacement coverage describes: an 8-bit one, with a
    // 32-bit displacement once promoted; or a 32-bit one, with 8 bits until
    // promoted back.
    Branch,
    // On ARM64: adrp of target's page; add of target's offset in its page, or
    // a 64-bit ldr from it, in place of the instruction's opcode; movz of
    // zero.
    Arm64Page,
    Arm64Add,
    Arm64Load,
    Arm64Zero,
  };

  uint32_t offset;
  uint32_t oldSize;
  uint32_t relaxedSize;
  Kind kind;
  Defined *target = nullptr;
  const ObjFile::LinkFusion *fusion = nullptr;
  uint8_t opcode = 0;
  const ObjFile::LinkCoverage *coverage = nullptr;
  uint32_t newOffset = 0;
  uint32_t newSize = 0;
  bool promoted = false;
};

// The changes the linker makes to the code of a section chunk, and how it
// lays the code out where it resizes it.
struct Relaxation {
  // The changes, by offset, none overlapping another.
  std::vector<RelaxEdit> edits;
  // The chunk's size as written, where the linker resizes it.
  uint32_t size = 0;

  // Where the place at offset in the chunk's contents is in the chunk as
  // written. A place inside a change maps into the bytes that replace it.
  uint32_t getOutputOffset(uint32_t offset) const;
  // The change whose bytes include offset, if any.
  const RelaxEdit *findEdit(uint32_t offset) const;
  RelaxEdit *findEdit(uint32_t offset) {
    return const_cast<RelaxEdit *>(std::as_const(*this).findEdit(offset));
  }
};

// Adds e, a change to an instruction of sc, to the changes the linker makes
// there, unless it overlaps one already made, which is an error.
void addEdit(SectionChunk *sc, RelaxEdit e);

// Writes n bytes of no-operations at buf, each at most maxNop bytes long.
void writeNops(uint8_t *buf, size_t n, unsigned maxNop);

// Decides, once every reference is bound, which covered section chunks the
// linker resizes, and how.
void relaxSections(COFFLinkerContext &ctx);

// Leaves out each jump that ends its function, its chunk with its target,
// where the target begins right after the jump's chunk in the layout that
// layOut gives, so that the jump's function runs into it.
void leaveOutTailJumps(
    COFFLinkerContext &ctx,
    llvm::ArrayRef<std::pair<SectionChunk *, Defined *>> jumps,
    llvm::function_ref<void()> layOut);

// Writes sc, whose code the linker changes, at buf.
void writeEdited(const SectionChunk *sc, uint8_t *buf);

// Writes into buf, the contents of sc from its offset begin, each value of
// sc's coverage between begin and end that depends on code the linker
// resizes.
void writeCoverageValues(const SectionChunk *sc, uint8_t *buf, uint32_t begin,
                         uint32_t end);

// How far the place that rel, a relocation in sc, reaches moves relative to
// the relocation's symbol, a definition at value in the chunk target, whose
// code the linker resizes, which the relocation's addend, addend, moves by.
int64_t getAddendShift(const SectionChunk *sc,
                       const llvm::object::coff_relocation &rel,
                       const SectionChunk *target, uint32_t value,
                       int64_t addend);
} // namespace lld::coff

#endif
