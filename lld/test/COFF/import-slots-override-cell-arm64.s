# REQUIRES: aarch64
## The AArch64 form of a DLL's fill of its overridable-default cells, which its
## C initializer table runs: it reads the executable's image base through the
## PEB and the anchor's mask, and for each cell of its list whose slot the mask
## names replaced writes the base plus the slot's RVA, making the read-only
## cells writable before the first and read-only again at its end through
## NtProtectVirtualMemory, from a frame that its unwind information describes.
## It returns 0, or the status of a failed protection call.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:use.dll
# RUN: llvm-objdump -d --no-show-raw-insn use.dll > use.txt
# RUN: llvm-objdump -s -j .rdata use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s
# RUN: llvm-readobj --unwind use.dll | FileCheck --check-prefix=UNWIND %s

# CHECK:      ldr x16, [x18, #0x60]
# CHECK-NEXT: ldr x21, [x16, #0x10]
# CHECK-NEXT: ldr w9, [x21, #0x80]
# CHECK-NEXT: mov w10, #0xc2d5
# CHECK-NEXT: movk w10, #0x7b3a, lsl #16
# CHECK-NEXT: cmp w9, w10
# CHECK-NEXT: csel x21, x21, xzr, eq
# CHECK-NEXT: adrp x19, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x19, x19, #0x[[#%x,OFF:]]
# CHECK-NEXT: adrp x20,
# CHECK-NEXT: add x20, x20, #0x[[#%x,OFF+0x20]]
# CHECK-NEXT: cmp x19, x20
# CHECK-NEXT: b.hs
# CHECK-NEXT: ldr x22, [x19]
# CHECK-NEXT: and x22, x22, #0xfffffffffffffffe
# CHECK-NEXT: cbz x21,
# CHECK-NEXT: ldr w11, [x21, #0x88]
# CHECK-NEXT: add x11, x21, x11
# CHECK-NEXT: ldr w12, [x11, #0x8]
# CHECK-NEXT: lsr x13, x22, #1
# CHECK-NEXT: lsr w12, w12, w13
# CHECK-NEXT: tbz w12, #0x0,
# CHECK-NEXT: lsr x13, x22, #32
# CHECK-NEXT: ldr w13, [x21, x13]
# CHECK-NEXT: add x23, x21, x13
# CHECK-NEXT: b 0x[[#%x,NEXT:]]
# CHECK-NEXT: b
# CHECK-NEXT: [[#%x,NEXT]]: ldr w9, [x19]
# CHECK-NEXT: tbz w9, #0x0, 0x[[#%x,STORE:]]
# CHECK-NEXT: tbnz w24, #0x0, 0x[[#%x,STORE]]
# CHECK-NEXT: orr w24, w24, #0x1
# CHECK-NEXT: adrp x0, 0x180002000
# CHECK-NEXT: add x0, x0, #0x90
# CHECK-NEXT: str x0, [sp, #0x58]
# CHECK-NEXT: mov x1, #0x8
# CHECK-NEXT: movk x1, #0x0, lsl #16
# CHECK-NEXT: str x1, [sp, #0x60]
# CHECK-NEXT: mov w3, #0x4
# CHECK:      blr x16
# CHECK-NEXT: cbnz w0,
# CHECK-NEXT: [[#%x,STORE]]: ldr w9, [x19, #0x8]
# CHECK-NEXT: str x23, [x27, x9]
# CHECK:      tbz w24, #0x0,
# CHECK-NEXT: adrp x0, 0x180002000
# CHECK-NEXT: add x0, x0, #0x90
# CHECK:      mov w3, #0x2

## The cells, {key, rva, kind}: the writable one in .data and the read-only
## import entry, bit 0 of whose key is set, each of slot 0, whose RVA is at
## 0x44 in the executable's header.
# CHECK:      [[#%x,PAGE+OFF]] 00000000 44000000 00300000 02000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x10]] 01000000 44000000 90200000 02000000

# UNWIND:      EpiloguePacked: Yes
# UNWIND:      0xe1                ; mov fp, sp
# UNWIND-NEXT: 0x91                ; stp x29, x30, [sp, #-144]!

## A thread-local cell is also written in the loading thread's block, at its
## offset in the template, through the index the TLS directory names.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -dll -noentry tl.obj crt.obj rt.lib \
# RUN:   -out:tl.dll
# RUN: llvm-objdump -d --no-show-raw-insn tl.dll > tl.txt
# RUN: llvm-objdump -s -j .rdata tl.dll >> tl.txt
# RUN: FileCheck --check-prefix=TL --input-file=tl.txt %s
# TL:      adrp x19, 0x[[#%x,PAGE:]]
# TL-NEXT: add x19, x19, #0x[[#%x,OFF:]]
# TL:      str x23, [x27, x9]
# TL-NEXT: ldr w10, [x19, #0xc]
# TL-NEXT: cmp w10, #0x1, lsl #12
# TL-NEXT: b.hs
# TL-NEXT: tbz w10, #0x2,
# TL-NEXT: adrp x10, 0x180003000
# TL-NEXT: ldr w10, [x10]
# TL-NEXT: ldr x11, [x18, #0x58]
# TL-NEXT: ldr x11, [x11, x10, lsl #3]
# TL-NEXT: mov w12, #0xa000
# TL-NEXT: movk w12, #0xffff, lsl #16
# TL-NEXT: add w9, w9, w12
# TL-NEXT: str x23, [x11, x9]
## The cell, of kind 6, a slot in the template.
# TL:      [[#%x,PAGE+OFF]] 00000000 44000000 08600000 06000000

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        ret
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
## The C initializer table's bounds, as the C runtime provides them, so the
## linker's fill joins the table the runtime's start-up runs.
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
        bl _Znwy
        ret
        .data
        .globl gnew
gnew:
        .xword _Znwy
        .section .rdata,"dr",discard,cnew
        .globl cnew
        .p2align 3
cnew:
        .xword _Znwy

#--- tl.s
        .text
        .globl dllmain
dllmain:
        ret
        .section .tls,"dw"
        .globl _tls_start
_tls_start:
        .xword 0
        .section .tls$,"dw"
        .globl tnew
        .p2align 3
tnew:
        .xword _Znwy
        .section .tls$ZZZ,"dw"
        .globl _tls_end
_tls_end:
        .xword 0
        .data
        .globl _tls_index
_tls_index:
        .word 0
        .section .rdata,"dr"
        .globl _tls_used
        .p2align 3
_tls_used:
        .xword _tls_start
        .xword _tls_end
        .xword _tls_index
        .xword 0
        .word 0
        .word 0
