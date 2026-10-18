# REQUIRES: aarch64
## The ARM64 form of the image's fill: each residual word of its list, {word,
## import, addend}, is written from its import's entry plus its addend, and
## the read-only ones, in .sealed, are made read-only through
## NtProtectVirtualMemory from a frame that its unwind information describes,
## whose status the fill returns.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rw.s -o rw.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn out.dll > out.txt
# RUN: llvm-objdump -s -j .rdata out.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s
# RUN: llvm-readobj --sections --unwind out.dll | \
# RUN:   FileCheck --check-prefix=IMAGE %s
# RUN: llvm-objdump -s -j .CRT out.dll | FileCheck --check-prefix=TABLE %s

# CHECK:      180001000: stp x29, x30, [sp, #-0x90]!
# CHECK-NEXT:            mov x29, sp
# CHECK-NEXT:            stp x19, x20, [sp, #0x10]
# CHECK-NEXT:            stp x21, x22, [sp, #0x20]
# CHECK-NEXT:            stp x23, x24, [sp, #0x30]
# CHECK-NEXT:            stp x25, x26, [sp, #0x40]
# CHECK-NEXT:            str x27, [sp, #0x50]
# CHECK-NEXT:            mov w24, #0x0
# CHECK-NEXT:            adrp x27, 0x180000000
# CHECK-NEXT:            add x27, x27, #0x0
# CHECK-NEXT:            adrp x19, 0x[[#%x,PAGE:]]
# CHECK-NEXT:            add x19, x19, #0x[[#%x,OFF:]]
# CHECK-NEXT:            adrp x20, 0x[[#%x,PAGE]]
# CHECK-NEXT:            add x20, x20, #0x[[#%x,OFF+0x30]]
# CHECK-NEXT:            cmp x19, x20
# CHECK-NEXT:            b.hs
# CHECK-NEXT:            ldr w9, [x19, #0x4]
# CHECK-NEXT:            ldr x10, [x27, x9]
# CHECK-NEXT:            ldr x11, [x19, #0x8]
# CHECK-NEXT:            add x10, x10, x11
# CHECK-NEXT:            ldr w9, [x19]
# CHECK-NEXT:            str x10, [x27, x9]
# CHECK-NEXT:            add x19, x19, #0x10
# CHECK-NEXT:            b
# CHECK-NEXT:            mov w0, #0x0
# CHECK-NEXT:            mov w19, w0
# CHECK-NEXT:            cbnz w19,
# CHECK-NEXT:            adrp x0, 0x180006000
# CHECK-NEXT:            add x0, x0, #0x0
# CHECK-NEXT:            str x0, [sp, #0x58]
# CHECK-NEXT:            mov x1, #0x10
# CHECK-NEXT:            movk x1, #0x0, lsl #16
# CHECK-NEXT:            str x1, [sp, #0x60]
# CHECK-NEXT:            mov w3, #0x2
# CHECK-NEXT:            mov x0, #-0x1
# CHECK-NEXT:            add x1, sp, #0x58
# CHECK-NEXT:            add x2, sp, #0x60
# CHECK-NEXT:            add x4, sp, #0x68
# CHECK-NEXT:            adrp x16, 0x180002000
# CHECK-NEXT:            ldr x16, [x16, #0xa0]
# CHECK-NEXT:            blr x16
# CHECK-NEXT:            mov w19, w0
# CHECK-NEXT:            mov w0, w19
# CHECK-NEXT:            ldr x27, [sp, #0x50]
# CHECK-NEXT:            ldp x25, x26, [sp, #0x40]
# CHECK-NEXT:            ldp x23, x24, [sp, #0x30]
# CHECK-NEXT:            ldp x21, x22, [sp, #0x20]
# CHECK-NEXT:            ldp x19, x20, [sp, #0x10]
# CHECK-NEXT:            mov sp, x29
# CHECK-NEXT:            ldp x29, x30, [sp], #0x90
# CHECK-NEXT:            ret

## The words: two in .sealed, arr + 12 and arr - 8, and the one in .data,
## arr + 0x100002345, each from arr's entry at 0x2090.
# CHECK:      [[#%x,PAGE+OFF]] 00600000 90200000 0c000000 00000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x10]] 08600000 90200000 f8ffffff ffffffff
# CHECK-NEXT: [[#%x,PAGE+OFF+0x20]] 00300000 90200000 45230000 01000000

# IMAGE:      Name: .sealed
# IMAGE-NEXT: VirtualSize: 0x10
# IMAGE:      IMAGE_SCN_MEM_WRITE
# IMAGE:      RuntimeFunction {
# IMAGE-NEXT:   Function: 0x180001000
# IMAGE-NEXT:   ExceptionRecord: 0x180002{{[0-9A-F]+}}
# IMAGE-NEXT:   ExceptionData {
# IMAGE-NEXT:     FunctionLength: 204
# IMAGE-NEXT:     Version: 0
# IMAGE-NEXT:     ExceptionData: No
# IMAGE-NEXT:     EpiloguePacked: Yes
# IMAGE-NEXT:     EpilogueOffset: 0
# IMAGE-NEXT:     ByteCodeLength: 16
# IMAGE-NEXT:     Prologue [
# IMAGE-NEXT:       0xd20a ; str x27, [sp, #80]
# IMAGE-NEXT:       0xc988 ; stp x25, x26, [sp, #64]
# IMAGE-NEXT:       0xc906 ; stp x23, x24, [sp, #48]
# IMAGE-NEXT:       0xc884 ; stp x21, x22, [sp, #32]
# IMAGE-NEXT:       0xc802 ; stp x19, x20, [sp, #16]
# IMAGE-NEXT:       0xe1 ; mov fp, sp
# IMAGE-NEXT:       0x91 ; stp x29, x30, [sp, #-144]!
# IMAGE-NEXT:       0xe4 ; end
# IMAGE-NEXT:     ]

# TABLE: 11111111 11111111 00100080 01000000

## With only writable words, the fill does not seal.
# RUN: lld-link -dll -noentry -import-slots -out:leaf.dll crt.obj rw.obj \
# RUN:   lib.lib
# RUN: llvm-objdump -d --no-show-raw-insn leaf.dll | \
# RUN:   FileCheck --check-prefix=LEAF %s

# LEAF:      mov w0, #0x0
# LEAF-NEXT: mov w19, w0
# LEAF-NEXT: mov w0, w19
# LEAF-NEXT: ldr x27, [sp, #0x50]

#--- lib.def
LIBRARY lib.dll
EXPORTS
  arr DATA

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .xword 0x1111111111111111
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .xword 0

#--- ro.s
  .section .rdata,"dr"
  .p2align 3
  .xword arr+12
  .xword arr-8

#--- rw.s
  .data
  .p2align 3
  .xword arr+0x100002345
