// A code section in which the linker may change the size of an instruction
// that a site describes is covered: the object describes each value that
// depends on where code across such an instruction lies, and each alignment
// after the first, which the linker redoes. A difference across nothing the
// linker resizes is folded as usual. Alignment with data, or padding that
// aligns branches, which depends on where each lies in the image, leaves a
// section uncovered.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=call %s -o %t.pad.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.pad.o | \
// RUN:   FileCheck --check-prefix=PAD %s

// After the sites, a call in each code section, a group of kind 30 and 29
// bytes. First .text's own group (symbols 0 and 0) with 2 entries, each
// (delta << 4 | implied << 3 | same << 2 | type):
// - 4a 12 07: a PC-relative value (type 2) at 0x4, in an 8-bit jump's field
//   (kind 2, size 1), to 0x4 + 7, less the field's size (implied): je
//   .Lafter, across the call.
// - e001 0e e403 10: an alignment (type 0) at 0x12 of 14 bytes, to 16 (2^4)
//   with no-operations of up to 15 bytes, skipping at most 16.
// The lea of .Lafter crosses nothing the linker resizes, so it has no entry.
// Then the group of .rdata's values (symbol 10) in .text, with 4 entries,
// whose places are delta-coded:
// - 09 40 20 0b: a difference (type 1) at 0x0 in 4 bytes (kind 0, size 4),
//   0x20 - 0xb with no constant (implied): .Lloop - .Lafter, across the
//   padding.
// - 43 00: at 0x4, a relocation to .text whose place is 0x20 (+0), .Lloop,
//   since its addend also holds the distance from .Ltab.
// - 49 13 00 75: at 0x8, an unsigned LEB128 of 1 byte (kind 3), 0x20 - 0.
// - 19 40 6b 00: at 0x9, 0xb - 0 in 4 bytes: .Lafter - f, across the call.
// The ADDR64 relocation to .Lloop+2 at 0xd has the place's offset as its
// addend, so it needs no entry; .text$u, with data alignment, has no group.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070206 00017108 01211e1d
// CHECK-NEXT: 0010 0000024a 1207e001 0ee40310 0a000409
// CHECK-NEXT: 0020 40200b43 00491300 7519406b 00
// CHECK-EMPTY:

// PAD:      Contents of section .llvm_link_records:
// PAD-NEXT: 0000 4c4c5243 01070206 00017108 0121
// PAD-EMPTY:

  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
.seh_proc f
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  testl %ecx, %ecx
  je .Lafter
  callq *__imp_g(%rip)
.Lafter:
  leaq .Lafter(%rip), %rax
  .p2align 4
.Lloop:
  decl %ecx
  jne .Lloop
  popq %rbx
  retq
.seh_endproc

  .section .text$u,"xr"
  callq *__imp_g(%rip)
  jmp .Lu
  nop
.Lu:
  retq
  .p2align 0, 0

  .section .rdata,"dr"
.Ltab:
  .long .Lloop - .Lafter
  .long .Lloop - .Ltab
  .uleb128 .Lloop - f
  .long .Lafter - f
  .quad .Lloop+2
