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
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace lld::coff {
class COFFLinkerContext;
class Defined;
class SectionChunk;

// A change the linker makes to a section chunk's code: oldSize bytes at offset
// become newSize bytes at newOffset as written. Covers rewritten instructions
// and the alignments and jumps coverage lets it re-lay-out; takes relaxedSize
// bytes where the chunk is resized, else its own length, and writes its relocs.
struct RelaxEdit {
  enum Kind : uint8_t {
    // call rel32 or jmp rel32 to target; at old length a call gets an addr32
    // prefix, a jump a trailing int3 (a jump ends an epilogue only so).
    DirectCall,
    DirectJump,
    // call [rip+d] or jmp [rip+d] with REX.W, through target, a pointer.
    PointerCall,
    PointerJump,
    // A RIP-relative mov or lea of target in place of the instruction's opcode,
    // plus fusion's constant, an add whose sum it takes into that register.
    LoadPointer,
    LoadAddress,
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
    // The access of a thread-local variable's record that fusion describes, a
    // load of a word the linker knows: of the address of target, the TLS
    // index, made lea of it; of target's offset in its section, made a move
    // of the offset.
    AccessAddress,
    AccessOffset,
    // The memory operand that fusion describes, which added target's offset
    // in its section from a register: the register left out and the offset
    // added to the displacement.
    Index,
    // A load of a pointer of an image's data that becomes lea of base plus
    // delta, or on ARM64 an add of delta to base, base holding the pointer of
    // other data of that image.
    Anchor,
    // No-operations that align what follows, as coverage describes.
    Align,
    // The jump whose displacement coverage describes: 8-bit, widened to 32 when
    // promoted; or 32-bit, held at 8 bits until promoted back.
    Branch,
    // ARM64: adrp of target's page; add of its offset, or a 64-bit ldr from it,
    // in place of the opcode; movz of zero.
    Arm64Page,
    Arm64Add,
    Arm64Load,
    Arm64Zero,
  };

  RelaxEdit(uint32_t offset, uint16_t oldSize, uint16_t relaxedSize, Kind kind,
            Defined *target = nullptr,
            const ObjFile::LinkFusion *fusion = nullptr)
      : offset(offset), oldSize(oldSize), relaxedSize(relaxedSize), kind(kind),
        target(target), entry(fusion) {}

  const ObjFile::LinkFusion *fusion() const {
    return llvm::dyn_cast_if_present<const ObjFile::LinkFusion *>(entry);
  }
  const ObjFile::LinkCoverage *coverage() const {
    return llvm::dyn_cast_if_present<const ObjFile::LinkCoverage *>(entry);
  }

  uint32_t offset;
  uint32_t newOffset = 0;
  uint16_t oldSize;
  uint16_t relaxedSize;
  uint16_t newSize = 0;
  Kind kind;
  // For an anchor, the register numbered as encoded and the distance.
  uint8_t base = 0;
  int32_t delta = 0;
  Defined *target;
  // The record entry the edit writes, as its kind says.
  llvm::PointerUnion<const ObjFile::LinkFusion *, const ObjFile::LinkCoverage *>
      entry;
};

// The changes to a section chunk's code, and how it is laid out where resized.
struct Relaxation {
  // The changes, by offset, none overlapping another.
  std::vector<RelaxEdit> edits;
  // The chunk's size as written, where the linker resizes it.
  uint32_t size = 0;

  // Where offset in the chunk's contents lands in the chunk as written; a place
  // inside a change maps into the bytes that replace it.
  uint32_t getOutputOffset(uint32_t offset) const;
  // The change whose bytes include offset, if any.
  const RelaxEdit *findEdit(uint32_t offset) const;
  RelaxEdit *findEdit(uint32_t offset) {
    return const_cast<RelaxEdit *>(std::as_const(*this).findEdit(offset));
  }
};

// Adds edit e to sc's changes; an overlap with an existing one is an error.
void addEdit(SectionChunk *sc, RelaxEdit e);

// The offset of d in its output section, which a section-relative relocation
// to it in sc holds.
uint32_t getSectionOffset(const SectionChunk *sc, Defined *d);

// Writes n bytes of no-operations at buf, each at most maxNop bytes long.
void writeNops(uint8_t *buf, size_t n, unsigned maxNop);

// Decides, once references are bound, which covered chunks to resize and how.
void relaxSections(COFFLinkerContext &ctx);

// Leaves out each function-ending jump whose target, in the layout layOut
// gives, begins right after the jump's chunk, so the function runs into it.
void leaveOutTailJumps(
    COFFLinkerContext &ctx,
    llvm::ArrayRef<std::pair<SectionChunk *, Defined *>> jumps,
    llvm::function_ref<void()> layOut);

// Writes sc, whose code the linker changes, at buf.
void writeEdited(const SectionChunk *sc, uint8_t *buf);

// Writes, into buf holding sc's contents from offset begin, each coverage value
// between begin and end that depends on code the linker resizes.
void writeCoverageValues(const SectionChunk *sc, uint8_t *buf, uint32_t begin,
                         uint32_t end);

// How far rel's place (in sc) moves relative to its symbol, defined at value in
// target, when the linker resizes target and the addend moves the place in it.
int64_t getAddendShift(const SectionChunk *sc,
                       const llvm::object::coff_relocation &rel,
                       const SectionChunk *target, uint32_t value,
                       int64_t addend);
} // namespace lld::coff

#endif
