# REQUIRES: aarch64
## The AArch64 form of a DLL's fill of its cells bound by name: the builtins'
## __llvm_fill writes each cell with the address the anchor's entry with the
## default's key names, from a list the linker builds under that key, a
## read-only range preceding the cells it marks read-only.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium l.s -o l.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry l.obj -out:l.dll -implib:l.lib
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj l.lib ntdll.lib \
# RUN:   -out:use.dll
# RUN: llvm-objdump -s -j .rdata use.dll | FileCheck %s

## The list: one read-only range over the import entry at 0x20C0, then the cells
## {key, rva, kind}, each under the default's key, a default bound by name being
## of kind 0: the writable word, then the read-only import entry whose key has
## bit 0 set.
# CHECK:      180002020 01000000 00000000 c0200000 08000000
# CHECK-NEXT: 180002030 d2315e5a e1358eba 00300000 00000000
# CHECK-NEXT: 180002040 d3315e5a e1358eba c0200000 00000000

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

## The image's start-up, which calls its fill directly.
        .section .text$start,"xr"
        .globl __start
__start:
        bl __llvm_run_fill
        ret

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it makes read-only cells writable with.
        .def __llvm_fill
        .scl 2
        .type 32
        .endef
        .section .text,"xr",one_only,__llvm_fill
        .globl __llvm_fill
__llvm_fill:
        adrp x0, __llvm_fill_list
        add x0, x0, :lo12:__llvm_fill_list
        adrp x1, __imp_NtProtectVirtualMemory
        ldr x1, [x1, :lo12:__imp_NtProtectVirtualMemory]
        blr x1
        ret

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
