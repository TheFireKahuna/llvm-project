// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -S -r %t.o | FileCheck %s
// RUN: llvm-readobj --coff-link-records %t.o | FileCheck %s --check-prefix=REC
// RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj --defsym PLAIN=1 \
// RUN:   %s -o %t.p.o
// RUN: llvm-objdump -d --no-show-raw-insn -j '.text$c' %t.p.o | \
// RUN:   FileCheck %s --check-prefix=PLAIN
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | FileCheck %s --check-prefix=ASM
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym ERR=2 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR2
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -x86-align-branch-boundary=32 -x86-align-branch=jcc+fused \
// RUN:   --defsym ERR=3 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR3

// A .p2align of code names, as its fourth operand, the end of the loop that
// begins after it. In an x86-64 Windows Itanium object a loop of at most 32
// bytes takes no alignment: a 64-byte line keeps it by where its section
// begins. A larger loop, and one that holds other than instructions and such
// loops, takes its directives as written, as every loop of another object
// does.

// ASM: .p2align 4, , 10, .La_end
// ASM: .p2align 3, , , .La_end

// The section stays 16-byte aligned, and the Sections record bars it from
// the places 0, 16, 32 and 48 bytes into a line, as bits 0 to 3, at which a
// loop of it would cross a line. a: 12 bytes at 60 cross from 0. c: at 12
// they cross from 48. d: at 28, from 32. e: the nest, 12 bytes at 90, from
// 32. f: its first three loops bar 0, 16 and 32, and the fourth, which would
// bar 48, is padded by the 7 bytes that leave that place. w is 64-byte
// aligned and begins at 0 alone, so its loop is padded to a line.
// CHECK:      Name: .text$a
// CHECK:      IMAGE_SCN_ALIGN_16BYTES
// CHECK:      Name: .text$w
// CHECK:      IMAGE_SCN_ALIGN_64BYTES
// REC:        ResizableSection: .text$m
// REC-NOT:    ResizableSection
// REC-NOT:    LinePlacesBarred
// REC:        LinePlacesBarred: .text$a (1)
// REC-NEXT:   LinePlacesBarred: .text$c (8)
// REC-NEXT:   LinePlacesBarred: .text$d (4)
// REC-NEXT:   LinePlacesBarred: .text$e (4)
// REC-NEXT:   LinePlacesBarred: .text$f (7)
// REC-NEXT:   LinePlacesBarred: .text$w (14)

// Behind a site the linker may resize a loop's offset is the linker's, and
// the record lists the loop for it to bar by. A loop of at most 17 bytes bars
// at most one place and a larger one two, so m1 and m3 leave a place in every
// link. m4 may not: its padding is an ALIGN in line mode (180355552 is the
// longest no-operation, 15, bit 27 and a span of 11), in the object the span.
// m2, larger than a line keeps, is 16 with at most 10 bytes, then 8. m5 is
// larger too and its alignment is of one byte, which pads nothing in any
// link and states nothing.
// REC-NEXT:   LinePlacesBarred: .text$m (0)
// REC-NEXT:   Loop: 0x3b (12)
// REC-NEXT:   Loop: 0x74 (20)

// n is f behind a site, which an .org keeps from being resized. That is
// known before layout, so its loops are kept as f's: it is barred from 0, 16
// and 32 and its fourth loop is padded by 7, with no ALIGN.
// REC-NEXT:   LinePlacesBarred: .text$n (7)
// REC-NOT:    LinePlacesBarred
// CHECK:      Section ({{[0-9]+}}) .text$m {
// CHECK-NEXT:   0x3 IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_g
// CHECK-NEXT:   0x47 IMAGE_REL_LLVM_ALIGN - (5604)
// CHECK-NEXT:   0x50 IMAGE_REL_LLVM_ALIGN - (483)
// CHECK-NEXT:   0x8B IMAGE_REL_LLVM_ALIGN - (180355552)
// CHECK-NEXT: }
// CHECK-NEXT: Section ({{[0-9]+}}) .text$n {
// CHECK-NEXT:   0x3 IMAGE_REL_LLVM_MEM_LOAD_CALL __imp_g
// CHECK-NEXT: }

// PLAIN:     c: nopl
// PLAIN:     1a: jne 0x10

