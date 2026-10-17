# REQUIRES: aarch64
## The ARM64 form of the residual fill: each word is written from its import's
## entry, and the read-only ones, in .sealed, are made read-only through
## NtProtectVirtualMemory from a frame that its unwind information describes,
## whose status the fill returns. An addend outside an add's immediate is built
## in a register.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rw.s -o rw.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-objdump -d --no-show-raw-insn out.dll | FileCheck %s
# RUN: llvm-readobj --sections --unwind out.dll | \
# RUN:   FileCheck --check-prefix=IMAGE %s
# RUN: llvm-objdump -s -j .CRT out.dll | FileCheck --check-prefix=TABLE %s

# CHECK:      180001000: stp x29, x30, [sp, #-0x30]!
# CHECK-NEXT:            mov x29, sp
# CHECK-NEXT:            adrp x16, 0x180002000
# CHECK-NEXT:            ldr x16, [x16, #0x{{[0-9a-f]+}}]
# CHECK-NEXT:            add x16, x16, #0xc
# CHECK-NEXT:            adrp x17, 0x18000{{[0-9a-f]+}}
# CHECK-NEXT:            add x17, x17, #0x0
# CHECK-NEXT:            str x16, [x17]
# CHECK-NEXT:            adrp x16, 0x180002000
# CHECK-NEXT:            ldr x16, [x16, #0x{{[0-9a-f]+}}]
# CHECK-NEXT:            sub x16, x16, #0x8
# CHECK-NEXT:            adrp x17, 0x18000{{[0-9a-f]+}}
# CHECK-NEXT:            add x17, x17, #0x8
# CHECK-NEXT:            str x16, [x17]
# CHECK-NEXT:            adrp x16, 0x180002000
# CHECK-NEXT:            ldr x16, [x16, #0x{{[0-9a-f]+}}]
# CHECK-NEXT:            mov x17, #0x2345
# CHECK-NEXT:            movk x17, #0x1, lsl #32
# CHECK-NEXT:            add x16, x16, x17
# CHECK-NEXT:            adrp x17, 0x18000{{[0-9a-f]+}}
# CHECK-NEXT:            add x17, x17, #0x0
# CHECK-NEXT:            str x16, [x17]
# CHECK-NEXT:            adrp x0, 0x18000{{[0-9a-f]+}}
# CHECK-NEXT:            add x0, x0, #0x0
# CHECK-NEXT:            str x0, [sp, #0x10]
# CHECK-NEXT:            mov x1, #0x10
# CHECK-NEXT:            movk x1, #0x0, lsl #16
# CHECK-NEXT:            str x1, [sp, #0x18]
# CHECK-NEXT:            mov x0, #-0x1
# CHECK-NEXT:            add x1, sp, #0x10
# CHECK-NEXT:            add x2, sp, #0x18
# CHECK-NEXT:            mov w3, #0x2
# CHECK-NEXT:            add x4, sp, #0x20
# CHECK-NEXT:            adrp x16, 0x180002000
# CHECK-NEXT:            ldr x16, [x16, #0x{{[0-9a-f]+}}]
# CHECK-NEXT:            blr x16
# CHECK-NEXT:            mov sp, x29
# CHECK-NEXT:            ldp x29, x30, [sp], #0x30
# CHECK-NEXT:            ret

# IMAGE:      Name: .sealed
# IMAGE-NEXT: VirtualSize: 0x10
# IMAGE:      IMAGE_SCN_MEM_WRITE
# IMAGE:      RuntimeFunction {
# IMAGE-NEXT:   Function: 0x180001000
# IMAGE-NEXT:   ExceptionRecord: 0x180002{{[0-9A-F]+}}
# IMAGE-NEXT:   ExceptionData {
# IMAGE-NEXT:     FunctionLength: 156
# IMAGE-NEXT:     Version: 0
# IMAGE-NEXT:     ExceptionData: No
# IMAGE-NEXT:     EpiloguePacked: Yes
# IMAGE-NEXT:     EpilogueOffset: 0
# IMAGE-NEXT:     ByteCodeLength: 4
# IMAGE-NEXT:     Prologue [
# IMAGE-NEXT:       0xe1 ; mov fp, sp
# IMAGE-NEXT:       0x85 ; stp x29, x30, [sp, #-48]!
# IMAGE-NEXT:       0xe4 ; end
# IMAGE-NEXT:     ]

# TABLE: 11111111 11111111 00100080 01000000

## With only writable words, the fill is a leaf that returns 0.
# RUN: lld-link -dll -noentry -import-slots -out:leaf.dll crt.obj rw.obj \
# RUN:   lib.lib
# RUN: llvm-objdump -d --no-show-raw-insn leaf.dll | \
# RUN:   FileCheck --check-prefix=LEAF %s

# LEAF:      180001000: adrp x16, 0x180002000
# LEAF-NEXT:            ldr x16, [x16, #0x{{[0-9a-f]+}}]
# LEAF-NEXT:            mov x17, #0x2345
# LEAF-NEXT:            movk x17, #0x1, lsl #32
# LEAF-NEXT:            add x16, x16, x17
# LEAF-NEXT:            adrp x17, 0x18000{{[0-9a-f]+}}
# LEAF-NEXT:            add x17, x17, #0x0
# LEAF-NEXT:            str x16, [x17]
# LEAF-NEXT:            mov w0, #0x0
# LEAF-NEXT:            ret

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
