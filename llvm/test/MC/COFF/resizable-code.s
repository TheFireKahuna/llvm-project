// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --symbols --coff-link-records %t.o | FileCheck %s
// RUN: llvm-objdump -d -j .text -j '.text$a' %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-readobj --unwind %t.o | FileCheck %s --check-prefix=UNWIND
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %s -o %t.n.o
// RUN: llvm-readobj -r --symbols --coff-link-records %t.n.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc --defsym MSVC=1 -filetype=obj %s -o %t.m.o
// RUN: llvm-readobj -r --symbols %t.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=Label \
// RUN:     '--implicit-check-not=IMAGE_REL_LLVM_{{[B-Z]}}' --implicit-check-not=PAIR

// On Windows Itanium and NT-POSIX a section whose code the linker may move is
// listed in the Sections link records, and every value that depends on its
// layout is a relocation against a label. An ALIGN index holds the alignment,
// longest no-op and most bytes skipped: 484 is 16 and 15, 2019 is 8, 15 and 3.

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{[0-9]+}}) .text {
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_JUMP8 [[AFTER:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x7 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x15 IMAGE_REL_AMD64_SREL32 [[LOOP:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x1B IMAGE_REL_AMD64_SREL32 [[LOOP]]
// CHECK-NEXT:     0x21 IMAGE_REL_AMD64_SREL32 [[LOOP]]
// CHECK-NEXT:     0x26 IMAGE_REL_LLVM_SSPAN8 [[LOOP]]
// CHECK-NEXT:     0x26 IMAGE_REL_AMD64_PAIR [[END:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x2B IMAGE_REL_LLVM_MEM_JUMP __imp_g
// CHECK-NEXT:     0x32 IMAGE_REL_LLVM_MEM_JUMP_P1 __imp_g
// CHECK-NEXT:     0x36 IMAGE_REL_LLVM_ALIGN - (484)
// CHECK-NEXT:     0x45 IMAGE_REL_LLVM_JUMP8 [[AFTER]]
// CHECK-NEXT:     0x48 IMAGE_REL_LLVM_JUMP32 [[FAR:\$L[0-9a-f]+]]
// CHECK-NEXT:     0xD0 IMAGE_REL_LLVM_ALIGN - (2019)
// CHECK-NEXT:     0xD1 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:     0xD8 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }

// In a section the linker does not resize, the assembler resolves what it
// can, as on another triple, and no relocation names a label.
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$d {
// CHECK-NEXT:     0x3 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$o {
// CHECK-NEXT:     0x3 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$p {
// CHECK-NEXT:     0x3 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$s {
// CHECK-NEXT:     0x3 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$b {
// CHECK-NEXT:     0x12 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x1A IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$a {
// CHECK-NEXT:     0x1 IMAGE_REL_LLVM_JUMP8 $L{{[0-9a-f]+}}
// CHECK-NEXT:     0xB IMAGE_REL_LLVM_ALIGN - (484)
// CHECK-NEXT:     0x11 IMAGE_REL_LLVM_BRANCH32 ext
// CHECK-NEXT:     0x15 IMAGE_REL_LLVM_ALIGN - (484)
// CHECK-NEXT:     0x23 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$e {
// CHECK-NEXT:     0x2 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x7 IMAGE_REL_LLVM_SSPAN8 [[FIRST:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x7 IMAGE_REL_AMD64_PAIR $L{{[0-9a-f]+}}
// CHECK-NEXT:     0x9 IMAGE_REL_LLVM_SSPAN8 [[LAST:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x9 IMAGE_REL_AMD64_PAIR $L{{[0-9a-f]+}}
// CHECK-NEXT:     0xC IMAGE_REL_LLVM_MEM_JUMP_TAIL __imp_g
// CHECK-NEXT:     0x14 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$n {
// CHECK-NEXT:     0x1 IMAGE_REL_LLVM_BRANCH32 ext
// CHECK-NEXT:     0x6 IMAGE_REL_LLVM_ALIGN - (100)
// CHECK-NEXT:     0x6 IMAGE_REL_AMD64_SREL32 $L{{[0-9a-f]+}}
// CHECK-NEXT:     0x6 IMAGE_REL_AMD64_ABSOLUTE - (0)
// CHECK-NEXT:     0xA IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$v {
// CHECK-NEXT:     0x5 IMAGE_REL_LLVM_JUMP8 $L{{[0-9a-f]+}}
// CHECK-NEXT:     0xD IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x19 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .xdata {
// CHECK-NEXT:     0x6 IMAGE_REL_LLVM_SSPAN_EPILOGV2 $L{{[0-9a-f]+}}
// CHECK-NEXT:     0x6 IMAGE_REL_AMD64_PAIR $L{{[0-9a-f]+}}
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$w {
// CHECK-NEXT:     0xE IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }

// In a section with no such site the linker keeps the length of every site,
// so the assembler resolves its distances and states no padding. Where it
// ends in a jmp that the linker may delete, the section is listed and only a
// value that spans that jmp is a relocation.
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$k {
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x11 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$t {
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x9 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .rdata$t {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_SSPAN32 [[TEND:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_PAIR t
// CHECK-NEXT:   }

// A difference of two labels that the linker may move apart is a span, of any
// width, and one it cannot change is a number. An address names a label.
// CHECK-NEXT:   Section ({{[0-9]+}}) .rdata {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_SSPAN32 [[LOOP]]
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_PAIR [[AFTER]]
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_SSPAN8 [[LOOP]]
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_PAIR [[AFTER]]
// CHECK-NEXT:     0x5 IMAGE_REL_LLVM_SSPAN16 [[LOOP]]
// CHECK-NEXT:     0x5 IMAGE_REL_AMD64_PAIR [[AFTER]]
// CHECK-NEXT:     0x7 IMAGE_REL_LLVM_SSPAN_ULEB [[LOOP]]
// CHECK-NEXT:     0x7 IMAGE_REL_AMD64_PAIR [[BEGIN:\$L[0-9a-f]+]]
// CHECK-NEXT:     0xC IMAGE_REL_AMD64_ADDR64 [[LOOP]]
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .pdata {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_ADDR32NB [[BEGIN]]
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_ADDR32NB [[PDATAEND:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x8 IMAGE_REL_AMD64_ADDR32NB .xdata
// CHECK:        Section ({{[0-9]+}}) .pdata {
// CHECK:        Section ({{[0-9]+}}) .pdata {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_ADDR32NB .text$w
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_ADDR32NB .text$w

// CHECK:          Name: [[AFTER]]
// CHECK-NEXT:     Value: 11
// CHECK-NEXT:     Section: .text
// CHECK-NEXT:     BaseType: Null
// CHECK-NEXT:     ComplexType: Null
// CHECK-NEXT:     StorageClass: Label
// CHECK:          Name: [[FIRST]]
// CHECK-NEXT:     Value: 0
// CHECK:          Name: [[LAST]]
// CHECK-NEXT:     Value: 16
// CHECK:          Name: [[PDATAEND]]
// CHECK-NEXT:     Value: 208

// .text, .text$b, .text$a, .text$e, .text$n and .text$v are listed, the
// padding of .text$n being stated by a marker and the unwind data of the
// function in .text$v being of version 2, whose distance is a relocation.
// The others are not: .text$d aligns with data, .text$o has an .org, .text$p a
// .prefalign and .text$s a fill of a computed length after the first such
// instruction, and the unwind data of the function in .text$w is of version
// 3, whose values are as assembled.
// CHECK:      LinkRecords {
// CHECK-NEXT:   Group {
// CHECK-NEXT:     Kind: Symbols (0x1)
// CHECK-NEXT:   }
// CHECK-NEXT:   Group {
// CHECK-NEXT:     Kind: Sections (0x2)
// CHECK-NEXT:     ResizableSection: .text ({{[0-9]+}})
// CHECK-NEXT:     ResizableSection: .text$b ({{[0-9]+}})
// CHECK-NEXT:     ResizableSection: .text$a ({{[0-9]+}})
// CHECK-NEXT:     ResizableSection: .text$e ({{[0-9]+}})
// CHECK-NEXT:     ResizableSection: .text$n ({{[0-9]+}})
// CHECK-NEXT:     ResizableSection: .text$v ({{[0-9]+}})
// CHECK-NEXT:     ResizableSection: .text$t ({{[0-9]+}})
// CHECK-NEXT:   }

// UNWIND: EPILOG offset=0xC

// The fields keep their widths and their assembled values, and the padding
// is no-operations.
// DIS:       3: 74 00                         je
// DIS:      12: 48 8d 05 27 00 00 00          leaq 0x27(%rip), %rax
// DIS:      19: 83 3d 20 00 00 00 01          cmpl $0x1, 0x20(%rip)
// DIS:      20: e8 1b 00 00 00                callq
// DIS:      25: e3 19                         jrcxz
// DIS:      27: e2 e2                         loop
// DIS:      36: 66 2e 0f 1f 84 00 00 00 00 00 nopw %cs:(%rax,%rax)
// DIS:      42: 75 fc                         jne
// DIS:      44: 75 00                         jne
// DIS:      46: 0f 84 00 00 00 00             je
// DIS:      d0: e9 00 00 00 00                jmp
// DIS-LABEL: section .text$a:
// DIS:       b: 0f 1f 44 00 00                nopl
// DIS:      15: 66 66 2e 0f 1f 84 00 00 00 00 00 nopw

// Another triple's object has none of this.
// MSVC: 0x7 IMAGE_REL_AMD64_REL32 __imp_g

// A fused read, after which the linker moves the code of its section.
.macro moves
.ifdef MSVC
  movq __imp_g(%rip), %rax
.else
  movq __imp_g@FUSED(%rip), %rax
.endif
.endm

  .text
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
.Lbegin:
  .seh_proc f
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  testl %ecx, %ecx
  je .Lafter
  callq *__imp_g(%rip)
.Lafter:
  leaq .Lafter(%rip), %rax
  leaq .Lloop(%rip), %rax
  cmpl $1, .Lloop(%rip)
  callq .Lloop
  jrcxz .Lloop
.Lend:
  loop .Lafter
  jmpq *__imp_g(%rip)
  rex64 jmpq *__imp_g(%rip)
  .p2align 4
.Lloop:
  decl %ecx
  jne .Lloop
  jne .Lafter
  je .Lfar
  .fill 130, 1, 0x90
.Lfar:
  popq %rbx
  retq
  .seh_endproc
  .p2align 3, , 3
  jmp ext
  moves

  .section .text$d,"xr"
  moves
  jmp .Ld
  nop
.Ld:
  retq
  .p2align 2, 0

  .section .text$o,"xr"
  moves
  .org 16
  retq

  .section .text$p,"xr"
  moves
  .prefalign 4, .Lp, nop
  retq
.Lp:

  .section .text$s,"xr"
.Ls:
  moves
  .fill 16 - (. - .Ls), 1, 0xcc
  retq

  .section .text$b,"xr"
  nop
  .p2align 4, 0
  callq *__imp_g(%rip)
  retq
  moves

// The relocation is at the padding, also where instructions of a fixed size
// precede it in its fragment and where it ends the section.
  .section .text$a,"xr"
  jne .La
  movq %rax, %rcx
  movq %rax, %rcx
  movq %rax, %rcx
  .p2align 4
.La:
  callq ext
  .p2align 4
  moves

// An 8-bit field's target that is an external symbol of the field's section
// is the point the assembler resolved it to, which a label marks.
  .section .text$e,"xr"
  .globl first, last
first:
  callq *__imp_g(%rip)
  loop first
  jrcxz last
  jmpq *__imp_g(%rip)
last:
  retq
  moves

// .reloc states a type that marks a place: ALIGN with its index, SREL32 with
// its target and ABSOLUTE.
  .section .text$n,"xr"
n:
  callq ext
  retq
  .reloc ., IMAGE_REL_LLVM_ALIGN, 100
  .reloc ., IMAGE_REL_AMD64_SREL32, n
  .reloc ., IMAGE_REL_AMD64_ABSOLUTE
  nop
  moves

// Unwind data of version 2 or 3 places an epilogue by its distance from the
// function's end, which a relocation states under version 2.
  .section .text$v,"xr"
  .seh_proc v
v:
  .seh_unwindversion 2
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  je .Lv
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
.Lv:
  callq *__imp_g(%rip)
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  .seh_endepilogue
  retq
  .seh_endproc
  moves

  .section .text$w,"xr"
  .seh_proc w
w:
  .seh_unwindversion 3
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  je .Lw
  .seh_startepilogue
  addq $40, %rsp
  .seh_endepilogue
  retq
.Lw:
  moves
  .seh_startepilogue
  addq $40, %rsp
  .seh_endepilogue
  retq
  .seh_endproc

  .section .text$k,"xr"
  je .Lk
  callq *__imp_g(%rip)
  .p2align 4
.Lk:
  jmp ext
  retq

  .section .text$t,"xr"
t:
  je .Lt
  callq *__imp_g(%rip)
.Lt:
  jmp ext
.Ltend:

  .section .rdata$t,"dr"
  .long .Ltend - t
  .long .Lt - t

  .section .rdata,"dr"
  .long .Lloop - .Lafter
  .byte .Lloop - .Lafter
  .short .Lloop - .Lafter
  .uleb128 .Lloop - .Lbegin
  .long .Lend - .Lafter
  .quad .Lloop
