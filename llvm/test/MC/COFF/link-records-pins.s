// A .linkpin directive asks the linker to place a symbol at an address
// congruent to a residue modulo a power of two. Pins travel in a
// LinkRecordPins group (kind 1, critical) of the link-only records, keyed by
// symbol index. A symbol the symbol table leaves out is pinned through its
// section's symbol, with the residue moved by its offset.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | \
// RUN:   FileCheck --check-prefix=OBJ %s
// RUN: llvm-objdump -t %t.o | FileCheck --check-prefix=SYM %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.msvc.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.msvc.o | \
// RUN:   FileCheck --check-prefix=MSVC %s

// ASM: .linkpin _ZTV1D, 12, 4072, required
// ASM: .linkpin _ZTV1A, 6, 56
// ASM: .linkpin .Linner, 6, 40

// SYM: [ 8](sec  4)(fl 0x00)(ty   0)(scl   2) (nx 0) 0x00000000 _ZTV1D
// SYM: [11](sec  5)(fl 0x00)(ty   0)(scl   3) (nx 0) 0x00000000 _ZTV1A
// SYM: [12](sec  6)(fl 0x00)(ty   0)(scl   3) (nx 1) 0x00000000 .rdata$inner

// "LLRC", version 1, the x86-64 capabilities (3), then a group of kind 1 and
// 10 bytes: _ZTV1D (8), flags 12 << 1 | 1 = 0x19, residue 4072 (0xe8 0x1f);
// _ZTV1A (11), flags 6 << 1 = 0x0c, residue 56 (0x38); .Linner, 8 bytes into
// its section, as the section's symbol (12) with residue (40 - 8) mod 64 = 32.
// OBJ:      Contents of section .llvm_link_records:
// OBJ-NEXT: 0000 4c4c5243 0103010a 0819e81f 0b0c380c
// OBJ-NEXT: 0010 0c20

// The group is written for any target the directive is used on.
// MSVC:      Contents of section .llvm_link_records:
// MSVC-NEXT: 0000 4c4c5243 0100010a

  .section .rdata,"dr",discard,_ZTV1D
  .globl _ZTV1D
  .p2align 3
_ZTV1D:
  .quad 0, 0, 0
  .linkpin _ZTV1D, 12, 4072, required

  .section .rdata,"dr",discard,_ZTV1A
  .p2align 3
_ZTV1A:
  .quad 0, 0, 0, 0
  .linkpin _ZTV1A, 6, 56

  .section .rdata$inner,"dr"
  .p2align 3
  .quad 0
.Linner:
  .quad 0
  .linkpin .Linner, 6, 40
