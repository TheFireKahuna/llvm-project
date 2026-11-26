# REQUIRES: x86, aarch64

## Unwind records fold where they are equal in the image, as assembled or not,
## and follow the record they were one with before relaxation; folding that
## moves an address out of relaxation's reach is undone.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64.obj
# DEFINE: %{x64} = lld-link -dll -noentry -export:f1 -export:f2 -export:f3 \
# DEFINE:   -export:f4 -export:f5 -export:f6 -export:c1 -export:c2 x64.obj

# RUN: %{x64} -out:x64.dll -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-readobj --unwind x64.dll | FileCheck --check-prefix=X64 %s
# RUN: %{x64} -opt:noicf -out:x64-noicf.dll
# RUN: llvm-readobj --unwind x64-noicf.dll | FileCheck --check-prefix=NOICF %s
# RUN: lld-link -machine:arm64 -dll -noentry -export:f1 -export:f2 -export:f3 \
# RUN:   -export:f4 -export:f5 a64.obj -out:a64.dll -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=A64-LOG %s
# RUN: llvm-readobj --unwind a64.dll | FileCheck --check-prefix=A64 %s
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium undo.s -o undo.obj
# RUN: lld-link -machine:arm64 -dll -noentry -export:f1 -export:f2 -export:f3 \
# RUN:   undo.obj -out:undo.dll -verbose 2>&1 | FileCheck --check-prefix=UNDO-LOG %s
# RUN: llvm-objdump -d --no-show-raw-insn undo.dll | \
# RUN:   FileCheck --check-prefix=UNDO-CODE %s
# RUN: llvm-readobj --unwind undo.dll | FileCheck --check-prefix=UNDO %s

# LOG: folded 1 unwind records
# A64-LOG: folded 2 unwind records

## f1, f2 and f6 share a record; f3, f4, f5, and c1 and c2 with their chained
## records, have their own.
# X64:      StartAddress: (0x180001000)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x180002098)
# X64:      StartAddress: (0x18000100B)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x180002098)
# X64:      StartAddress: (0x180001016)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x1800020AC)
# X64:      StartAddress: (0x180001022)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x1800020C0)
# X64:      StartAddress: (0x18000102D)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x1800020D4)
# X64:      StartAddress: (0x180001038)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x180002098)
# X64:      StartAddress: (0x180001045)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x1800020E8)
# X64:      StartAddress: (0x18000104B)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x1800020F0)
# X64:      StartAddress: (0x180001052)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x180002100)
# X64:      StartAddress: (0x180001058)
# X64-NEXT: EndAddress:
# X64-NEXT: UnwindInfoAddress: (0x180002108)

# NOICF:      StartAddress: (0x180001000)
# NOICF-NEXT: EndAddress:
# NOICF-NEXT: UnwindInfoAddress: (0x1800020A0)
# NOICF:      StartAddress: (0x18000100B)
# NOICF-NEXT: EndAddress:
# NOICF-NEXT: UnwindInfoAddress: (0x1800020B4)

## f1, a word shorter in the image than as assembled, f2, f4 and f5 share a
## record; f3, with another handler, has its own.
# A64:      Function: 0x180001000
# A64-NEXT: ExceptionRecord: 0x180002074
# A64:      FunctionLength: 28
# A64:      Function: 0x18000101C
# A64-NEXT: ExceptionRecord: 0x180002074
# A64:      Function: 0x180001038
# A64-NEXT: ExceptionRecord: 0x180002084
# A64:      Function: 0x180001054
# A64-NEXT: ExceptionRecord: 0x180002074
# A64:      Function: 0x180001070
# A64-NEXT: ExceptionRecord: 0x180002074

## Folding f2's record into f1's moves cell out of f1's adr: f1's adrp and add
## return, and f1, f2 and f3 keep the records they had.
# UNDO-LOG:      folded 1 unwind records
# UNDO-LOG-NEXT: unfolded the unwind records
# UNDO-CODE:      180103004:   adrp x0, 0x180003000
# UNDO-CODE-NEXT: 180103008:   add x0, x0, #0x8
# UNDO:      Function: 0x180103000
# UNDO-NEXT: ExceptionRecord: 0x180002058
# UNDO:      FunctionLength: 20
# UNDO:      Function: 0x180103014
# UNDO-NEXT: ExceptionRecord: 0x180002068
# UNDO:      FunctionLength: 16
# UNDO:      Function: 0x180103024
# UNDO-NEXT: ExceptionRecord: 0x180002068
# UNDO:      FunctionLength: 16

