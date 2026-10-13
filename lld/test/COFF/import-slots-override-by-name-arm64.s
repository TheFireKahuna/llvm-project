# REQUIRES: aarch64
## The AArch64 form of a DLL's fill of its cells of an overridable default
## replaced by name: it searches the anchor's entries, after its slots, for the
## default's key, writes the address an entry with the key names into the cell,
## and writes the read-only cells whenever the table has a block.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium l.s -o l.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry l.obj -out:l.dll -implib:l.lib
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj l.lib ntdll.lib \
# RUN:   -out:use.dll
# RUN: llvm-objdump -d --no-show-raw-insn use.dll | FileCheck %s

# CHECK:      ldr     x16, [x18, #0x60]
# CHECK-NEXT: ldr     x16, [x16, #0x10]
# CHECK-NEXT: ldr     w17, [x16, #0x80]
# CHECK-NEXT: mov     w9, #0xc2d5
# CHECK-NEXT: movk    w9, #0x7b3a, lsl #16
# CHECK-NEXT: cmp     w17, w9
# CHECK-NEXT: b.ne
# CHECK-NEXT: ldr     w10, [x16, #0x88]
# CHECK-NEXT: add     x10, x16, x10
# CHECK-NEXT: ldr     w10, [x10, #0x8]
# CHECK-NEXT: ldr     w11, [x16, #0x88]
# CHECK-NEXT: add     x11, x16, x11
# CHECK-NEXT: ldr     w12, [x11, #0xc]
# CHECK-NEXT: ldrh    w13, [x16, #0x86]
# CHECK-NEXT: add     x11, x11, x13, lsl #3
# CHECK-NEXT: add     x11, x11, #0x10
# CHECK-NEXT: mov     x14, #0x31d2
# CHECK-NEXT: movk    x14, #0x5a5e, lsl #16
# CHECK-NEXT: movk    x14, #0x35e1, lsl #32
# CHECK-NEXT: movk    x14, #0xba8e, lsl #48
# CHECK-NEXT: cbz     w12, 0x[[#%x,NEXT:]]
# CHECK-NEXT: ldr     x13, [x11]
# CHECK-NEXT: cmp     x13, x14
# CHECK-NEXT: b.eq
# CHECK-NEXT: add     x11, x11, #0x10
# CHECK-NEXT: sub     w12, w12, #0x1
# CHECK-NEXT: b
# CHECK-NEXT: ldr     x17, [x11, #0x8]
# CHECK-NEXT: adrp    x9
# CHECK-NEXT: add     x9, x9
# CHECK-NEXT: str     x17, [x9]
# CHECK-NEXT: [[#%x,NEXT]]: ldr w9, [x16, #0x8c]
# CHECK-NEXT: cbz     w9

#--- l.s
        .text
        .weak hook
hook:
        ret
        .section .drectve,"yni"
        .ascii " /OVERRIDABLEBYNAME:hook /EXPORT:hook"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .xword 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .xword 0

#--- use.s
        .text
        .globl dllmain
dllmain:
        bl hook
        ret
        .data
        .globl ghook
ghook:
        .xword hook
        .section .drectve,"yni"
        .ascii " /INCLUDE:ghook"
