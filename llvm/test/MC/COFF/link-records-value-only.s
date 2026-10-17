// .linkvalueonly says that the next instruction, a load of an import pointer
// of data, fills a register that only reads of memory read, as their base. It
// becomes a fact of kind 7 on the load's site, with a page of zero, beside any
// other fact of the site. Before an instruction that is not a load with a
// site, it is dropped.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// A group of kind 2 and 20 bytes: .text with 4 sites.
// - 3b 02 0700: the load at 0x3, value-only.
// - ab01 04 0606 0700: the load at 0xd, which only accesses read (kind 6),
//   before movl, 3 bytes, and value-only.
// - 48 04 03f1a740: the access at 0x11 through site 1's register.
// - 64: the lea at 0x17, which has no fact.
// Then .text is covered.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070214 00043b02 0700ab01
// CHECK-NEXT: 0010 04060607 00480403 f1a74064 1e030000
// CHECK-NEXT: 0020 00
// CHECK-EMPTY:

// ASM:      .linkvalueonly
// ASM-NEXT: movq __imp_v(%rip), %rax

  .text
  .globl f
f:
  .linkvalueonly
  movq __imp_v(%rip), %rax
  movl (%rax,%rcx,4), %eax
.Lr0:
  .linkvalueonly
  .linkfusedread
  movq __imp_v(%rip), %rax
  .linkfusedaccess .Lr0, %rax
  movl 8(%rax), %eax
  .linkvalueonly
  leaq v(%rip), %rax
  .linkvalueonly
  movl $1, %eax
  retq
