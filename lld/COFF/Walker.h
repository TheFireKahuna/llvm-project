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
// address and any other operand. Where the object lists the sites or the
// functions whose address it takes, that answers for a reference to a
// function. Otherwise the instructions of the function that holds the
// reference are decoded: forward from its start where the object is known to
// be a compiler's output, and otherwise by recursive descent from the
// instruction starts that the object's own tables prove. A function is
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
#include "llvm/BinaryFormat/COFF.h"
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
  // What answered, for a message.
  const char *how = ", by its type";
  // The bytes of a decoded site's instruction before its field.
  uint8_t before = 0;
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

// What an x86-64 instruction with an operand in memory does with it.
struct ReadInsn {
  uint8_t size;
  // Whether it reads the memory, other than as a call or jump through it, and
  // the bytes it reads where the operand states them, else 64.
  bool reads;
  uint8_t width;
  // Whether it uses the stack pointer.
  bool stack;
  // The register that a 64-bit mov or lea relative to rip alone writes, and
  // whether it is the lea; else -1.
  int8_t value;
  bool address;
};

// How an x86-64 instruction reads a register that holds a pointer, as an
// object for Windows Itanium or NT-POSIX states a fused site.
struct FusedInsn {
  llvm::COFF::FusedUse use;
  // The register's number and the instruction's length.
  uint8_t reg, size;
  // The bytes that a mov loads from memory into a register and nothing else,
  // 4 or 8, and that register's number; no bytes for any other instruction,
  // though a load that extends a 32-bit word's sign has its register too.
  uint8_t loads, dest;
  // What a lea adds to the register, or the displacement of an access or an
  // index.
  int64_t disp;
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

  // Decodes the instruction at the start of code as a fused site: one that
  // decode admits and that reads a register in one operand alone as the
  // assembler describes a site. With direct, also writes there the
  // instruction of an access with its operand in memory relative to rip, and
  // sets field to the offset in it of the displacement, which holds zero.
  // An index adds two registers, and reads the first of them, or with second
  // the second: its direct form has that register's place taken by a
  // displacement of 32 bits, and the other register's by base where that is
  // a register's number.
  bool decodeFused(ArrayRef<uint8_t> code, FusedInsn &insn,
                   llvm::SmallVectorImpl<char> *direct = nullptr,
                   unsigned *field = nullptr, bool second = false,
                   int base = -1);

  // Decodes the instruction at the start of code as the load of a block
  // from the thread's TLS array, mov r64, [r64 + r64 * 8], or its add to a
  // register. Sets size to its length, index to the number of the register
  // that holds the index, and adds to that of the register an add writes,
  // or -1.
  bool decodeTlsBlock(ArrayRef<uint8_t> code, unsigned &size, unsigned &index,
                      int &adds);

  // Encodes the index at the start of code, which reads the first of its two
  // registers or with second the second, into out with the register that the
  // index at the start of from, read likewise as fromSecond says, adds to
  // it in the other's place.
  bool encodeOnRegisterOf(ArrayRef<uint8_t> code, bool second,
                          ArrayRef<uint8_t> from, bool fromSecond,
                          llvm::SmallVectorImpl<char> &out);

  // Encodes the index at the start of code, which reads the first of its two
  // registers or with second the second, into out with the thread pointer,
  // r14, in the other's place.
  bool encodeOnThreadPointer(ArrayRef<uint8_t> code, bool second,
                             llvm::SmallVectorImpl<char> &out);

  // Decodes the instruction at the start of code as a site that reads
  // memory relative to rip.
  bool decodeRead(ArrayRef<uint8_t> code, ReadInsn &insn);

  // The decoder of the calling thread.
  static X86Decoder *get();

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

// Whether the 32-bit field at offset o of c is that of an x86-64 lea of 64
// bits relative to rip: REX.W, or REX2 of the first opcode map with W, then
// 8D with a RIP-relative ModRM.
bool isRipLea64(ArrayRef<uint8_t> c, size_t o);
} // namespace lld::coff

#endif
