# REQUIRES: aarch64

## On ARM64, when every instruction that refers to a local import pointer is
## the adrp or the 64-bit ldr of a load of it, each becomes the adrp or add of
## its symbol, and the pointer is left out of the image unless data or a root
## refers to it. Any other instruction keeps every load of the pointer, since an adrp may
## serve several loads. A .refptr.X cell, which holds only X's address, is such
## a pointer to X when X is in the image; it stays where it is, as the pointer,
## while anything else reaches it.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj defs.obj \
# RUN:   2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d -s --no-show-raw-insn a.exe | FileCheck %s

## Each object that loads a pointer is reported, as link.exe reports it.
# WARN-DAG: warning: main.obj: locally defined symbol imported: f (defined in defs.obj) [LNK4217]
# WARN-DAG: warning: main.obj: locally defined symbol imported: g (defined in defs.obj) [LNK4217]
# WARN-DAG: warning: main.obj: locally defined symbol imported: h (defined in defs.obj) [LNK4217]

## The cells .refptr.u and .refptr.w, which other names reach, and the
## pointers to g and to h, which .xdata reaches, remain; .refptr.v and the
## pointer to f go.
# CHECK:      Contents of section .rdata:
# CHECK-NEXT: 140002000 00300040 01000000 10300040 01000000
# CHECK-NEXT: 140002010 48100040 01000000 4c100040 01000000
# CHECK-NEXT: 140002020 f8ffffff
# CHECK:      adrp x0, 0x140001000
# CHECK-NEXT: add x0, x0, #0x44
# CHECK-NEXT: adrp x1, 0x140002000
# CHECK-NEXT: ldr x1, [x1, #0x10]
# CHECK-NEXT: adrp x2, 0x140002000
# CHECK-NEXT: add x2, x2, #0x10
# CHECK-NEXT: adrp x3, 0x140001000
# CHECK-NEXT: add x3, x3, #0x4c
# CHECK-NEXT: adrp x4, 0x140003000
# CHECK-NEXT: add x4, x4, #0x8
# CHECK-NEXT: adrp x5, 0x140003000
# CHECK-NEXT: add x5, x5, #0x10
# CHECK-NEXT: adrp x6, 0x140003000
# CHECK-NEXT: add x6, x6, #0x0
# CHECK-NEXT: adrp x7, 0x140002000
# CHECK-NEXT: add x7, x7, #0x0

## A root keeps a pointer that its loads bypass.
# RUN: lld-link -entry:main -subsystem:console -out:c.exe main.obj defs.obj \
# RUN:   -include:__imp_f 2>&1 | FileCheck --check-prefix=ROOT-WARN %s
# RUN: llvm-objdump -d -s --no-show-raw-insn c.exe | FileCheck --check-prefix=ROOT %s
# ROOT-WARN: warning: <root>: locally defined symbol imported: f (defined in defs.obj) [LNK4217]
# ROOT:      140002010 44100040 01000000
# ROOT:      adrp x0, 0x140001000
# ROOT-NEXT: add x0, x0, #0x44

## Without garbage collection, a cell that only rewritten loads read goes too.
# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj defs.obj \
# RUN:   -opt:noref 2>/dev/null
# RUN: llvm-objdump -d -s --no-show-raw-insn b.exe | FileCheck %s

#--- main.s
  .globl main
main:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  adrp x1, __imp_g
  ldr x1, [x1, :lo12:__imp_g]
  adrp x2, __imp_g
  add x2, x2, :lo12:__imp_g
  adrp x3, __imp_h
  ldr x3, [x3, :lo12:__imp_h]
  adrp x4, .refptr.v
  ldr x4, [x4, :lo12:.refptr.v]
  adrp x5, .refptr.w
  ldr x5, [x5, :lo12:.refptr.w]
  adrp x6, .refptr.u
  ldr x6, [x6, :lo12:.refptr.u]
  adrp x7, local
  add x7, x7, :lo12:local
  ret

  .section .xdata,"dr"
  .word __imp_h-.

  .section .rdata$.refptr.v,"dr",discard,.refptr.v
  .globl .refptr.v
.refptr.v:
  .xword v

## Another name that the cell's section defines keeps it.
  .section .rdata$.refptr.w,"dr",discard,.refptr.w
  .globl .refptr.w, alias
.refptr.w:
alias:
  .xword w

## So does a local name that its object refers to.
  .section .rdata$.refptr.u,"dr",discard,.refptr.u
  .globl .refptr.u
.refptr.u:
local:
  .xword u

#--- defs.s
  .globl f, g, h
  .p2align 2
f:
  ret
g:
  ret
h:
  ret
  .data
  .globl u, v, w
u:
  .xword 0
v:
  .xword 0
w:
  .xword 0
