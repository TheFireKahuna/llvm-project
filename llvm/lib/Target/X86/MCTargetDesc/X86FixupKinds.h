//===-- X86FixupKinds.h - X86 Specific Fixup Entries ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_MCTARGETDESC_X86FIXUPKINDS_H
#define LLVM_LIB_TARGET_X86_MCTARGETDESC_X86FIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace X86 {
enum Fixups {
  reloc_riprel_4byte = FirstTargetFixupKind, // 32-bit rip-relative
  reloc_riprel_4byte_movq_load,              // 32-bit rip-relative in movq
  reloc_riprel_4byte_movq_load_rex2,         // 32-bit rip-relative in movq
                                             // with rex2 prefix
  reloc_riprel_4byte_relax,                  // 32-bit rip-relative in relaxable
                                             // instruction
  reloc_riprel_4byte_relax_rex,              // 32-bit rip-relative in relaxable
                                             // instruction with rex prefix
  reloc_riprel_4byte_relax_rex2,             // 32-bit rip-relative in relaxable
                                             // instruction with rex2 prefix
  reloc_riprel_4byte_relax_evex,             // 32-bit rip-relative in relaxable
                                             // instruction of APX NDD/NF with
                                             // EVEX prefix
  reloc_signed_4byte,                        // 32-bit signed. Unlike FK_Data_4
                                             // this will be sign extended at
                                             // runtime.
  reloc_signed_4byte_relax,                  // like reloc_signed_4byte, but
                                             // in a relaxable instruction.
  reloc_global_offset_table,                 // 32-bit, relative to the start
                                             // of the instruction. Used only
                                             // for _GLOBAL_OFFSET_TABLE_.
  reloc_branch_4byte_pcrel,                  // 32-bit PC relative branch.
  reloc_riprel_4byte_imm1,                   // 32-bit rip-relative followed
  reloc_riprel_4byte_imm2,                   // by a 1, 2 or 4-byte immediate,
  reloc_riprel_4byte_imm4,                   // on COFF
  // On Windows Itanium and NT-POSIX, whose relocation types state the form of
  // the instruction:
  reloc_riprel_4byte_call,    // call *sym(%rip), after any prefixes
  reloc_riprel_4byte_jump,    // jmp *sym(%rip)
  reloc_riprel_4byte_jump_p1, // jmp *sym(%rip) after one prefix byte
  reloc_riprel_4byte_tail,    // jmp *sym(%rip) that may be deleted
  reloc_riprel_4byte_lea,     // lea sym+k(%rip), r64
  // The same behind REX2.
  reloc_riprel_4byte_lea_rex2,
  reloc_riprel_4byte_narrow,  // lea sym+k(%rip), r16 or r32
  reloc_riprel_4byte_test,    // cmpq $0, sym@FUSED(%rip)
  reloc_jump_4byte_pcrel,     // jmp or jcc the assembler relaxed to 32 bits
  reloc_jump_1byte_pcrel,     // jmp or jcc the assembler left at 8 bits
  reloc_tail_4byte_pcrel,     // 32-bit jmp that no unwind frame covers
  // Any other field relative to rip: this kind plus the bytes of the
  // instruction before the field times 16 and those of its immediate after.
  reloc_riprel_4byte_insn,
  // Marker
  LastTargetFixupKind,
  NumTargetFixupKinds = LastTargetFixupKind - FirstTargetFixupKind
};
inline bool isRipRelInsn(MCFixupKind Kind) {
  return Kind >= reloc_riprel_4byte_insn && Kind < reloc_riprel_4byte_insn + 256;
}
}
}

#endif
