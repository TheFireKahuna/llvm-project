//===- Walker.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Classifies the references of code that an object does not type. An x86-64
// object of another producer has one relocation type, REL32, for a branch, an
// address and any other operand, so the instructions of the function that
// holds the reference are decoded: forward from its start where the object is
// known to be a compiler's output, and otherwise by recursive descent from
// the instruction starts that the object's own tables prove. A function is
// accepted only if the decode explains each of its bytes and relocations.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_WALKER_H
#define LLD_COFF_WALKER_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace llvm::object {
struct coff_relocation;
}

namespace lld::coff {
class ObjFile;
class SectionChunk;

// What the instruction at a relocation of code does with the symbol.
enum class SiteKind : uint8_t {
  // Calls it or jumps to it.
  Branch,
  // Takes its address where a load of the address fits: an x86-64 lea of 64
  // bits relative to rip, or an ARM64 adrp or add.
  Address,
  // Uses it as another operand.
  Operand,
  // Nothing: the relocation is in bytes that the object marks as data.
  Data,
  // Not known.
  Refused,
};

struct Site {
  Site(SiteKind kind = SiteKind::Refused, const char *reason = nullptr)
      : kind(kind), reason(reason) {}
  SiteKind kind;
  // Why a site is refused.
  const char *reason;
  // The function-typed symbol at or before the site, if the object has one.
  StringRef function;
  // Whether the function was decoded forward from its start, on the identity
  // of the object's producer, and not by descent from proven starts.
  bool linear = false;
};

// A field of an x86-64 instruction that a relocation may fill: an immediate,
// a displacement, or a branch's offset.
struct InsnField {
  uint8_t offset, size;
  bool branch;
  // A displacement from the address of the next instruction.
  bool pcRelative;
};

struct Insn {
  uint8_t size;
  // Whether execution does not continue at the next instruction.
  bool ends;
  // The target of a direct branch or call.
  std::optional<uint64_t> target;
  SmallVector<InsnField, 4> fields;
};

// LLVM's x86-64 disassembler, restricted to the encodings whose length and
// fields are the same on every processor and that survive being encoded
// again.
class X86Decoder {
public:
  // Null where this linker was built without the x86 target.
  static std::unique_ptr<X86Decoder> create();
  ~X86Decoder();

  // Decodes the instruction at the start of code, which is at address.
  // Returns false for an encoding that is undefined or not admitted.
  bool decode(ArrayRef<uint8_t> code, uint64_t address, Insn &insn);

private:
  struct Impl;
  X86Decoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};

class Walker {
public:
  Walker();
  ~Walker();

  // Classifies rel, a relocation of the code section sc of an object that
  // does not type its sites: an x86-64 REL32, or any ARM64 relocation, which
  // its type classifies. The function that holds an x86-64 site is decoded on
  // the first query for it and the result kept for the object. Queries for
  // one object must not run concurrently.
  Site classify(const SectionChunk &sc,
                const llvm::object::coff_relocation &rel);

  struct Object;

private:
  std::mutex mu;
  llvm::DenseMap<const ObjFile *, std::unique_ptr<Object>> objects;
  std::vector<std::unique_ptr<X86Decoder>> decoders;
};
} // namespace lld::coff

#endif