// DIS-LABEL: <a>:
// DIS-NOT:   nop{{[lw]}}
// DIS:       46: jne 0x3c
// e: 33 bytes at 3 take 8, since 16 is 13 bytes away, and at 49 the same.
// Neither loop of the nest is padded.
// DIS-LABEL: <e>:
// DIS:       3: nopl
// DIS:       27: jne 0x8
// DIS:       31: nopl
// DIS:       57: jne 0x38
// DIS-NOT:   nop{{[lw]}}
// DIS:       62: jne 0x5e
// DIS-NEXT:  64: jne 0x5a
// DIS-LABEL: <f>:
// DIS-NOT:   nop{{[lw]}}
// DIS:       a3: jne 0x99
// DIS:       c9: nopl
// DIS:       da: jne 0xd0
// DIS-LABEL: <w>:
// DIS:       3c: nopl
// DIS:       4a: jne 0x40
// o: 12 bytes that hold an alignment take theirs as written: 8, from 3.
// DIS-LABEL: <o>:
// DIS:       3: nopl
// DIS:       12: jne 0x8
// DIS-LABEL: <m>:
// DIS:       45: jne 0x3b
// DIS-NEXT:  47: nopw
// DIS:       6f: jne 0x50
// DIS-NOT:   nop{{[lw]}}
// DIS:       86: jne 0x74
// DIS:       8b: nopw
// DIS:       a0: jne 0x96
// DIS-LABEL: <n>:
// DIS-NOT:   nop{{[lw]}}
// DIS:       a3: jne 0x99
// DIS:       c9: nopl
// DIS:       da: jne 0xd0

// ERR: [[#@LINE+28]]:25: error: only a .p2align of code without a fill value takes the end of a loop
// ERR: [[#@LINE+28]]:21: error: only a .p2align of code without a fill value takes the end of a loop
// ERR2: error: .p2align loop end symbol '.Lbefore' must follow it in the current section
// ERR2: error: .p2align loop end symbol 'a' must follow it in the current section
// ERR2: error: .p2align loop end symbol 'undefined' must follow it in the current section
// Padding before a branch that what follows it makes unstatable shows only
// after layout that a section is not resized, once its loops behind a site
// were laid out for the linker to keep.
// ERR3: error: a line cannot keep a loop behind a resizable site in '.text$x', which its layout leaves fixed

.macro loop name, before, size
  .rept \before
  nop
  .endr
  .p2align 4, , 10, .L\name\()_end
  .p2align 3, , , .L\name\()_end
.L\name:
  .rept \size - 2
  nop
  .endr
  jne .L\name
.L\name\()_end:
.endm

.ifdef ERR
  .text
.Lbefore:
.if ERR == 1
  .p2align 4, 0x90, 10, .Lbefore
  .balign 16, , 10, .Lbefore
.elseif ERR == 3
  .section .text$x,"xr"
  movq __imp_g@FUSED(%rip), %rax
  .p2align 4, , 10, .Lx_end
.Lx:
  .rept 10
  nop
  .endr
  jmp .Lx
.Lx_end:
  movq %rax, %rcx
  cmpq %rax, %rcx
  .fill 30, 1, 0x90
  je .Ly
.Ly:
  ret
.else
  nop
  .p2align 4, , 10, .Lbefore
  .p2align 4, , 10, a
  .p2align 4, , 10, undefined
.endif
.endif

  .section .text$a,"xr"
a:
  loop a, 60, 12
  ret

  .section .text$c,"xr"
c:
  loop c, 12, 12
  ret

  .section .text$d,"xr"
d:
  loop d, 28, 12
  ret

  .section .text$e,"xr"
e:
  loop e1, 3, 33
  loop e2, 8, 33
  nop
  .p2align 4, , 10, .Louter_end
  .p2align 3, , , .Louter_end
.Louter:
  loop inner, 4, 6
  jne .Louter
.Louter_end:
  ret

  .section .text$f,"xr"
f:
  loop f1, 60, 12
  loop f2, 33, 12
  loop f3, 36, 12
  loop f4, 36, 12
  ret

  .section .text$w,"xr"
  .p2align 6
w:
  loop w, 60, 12
  ret

  .section .text$o,"xr"
o:
  .rept 3
  nop
  .endr
  .p2align 4, , 10, .Lo_end
  .p2align 3, , , .Lo_end
.Lo:
  .rept 4
  nop
  .endr
  .p2align 2
  .rept 6
  nop
  .endr
  jne .Lo
.Lo_end:
  ret

.ifndef PLAIN
  .section .text$m,"xr"
m:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  loop m1, 50, 12
  loop m2, 0, 33
  loop m3, 3, 20
  loop m4, 3, 12
  .p2align 0, , , .Lm5_end
.Lm5:
  .rept 40
  nop
  .endr
  jne .Lm5
.Lm5_end:
  ret

  .section .text$n,"xr"
n:
  movq __imp_g@LOAD_CALL(%rip), %rax
  callq *%rax
  loop n1, 51, 12
  loop n2, 33, 12
  loop n3, 36, 12
  loop n4, 36, 12
  ret
  .org 0x100
.endif
