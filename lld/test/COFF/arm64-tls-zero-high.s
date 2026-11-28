# REQUIRES: aarch64

## In a resizable section the add of a thread-local's high part goes where
## that part is zero and only the instruction after it reads what it wrote,
## by fact 15 or by writing the register as ELF proves a pair, and that
## instruction takes the add's base. Where the low part is zero too, in the
## add after it, and only an access after that reads the add, both go and the
## access takes the base. An offset of 4096, a store of the register, a
## register read further on, a label control reaches and fields that hold
## different addends keep the high add. The checked low part of
## -mtls-size=12, which has no high part, goes by the same rule where it is
## zero.

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix %s -o %t.obj
# RUN: lld-link -entry:main -subsystem:console -machine:arm64 %t.obj \
# RUN:   -out:%t.exe
# RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck %s
# RUN: llvm-readobj --unwind %t.exe | FileCheck --check-prefix=UNWIND %s

# CHECK:      <.text>:
# CHECK-NEXT:   ldr w0, [x9, #0x14]
# CHECK-NEXT:   add x1, x9, #0x10
# CHECK-NEXT:   str w2, [x9, #0x18]
# CHECK-NEXT:   ldr w3, [x9, x4, lsl #2]
# CHECK-NEXT:   add x5, x9, #0x0
# CHECK-NEXT:   ldr w6, [x5, x4, lsl #2]
# CHECK-NEXT:   str x5, [x10]
# CHECK-NEXT:   ldrb w7, [x9, #0xfff]
# CHECK-NEXT:   add x8, x9, #0x1, lsl #12
# CHECK-NEXT:   ldrb w7, [x8, #0xf]
# CHECK-NEXT:   add x8, x9, #0x0, lsl #12
# CHECK-NEXT:   ldr w0, [x8, #0x10]
# CHECK-NEXT:   add x0, x0, x8
# CHECK-NEXT:   add x8, x9, #0x0, lsl #12
# CHECK-NEXT:   str x8, [x8, #0x10]
# CHECK-NEXT:   add x8, x9, #0x0, lsl #12
# CHECK-NEXT:   ldr w0, [x8, #0x10]
# CHECK-NEXT:   cbz w0, 0x{{[0-9a-f]+}}
# CHECK-NEXT:   add x8, x9, #0x0, lsl #12
# CHECK-NEXT:   ldr w0, [x8, #0x18]
# CHECK-NEXT:   ldr w3, [x9, x4, lsl #2]
# CHECK-NEXT:   add x1, x9, #0x4
# CHECK-NEXT:   add x8, x9, #0x10
# CHECK-NEXT:   ldr w3, [x8, x4, lsl #2]
# CHECK-NEXT:   add x5, x9, #0x0
# CHECK-NEXT:   ldr w6, [x5, x4, lsl #2]
# CHECK-NEXT:   str x5, [x10]
# CHECK-NEXT:   ldr x8, [x28, #0x150]
# CHECK-NEXT:   ret
# CHECK-NEXT:   stp x29, x30, [sp, #-0x10]!
# CHECK-NEXT:   ldr w0, [x9, #0x10]
# CHECK-NEXT:   cbz w0, 0x[[#%x,ZERO:]]
# CHECK-NEXT:   ldp x29, x30, [sp], #0x10
# CHECK-NEXT:   ret
# CHECK-NEXT:   [[#%x,ZERO]]: ldr w0, [x9, #0x14]
# CHECK-NEXT:   ldp x29, x30, [sp], #0x10
# CHECK-NEXT:   ret

## The function's length and its epilogues' starts, in instructions, move
## with its code.
# UNWIND:      FunctionLength: 32
# UNWIND:      StartOffset: 3
# UNWIND:      StartOffset: 6

  .text
  .globl main
main:
## A folded access, by fact 15.
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv+4
  ldr w0, [x8, :secrel_lo12:tv+4]
## The low part's add, which writes the register.
  add x1, x9, :secrel_hi12:tv
  add x1, x1, :secrel_lo12:tv
## A store, by fact 15.
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv+8
  str w2, [x8, :secrel_lo12:tv+8]
## Both parts zero before an indexed access that alone reads the low add.
  add x8, x9, :secrel_hi12:t0
  .site_fact 15, 0
  add x8, x8, :secrel_lo12:t0
  ldr w3, [x8, x4, lsl #2]
## A store of the register after the access keeps the low add.
  add x5, x9, :secrel_hi12:t0
  add x5, x5, :secrel_lo12:t0
  ldr w6, [x5, x4, lsl #2]
  str x5, [x10]
## 4095 has no high part; 4096 has one.
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:t0+4095
  ldrb w7, [x8, :secrel_lo12:t0+4095]
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv+4095
  ldrb w7, [x8, :secrel_lo12:tv+4095]
## No fact, and the register read again.
  add x8, x9, :secrel_hi12:tv
  ldr w0, [x8, :secrel_lo12:tv]
  add x0, x0, x8
## A store of the register itself.
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv
  str x8, [x8, :secrel_lo12:tv]
## A label control reaches at the access.
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv
.Lin:
  ldr w0, [x8, :secrel_lo12:tv]
  cbz w0, .Lin
## The two fields hold different addends.
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv+4
  ldr w0, [x8, :secrel_lo12:tv+8]
## A low part alone, zero, before an indexed access that alone reads it.
  .site_fact 15, 0
  add x8, x9, :dtprel_lo12:t0
  ldr w3, [x8, x4, lsl #2]
## Before an add that writes its register.
  add x1, x9, :dtprel_lo12:t0
  add x1, x1, #4
## Not zero.
  .site_fact 15, 0
  add x8, x9, :dtprel_lo12:tv
  ldr w3, [x8, x4, lsl #2]
## Zero, and the register read again.
  add x5, x9, :dtprel_lo12:t0
  ldr w6, [x5, x4, lsl #2]
  str x5, [x10]
## Local-exec, the thread pointer's offset.
  add x8, x28, :tprel_hi12:tv
  ldr x8, [x8, :tprel_lo12_nc:tv]
  ret

  .globl framed
  .p2align 2
framed:
  .seh_proc framed
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  .seh_endprologue
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv
  ldr w0, [x8, :secrel_lo12:tv]
  cbz w0, .Lzero
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
.Lzero:
  .site_fact 15, 0
  add x8, x9, :secrel_hi12:tv+4
  ldr w0, [x8, :secrel_lo12:tv+4]
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
  .seh_endproc

  .section .tls$,"dw"
  .globl t0, tv
t0:
  .zero 16
tv:
  .zero 16
