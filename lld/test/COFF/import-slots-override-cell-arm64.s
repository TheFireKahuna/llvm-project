# REQUIRES: aarch64
## The AArch64 form of a DLL's fill of its overridable-default cells, which its
## C initializer table runs: it reads the executable's image base through the
## PEB and the anchor's mask, writes a writable cell of a replaced slot in place,
## and writes the read-only ones between two calls of NtProtectVirtualMemory,
## from a frame that its unwind information describes. It returns 0, or the
## status of a failed protection call.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium records.s -o records.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: llvm-lib -machine:arm64 -out:records.lib records.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib records.lib \
# RUN:   ntdll.lib -out:use.dll
# RUN: llvm-objdump -d --no-show-raw-insn use.dll | FileCheck %s
# RUN: llvm-readobj --unwind use.dll | FileCheck --check-prefix=UNWIND %s

# CHECK:      stp     x29, x30, [sp, #-0x30]!
# CHECK-NEXT: mov     x29, sp
# CHECK-NEXT: ldr     x16, [x18, #0x60]
# CHECK-NEXT: ldr     x16, [x16, #0x10]
# CHECK-NEXT: ldr     w17, [x16, #0x80]
# CHECK-NEXT: mov     w9, #0xc2d5
# CHECK-NEXT: movk    w9, #0x7b3a, lsl #16
# CHECK-NEXT: cmp     w17, w9
# CHECK-NEXT: b.ne    0x[[ZERO:[0-9a-f]+]]
# CHECK-NEXT: ldr     w10, [x16, #0x88]
# CHECK-NEXT: add     x10, x16, x10
# CHECK-NEXT: ldr     w10, [x10, #0x8]
# CHECK-NEXT: tbz     w10, #0x0,
# CHECK-NEXT: ldr     w17, [x16, #0x44]
# CHECK-NEXT: add     x17, x16, x17
# CHECK-NEXT: adrp    x9, 0x180003000
# CHECK-NEXT: add     x9, x9, #0x0
# CHECK-NEXT: str     x17, [x9]
# CHECK-NEXT: mov     w9, #0x1
# CHECK-NEXT: movk    w9, #0x0, lsl #16
# CHECK-NEXT: tst     w10, w9
# CHECK-NEXT: b.eq    0x[[ZERO]]
# CHECK-NEXT: adrp    x0, 0x180002000
# CHECK-NEXT: add     x0, x0, #0x[[RO:[0-9a-f]+]]
# CHECK-NEXT: str     x0, [sp, #0x10]
# CHECK-NEXT: mov     x1, #0x8
# CHECK-NEXT: movk    x1, #0x0, lsl #16
# CHECK-NEXT: str     x1, [sp, #0x18]
# CHECK-NEXT: mov     x0, #-0x1
# CHECK-NEXT: add     x1, sp, #0x10
# CHECK-NEXT: add     x2, sp, #0x18
# CHECK-NEXT: mov     w3, #0x4
# CHECK-NEXT: add     x4, sp, #0x20
# CHECK-NEXT: adrp    x16, 0x180002000
# CHECK-NEXT: ldr     x16, [x16, #0x[[PROTECT:[0-9a-f]+]]]
# CHECK-NEXT: blr     x16
# CHECK-NEXT: cbnz    w0, 0x[[DONE:[0-9a-f]+]]
# CHECK-NEXT: ldr     x16, [x18, #0x60]
# CHECK-NEXT: ldr     x16, [x16, #0x10]
# CHECK-NEXT: ldr     w10, [x16, #0x88]
# CHECK-NEXT: add     x10, x16, x10
# CHECK-NEXT: ldr     w10, [x10, #0x8]
# CHECK-NEXT: tbz     w10, #0x0,
# CHECK-NEXT: ldr     w17, [x16, #0x44]
# CHECK-NEXT: add     x17, x16, x17
# CHECK-NEXT: adrp    x9, 0x180002000
# CHECK-NEXT: add     x9, x9, #0x[[RO]]
# CHECK-NEXT: str     x17, [x9]
# CHECK-NEXT: ldr     w3, [sp, #0x20]
# CHECK-NEXT: mov     x0, #-0x1
# CHECK-NEXT: add     x1, sp, #0x10
# CHECK-NEXT: add     x2, sp, #0x18
# CHECK-NEXT: add     x4, sp, #0x20
# CHECK-NEXT: adrp    x16, 0x180002000
# CHECK-NEXT: ldr     x16, [x16, #0x[[PROTECT]]]
# CHECK-NEXT: blr     x16
# CHECK-NEXT: b       0x[[DONE]]
# CHECK-NEXT: [[ZERO]]: mov w0, #0x0
# CHECK-NEXT: [[DONE]]: mov sp, x29
# CHECK-NEXT: ldp     x29, x30, [sp], #0x30
# CHECK-NEXT: ret
## The call's thunk reads the read-only cell.
# CHECK-NEXT: adrp    x16, 0x180002000
# CHECK-NEXT: ldr     x16, [x16, #0x[[RO]]]
# CHECK-NEXT: br      x16

# UNWIND:      FunctionLength: 244
# UNWIND:      EpiloguePacked: Yes
# UNWIND:      0xe1                ; mov fp, sp
# UNWIND-NEXT: 0x85                ; stp x29, x30, [sp, #-48]!

## A thread-local cell is also written in the loading thread's block, at its
## offset in the template, through the index the TLS directory names.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -dll -noentry tl.obj crt.obj rt.lib records.lib \
# RUN:   -out:tl.dll
# RUN: llvm-objdump -d --no-show-raw-insn tl.dll | FileCheck --check-prefix=TL %s
# TL:      str     x17, [x9]
# TL-NEXT: adrp    x9, 0x180003000
# TL-NEXT: ldr     w9, [x9]
# TL-NEXT: ldr     x11, [x18, #0x58]
# TL-NEXT: ldr     x11, [x11, x9, lsl #3]
# TL-NEXT: mov     w12, #0x8
# TL-NEXT: movk    w12, #0x0, lsl #16
# TL-NEXT: str     x17, [x11, x12]
# TL-NEXT: mov     w0, #0x0
# TL-NEXT: ret

#--- rt.s
        .text
        .globl _Znwy
_Znwy:
        ret
        .section .drectve,"yni"
        .ascii " /EXPORT:_Znwy"

#--- records.s
        .globl __ovr_version
        .set __ovr_version, 1
        .globl "__ovr_slot$_Znwy"
        .set "__ovr_slot$_Znwy", 0

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
