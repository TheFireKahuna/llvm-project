# REQUIRES: aarch64
## The AArch64 form of a DLL's fill of its cells bound by name: it searches
## the anchor's entries for each key of its list, writes the address an entry
## with the key names into the cells under it, and writes the read-only ones
## between two protection calls, made only once a cell is to be written.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium l.s -o l.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry l.obj -out:l.dll -implib:l.lib
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj l.lib ntdll.lib \
# RUN:   -out:use.dll
# RUN: llvm-objdump -s -j .rdata use.dll > use.txt
# RUN: llvm-objdump -d --no-show-raw-insn use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s

## The list: the writable word and the import's entry, read-only, under the
## default's key.
# CHECK:      d2315e5a e1358eba {{[0-9a-f]+}} 00000000
# CHECK-NEXT: d3315e5a e1358eba {{[0-9a-f]+}} 00000000
## The executable's table, the anchor's entries searched for each key, and on
## a hit the address the entry names; the read-only cell once the first
## protection call has made it writable.
# CHECK:      ldr     x16, [x18, #0x60]
# CHECK-NEXT: ldr     x21, [x16, #0x10]
# CHECK-NEXT: ldr     w9, [x21, #0x80]
# CHECK-NEXT: mov     w10, #0xc2d5
# CHECK-NEXT: movk    w10, #0x7b3a, lsl #16
# CHECK:      ldr     x13, [x11]
# CHECK-NEXT: cmp     x13, x22
# CHECK-NEXT: b.eq
# CHECK:      ldr     w9, [x19]
# CHECK-NEXT: tbz     w9, #0x0,
# CHECK:      mov     w3, #0x4
# CHECK:      blr     x16
# CHECK:      str     x23, [x27, x9]
# CHECK:      ldr     x23, [x11, #0x8]
# CHECK-NEXT: ldr     w9, [x19, #0xc]
# CHECK-NEXT: cbz     w9,

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
