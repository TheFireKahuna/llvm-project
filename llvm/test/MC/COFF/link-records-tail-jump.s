// .linktailjump says that the next instruction, a jump to a function, ends its
// function. Where the jump also ends its section, the section is listed in a
// group of kind 32, so that a linker that places the target right after the
// section may leave the jump out, and the section is covered, so that what
// its function's length depends on is described. A jump that does not end its
// section is not listed, and a linker that does not know the kind takes the
// jump as the branch it is.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// A group of kind 30 and 6 bytes: .text$a and .text$b are covered. Then a
// group of kind 32 and 1 byte: .text$a, 06, ends in a tail jump.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01071e06 06060008 08002001
// CHECK-NEXT: 0010 06
// CHECK-EMPTY:

// ASM:      .linktailjump
// ASM-NEXT: jmp ext

  .section .text$a,"xr"
  .globl a
a:
  .linktailjump
  jmp ext

  .section .text$b,"xr"
  .globl b
b:
  .linktailjump
  jmp ext
  retq
