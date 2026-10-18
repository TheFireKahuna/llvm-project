# REQUIRES: aarch64
## On ARM64, where the record of a thread-local variable is in the image and
## the loads from it are described, the code that loaded its words takes what
## they hold, in the same instructions: the load of the address of the TLS
## index becomes adrp of the index's page, each load through it taking the
## index's offset in the page, and the load of the variable's or its guard's
## offset goes, each instruction that added it to another register taking it
## as its immediate, or becomes movz of it where one does not take it. The
## record's load becomes a nop. Where the TLS template fits 16 bits, so that
## the movz always does, the record the image writes is left out and the adrp
## of its page gives the index's instead; with a larger template the record
## stays, and an offset past 16 bits that no instruction takes is read from it.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tls.s -o tls.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium far.s -o far.obj
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   main.obj tls.obj -out:small.exe
# RUN: llvm-readobj --sections small.exe > small.txt
# RUN: llvm-objdump -d small.exe >> small.txt
# RUN: FileCheck --check-prefix=SMALL --input-file=small.txt %s
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   main.obj tls.obj far.obj -out:large.exe
# RUN: llvm-objdump -d large.exe | FileCheck --check-prefix=LARGE %s

## The template holds strong at 8 and its guard at 12, and in large.exe far
## past 64 KiB. _tls_index is at 0x140002004 in small.exe, which has no
## .rdata.
# SMALL-NOT:  Name: .rdata
# SMALL:      <.text>:
# SMALL-NEXT: ret
# SMALL-NEXT: ret
# SMALL-NEXT: adrp x8, 0x140002000
# SMALL-NEXT: nop
# SMALL-NEXT: ldr x10, [x18, #0x58]
# SMALL-NEXT: adrp x9, 0x140002000
# SMALL-NEXT: nop
# SMALL-NEXT: mov w12, #0xc
# SMALL-NEXT: ldr w9, [x9, #0x4]
# SMALL-NEXT: ldr x9, [x10, x9, lsl #3]
# SMALL-NEXT: ldr w0, [x9, #0x8]
# SMALL-NEXT: add x2, x9, #0x8
# SMALL-NEXT: ldrb w1, [x9, x12]
# SMALL-NEXT: str w12, [sp]
# SMALL-NEXT: ret

## far's offset does not fit 16 bits, so its load reads the record, at
## 0x140002000, whose load is then a nop, the load from the record's page
## taking its offset in the page. strong's offset still folds.
# LARGE:      <.text>:
# LARGE-NEXT: ret
# LARGE-NEXT: ret
# LARGE-NEXT: adrp x8, 0x140002000
# LARGE-NEXT: nop
# LARGE-NEXT: ldr w8, [x8, #0x8]
# LARGE-NEXT: ret
# LARGE-NEXT: adrp x8, 0x140002000
# LARGE-NEXT: nop
# LARGE-NEXT: ldr x10, [x18, #0x58]
# LARGE-NEXT: adrp x9, 0x140003000
# LARGE-NEXT: nop
# LARGE-NEXT: mov w12, #0xc
# LARGE-NEXT: ldr w9, [x9, #0x4]
# LARGE-NEXT: ldr x9, [x10, x9, lsl #3]
# LARGE-NEXT: ldr w0, [x9, #0x8]
# LARGE-NEXT: add x2, x9, #0x8

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
.Lc:
  .linkfusedread
  ldr w11, [x8, #8]
  .linkfusedaccess .La, x8
  ldr w12, [x8, #12]
  .linkfusedaccess .Lb, x9
  ldr w9, [x9]
  ldr x9, [x10, x9, lsl #3]
  .linkfusedindex .Lc, x11
  ldr w0, [x9, x11]
  .linkfusedindex .Lc, x11
  add x2, x11, x9
  ldrb w1, [x9, x12]
  str w12, [sp]
  ret

#--- far.s
  .section .text$far,"xr"
  .globl faraccess
faraccess:
  adrp x8, "__imp_far$tls"
.Lf:
  .linkfusedread
  ldr x8, [x8, :lo12:"__imp_far$tls"]
  .linkfusedaccess .Lf, x8
  ldr w8, [x8, #8]
  ret

  .section .tls$,"dw"
  .p2align 2
  .zero 65536
  .globl far
far:
  .word 2

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

  .text
  .globl _ZTH6strong
_ZTH6strong:
  ret

  .data
  .word 0
  .globl _tls_index
_tls_index:
  .word 0
