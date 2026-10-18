# REQUIRES: aarch64
## On ARM64, where the record of a thread-local variable is in the image and
## the loads from it are described, the code that loaded its words takes what
## they hold, in the same instructions: the load of the address of the TLS
## index becomes adrp of the index's page, each load through it taking the
## index's offset in the page, and the load of the variable's or its guard's
## offset becomes movz of it where it fits 16 bits. The record's load becomes a
## nop. The record stays, since the adrp that gave its page still names it.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tls.s -o tls.obj
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   main.obj tls.obj -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s

## The template holds strong at 8 and its guard at 12, and far past 64 KiB.
## _tls_index is at 0x140003004, the records at 0x140002000 (far's) and
## 0x140002010.

## far's offset does not fit 16 bits, so its load reads the record, whose
## load is then a nop, the load from the record's page taking its offset in
## the page.
# CHECK:      <.text>:
# CHECK-NEXT: ret
# CHECK-NEXT: ret
# CHECK-NEXT: adrp x8, 0x140002000
# CHECK-NEXT: nop
# CHECK-NEXT: ldr w8, [x8, #0x8]
# CHECK-NEXT: ret

# CHECK-NEXT: adrp x8, 0x140002000
# CHECK-NEXT: nop
# CHECK-NEXT: ldr x10, [x18, #0x58]
# CHECK-NEXT: adrp x9, 0x140003000
# CHECK-NEXT: mov w11, #0x8
# CHECK-NEXT: mov w12, #0xc
# CHECK-NEXT: ldr w9, [x9, #0x4]
# CHECK-NEXT: ldr x9, [x10, x9, lsl #3]
# CHECK-NEXT: ldr w0, [x9, x11]
# CHECK-NEXT: ldrb w1, [x9, x12]
# CHECK-NEXT: ret

#--- main.s
  .text
  .globl main
main:
  ret

  .section .text$strong,"xr"
  .globl direct
direct:
  adrp x8, "__imp_strong$tls"
.La:
  .linkfusedread
  ldr x8, [x8, :lo12:"__imp_strong$tls"]
  ldr x10, [x18, #88]
  .linkfusedaccess .La, x8
.Lb:
  .linkfusedread
  ldr x9, [x8]
  .linkfusedaccess .La, x8
  ldr w11, [x8, #8]
  .linkfusedaccess .La, x8
  ldr w12, [x8, #12]
  .linkfusedaccess .Lb, x9
  ldr w9, [x9]
  ldr x9, [x10, x9, lsl #3]
  ldr w0, [x9, x11]
  ldrb w1, [x9, x12]
  ret

  .section .text$far,"xr"
  .globl faraccess
faraccess:
  adrp x8, "__imp_far$tls"
.Lc:
  .linkfusedread
  ldr x8, [x8, :lo12:"__imp_far$tls"]
  .linkfusedaccess .Lc, x8
  ldr w8, [x8, #8]
  ret

#--- tls.s
  .section .tls$,"dw"
  .p2align 2
  .word 0, 0
  .globl strong
strong:
  .word 1
  .globl _ZGV6strong
_ZGV6strong:
  .byte 0
  .p2align 2
  .zero 65536
  .globl far
far:
  .word 2

  .text
  .globl _ZTH6strong
_ZTH6strong:
  ret

  .data
  .word 0
  .globl _tls_index
_tls_index:
  .word 0
