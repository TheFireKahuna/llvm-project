// A Windows Itanium or NT-POSIX object also describes how an instruction uses
// a relocation to an external function it defines, which another image's
// definition may preempt, so that the linker can redirect the reference. The
// assembler keeps every relocation to a function, in its own section too; a
// branch has no site, and a static function, which nothing preempts, none.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-objdump -dr %t.o | FileCheck %s --check-prefix=RELOC

// "LLRC", version 1, the x86-64 sites, call-only and function sites
// capabilities (7), then one group of kind 2 and 3 bytes: .text's symbol
// (index 0) with 1 site, an address at offset 3 (3 << 4 | 4).
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 010f0203 000134
// CHECK-EMPTY:

// RELOC:      leaq
// RELOC-NEXT: IMAGE_REL_AMD64_REL32 g
// RELOC-NEXT: callq
// RELOC-NEXT: IMAGE_REL_AMD64_REL32 g
// RELOC-NEXT: leaq
// RELOC-NEXT: IMAGE_REL_AMD64_REL32 s

  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
  leaq g(%rip), %rax           // 0x3: address of an external function
  callq g                      // a branch: no site
  leaq s(%rip), %rax           // a static function: no site
  retq
  .def g; .scl 2; .type 32; .endef
  .globl g
g:
  retq
  .def s; .scl 3; .type 32; .endef
s:
  retq
