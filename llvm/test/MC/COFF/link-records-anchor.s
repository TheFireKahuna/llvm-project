// .linkanchor before a load of the import pointer of data says that a
// register holds the pointer that a load of the same section loaded, of other
// data: kind 10 on the load's site, with the other load's index and the
// register; a load may have several. One naming an instruction that is not a
// load is left out.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// A group of kind 2 and 0xe bytes: .text with 4 sites.
// - 33: the load of a at 0x3.
// - 7b 02 0a01: the load of b at 0xa, which rcx (1) of site 0 may give.
// - 7b 04 0a01 0a29: the load of c at 0x11, which rcx of site 0 or r9 (9) of
//   site 1 (1 << 5) may give.
// - 73: the load of d at 0x18, whose anchor is not a load.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 0107020e 0004337b 020a017b
// CHECK-NEXT: 0010 040a010a 2973
// CHECK-EMPTY:

// ASM:      .linkanchor .La, %rcx
// ASM-NEXT: movq __imp_b(%rip), %r9

  .text
g:
.La:
  movq __imp_a(%rip), %rcx
.Lb:
  .linkanchor .La, %rcx
  movq __imp_b(%rip), %r9
  .linkanchor .La, %rcx
  .linkanchor .Lb, %r9
  movq __imp_c(%rip), %rdx
  .linkanchor .Lnotload, %rcx
  movq __imp_d(%rip), %rdx
.Lnotload:
  retq
