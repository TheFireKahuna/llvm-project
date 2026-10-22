// .linkfunctionoverride says that Windows selects the body a stub jumps to as
// it maps the image: a group of the non-critical kind 42 holding the stub's
// symbol index, the candidates' count and indices, then the diagram's node
// count and each node's left edge, right edge and value, the symbols
// registered even if nothing else names them.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-objdump -t %t.o | FileCheck --check-prefix=SYMS %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// After the header, a group of kind 42 (0x2a) and 15 bytes: symbol 8, f; two
// candidates, symbols 10 and 11; three nodes: (2, 1, 0x13e), (1, 1, 0) and
// (0, 0, 0).
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 010f1e03 0000002a 0f08020a
// CHECK-NEXT: 0010 0b030201 be020101 00000000

// SYMS: wide
// SYMS: narrow

// ASM: .linkfunctionoverride f, (wide, narrow), (2, 1, 0x13e), (1, 1, 0x0), (0, 0, 0x0)

  .text
  .globl f
f:
  jmp base
  .linkfunctionoverride f, (wide, narrow), (2, 1, 0x13e), (1, 1, 0), (0, 0, 0)
