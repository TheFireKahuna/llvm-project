//===-- llvm/MC/MCFixup.h - Instruction Relocation and Patching -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCFIXUP_H
#define LLVM_MC_MCFIXUP_H

#include "llvm/Support/Compiler.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SMLoc.h"
#include <cassert>

namespace llvm {
class MCExpr;

/// Extensible enumeration to represent the type of a fixup.
using MCFixupKind = uint16_t;
enum {
  // [0, FirstLiteralRelocationKind) encodes raw relocation types.

  // [FirstLiteralRelocationKind, FK_NONE) encodes raw relocation types coming
  // from .reloc directives. Fixup kind
  // FirstLiteralRelocationKind+t encodes relocation type t.
  FirstLiteralRelocationKind = 2000,

  // Other kinds indicate the fixup may resolve to a constant, allowing the
  // assembler to update the instruction or data directly without a relocation.
  FK_NONE = 4000, ///< A no-op fixup.
  FK_Data_1,      ///< A one-byte fixup.
  FK_Data_2,      ///< A two-byte fixup.
  FK_Data_4,      ///< A four-byte fixup.
  FK_Data_8,      ///< A eight-byte fixup.
  FK_Data_leb128, ///< A leb128 fixup.
  FK_SecRel_1,    ///< A one-byte section relative fixup.
  FK_SecRel_2,    ///< A two-byte section relative fixup.
  FK_SecRel_4,    ///< A four-byte section relative fixup.
  FK_SecRel_8,    ///< A eight-byte section relative fixup.

  FirstTargetFixupKind,
};

/// How an instruction uses the address a fixup gives it, where the code
/// emitter knows. Object writers that describe instructions to the linker read
/// it.
enum class MCFixupUse : uint8_t {
  Unknown, ///< Not described.
  Call,    ///< An indirect call through the pointer at the address.
  Jump,    ///< An indirect jump through the pointer at the address.
  Load,    ///< A load of the pointer at the address into a register.
  Address, ///< The address itself, computed into a register.
};

/// Encode information on a single operation to perform on a byte
/// sequence (e.g., an encoded instruction) which requires assemble- or run-
/// time patching.
///
/// Fixups are used any time the target instruction encoder needs to represent
/// some value in an instruction which is not yet concrete. The encoder will
/// encode the instruction assuming the value is 0, and emit a fixup which
/// communicates to the assembler backend how it should rewrite the encoded
/// value.
///
/// During the process of relaxation, the assembler will apply fixups as
/// symbolic values become concrete. When relaxation is complete, any remaining
/// fixups become relocations in the object file (or errors, if the fixup cannot
/// be encoded on the target).
class MCFixup {
  /// The value to put into the fixup location. The exact interpretation of the
  /// expression is target dependent, usually it will be one of the operands to
  /// an instruction or an assembler directive.
  const MCExpr *Value = nullptr;

  /// The byte index of start of the relocation inside the MCFragment.
  uint32_t Offset = 0;

  /// The target dependent kind of fixup item this is. The kind is used to
  /// determine how the operand value should be encoded into the instruction.
  MCFixupKind Kind = FK_NONE;

  /// True if this is a PC-relative fixup. The relocatable expression is
  /// typically resolved When SymB is nullptr and SymA is a local symbol defined
  /// within the current section.
  bool PCRel : 1;

  /// Used by RISC-V style linker relaxation. Whether the fixup is
  /// linker-relaxable.
  bool LinkerRelaxable : 1;

  /// How the instruction holding the fixup uses its address. The bit-fields
  /// share one underlying type size so that the Microsoft layout packs them
  /// into the bytes that follow Kind.
  uint8_t Use : 3;

  /// The fixup's offset from the start of its instruction, recorded with the
  /// use.
  uint8_t InstOffset : 4;

public:
  MCFixup() : PCRel(false), LinkerRelaxable(false), Use(0), InstOffset(0) {}

  static MCFixup create(uint32_t Offset, const MCExpr *Value, MCFixupKind Kind,
                        bool PCRel = false) {
    MCFixup FI;
    FI.Value = Value;
    FI.Offset = Offset;
    FI.Kind = Kind;
    FI.PCRel = PCRel;
    return FI;
  }

  MCFixupKind getKind() const { return Kind; }

  uint32_t getOffset() const { return Offset; }
  void setOffset(uint32_t Value) { Offset = Value; }

  const MCExpr *getValue() const { return Value; }

  bool isPCRel() const { return PCRel; }
  void setPCRel() { PCRel = true; }
  bool isLinkerRelaxable() const { return LinkerRelaxable; }
  void setLinkerRelaxable() { LinkerRelaxable = true; }
  MCFixupUse getUse() const { return static_cast<MCFixupUse>(Use); }
  unsigned getInstOffset() const { return InstOffset; }
  /// Records how the instruction uses the address, and where the fixup lies
  /// in the instruction.
  void setUse(MCFixupUse U, unsigned Offset) {
    assert(Offset < 16 && "an instruction is at most 15 bytes");
    Use = static_cast<uint8_t>(U);
    InstOffset = Offset;
  }

  /// Return the generic fixup kind for a value with the given size. It
  /// is an error to pass an unsupported size.
  static MCFixupKind getDataKindForSize(unsigned Size) {
    switch (Size) {
    default: llvm_unreachable("Invalid generic fixup size!");
    case 1:
      return FK_Data_1;
    case 2:
      return FK_Data_2;
    case 4:
      return FK_Data_4;
    case 8:
      return FK_Data_8;
    }
  }

  LLVM_ABI SMLoc getLoc() const;
};

// Every instruction and data directive with a relocatable operand holds one,
// so the flags stay within the padding after Kind.
static_assert(sizeof(MCFixup) == sizeof(void *) + 8, "MCFixup grew");

namespace mc {
// Check if the fixup kind is a relocation type. Return false if the fixup can
// be resolved without a relocation.
inline bool isRelocation(MCFixupKind FixupKind) { return FixupKind < FK_NONE; }

// Check if the fixup kind represents a relocation type from a .reloc directive.
// In ELF, this skips STT_SECTION adjustment and STT_TLS symbol type setting for
// TLS relocations.
inline bool isRelocRelocation(MCFixupKind FixupKind) {
  return FirstLiteralRelocationKind <= FixupKind && FixupKind < FK_NONE;
}
} // namespace mc

} // End llvm namespace

#endif