#--- x64.s
.macro fn name, sect, handler, data, pad, body:vararg
  .section .text$\sect,"xr",discard,\name
  .globl \name
  .seh_proc \name
\name:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
.L\name\()a:
  \body
  .fill \pad, 1, 0x90
.L\name\()b:
  addq $40, %rsp
  retq
  .seh_handler \handler, @unwind, @except
  .seh_handlerdata
  .long \data\()@IMGREL
  .uleb128 .L\name\()b - .L\name\()a
  .section .text$\sect,"xr",discard,\name
  .seh_endproc
.endm
## Chained information names its own function, so c1's and c2's stay apart.
.macro chained name, sect, reg
  .section .text$\sect,"xr",discard,\name
  .globl \name
  .seh_proc \name
\name:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  je g
  .seh_splitchained
  .seh_endprologue
  xorl \reg, \reg
  addq $40, %rsp
  retq
  .seh_endproc
  movq __imp_g@FUSED(%rip), %rax
.endm

## f1's je is six bytes as assembled and two in the image, where its record
## equals f2's; the link deletes the fused read after f1.
  fn f1, a, h, t, 0, je g
  movq __imp_g@FUSED(%rip), %rax
  fn f2, b, h, t, 0, xorl %eax, %eax
## f6's record equals f2's as assembled, so it follows f2's into f1's.
  fn f6, f, h, t, 0, xorl %esi, %esi
## Apart: another distance, another handler, data naming another symbol.
  fn f3, c, h, t, 1, xorl %eax, %eax
  fn f4, d, h2, t, 0, xorl %ecx, %ecx
  fn f5, e, h, t2, 0, xorl %edx, %edx

  .section .text$g,"xr",discard,g
  .globl g, h, h2
g:
h:
  retq
h2:
  retq
  chained c1, h, %eax
  chained c2, i, %ecx
  .data
  .globl t, t2
t:
  .long 0
t2:
  .long 0

#--- fn-a64.inc
.macro beg name, sect
  .section \sect,"xr",discard,\name
  .globl \name
  .seh_proc \name
\name:
  sub sp, sp, #16
  .seh_stackalloc 16
  .seh_endprologue
.endm
.macro end name, sect, handler, data
  .seh_startepilogue
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  ret
  .seh_handler \handler, @unwind, @except
  .seh_handlerdata
  .long \data
  .section \sect,"xr",discard,\name
  .seh_endproc
.endm

#--- a64.s
  .include "fn-a64.inc"
## f1 is a word shorter in the image, where its adrp and add are an adr.
  beg f1, .text$a
  adrp x0, t
  add x0, x0, :lo12:t
  mov x1, #1
  adrp x2, far
  add x2, x2, :lo12:far
  end f1, .text$a, h, t@IMGREL
  beg f2, .text$b
  mov x1, #2
  mov x1, #3
  adrp x2, far
  add x2, x2, :lo12:far
  end f2, .text$b, h, t@IMGREL
## Apart: another handler.
  beg f3, .text$c
  mov x1, #4
  mov x1, #5
  adrp x2, far
  add x2, x2, :lo12:far
  end f3, .text$c, h2, t@IMGREL
## f4 and f5 hold no pair, so they were one before relaxation and f5 follows
## f4's into f1's.
  beg f4, .text$d
  mov x1, #6
  mov x1, #7
  mov x1, #8
  mov x1, #9
  end f4, .text$d, h, t@IMGREL
  beg f5, .text$e
  mov x1, #10
  mov x1, #11
  mov x1, #12
  mov x1, #13
  end f5, .text$e, h, t@IMGREL

  .section .text$g,"xr",discard,h
  .globl h, h2
h:
  ret
h2:
  ret
  .data
  .globl t
t:
  .long 0
  .section .bss,"bw"
  .space 0x200000
far:
  .long 0

## cell is 1 MiB before f1's adrp, after the records, with what keeps .rdata
## its pages. f3's record was one with f2's and is again once they unfold.
#--- undo.s
  .include "fn-a64.inc"
  beg f1, .late$a
  adrp x0, cell
  add x0, x0, :lo12:cell
  end f1, .late$a, h, 0
  beg f2, .late$b
  mov x0, #1
  end f2, .late$b, h, 0
  beg f3, .late$c
  mov x0, #2
  end f3, .late$c, h, 0

  .text
  .globl h
h:
  ret
  .section .xdata$z,"dr"
  .zero 0xf90
cell:
  .quad 0
  .zero 0x40
  .section .gap,"bw"
  .zero 0xfe000
