# REQUIRES: aarch64
## The AArch64 form of a DLL that offers an overridable default reaching its own
## definition through the default's import. Calls stay direct on AArch64, so
## the DLL's branches to the definition, its tail branch included, reach the
## import's thunk, whose entry is a cell the fill writes, and its address-take
## loads that entry; the DLL still exports its body under the offer name.
## Without the runtime's import library the definition stays the DLL's own.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium offer.s -o offer.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib

# RUN: lld-link -import-slots -dll -noentry offer.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:offer.dll
# RUN: llvm-readobj --coff-exports --coff-imports offer.dll > offer.txt
# RUN: llvm-objdump -d --no-show-raw-insn offer.dll >> offer.txt
# RUN: FileCheck --input-file=offer.txt %s

# CHECK:      Name: rt.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[ENTRY:[0-9A-F]+]]
# CHECK-NEXT: Symbol: _Znwy
# CHECK:      Name: __ovr_offer$_Znwy
# CHECK-NEXT: RVA: 0x[[#%X,BODY:]]
# CHECK:      <.text>:
# CHECK-NEXT: bl      0x[[#%x,THUNK:]]
# CHECK-NEXT: adrp    x0, 0x[[PAGE:[0-9a-f]+]]
# CHECK-NEXT: ldr     x0, [x0, #0x[[OFF:[0-9a-f]+]]]
# CHECK-NEXT: b       0x[[#THUNK]]
# CHECK:      [[#%x,0x180000000+BODY]] <__ovr_offer$_Znwy>:
# CHECK:      [[#%x,THUNK]]: adrp x16, 0x[[PAGE]]
# CHECK-NEXT: ldr     x16, [x16, #0x[[OFF]]]
# CHECK-NEXT: br      x16

## Without the runtime's import library the branches reach the definition.
# RUN: lld-link -import-slots -dll -noentry offer.obj crt.obj ntdll.lib \
# RUN:   -out:own.dll
# RUN: llvm-readobj --coff-exports own.dll > own.txt
# RUN: llvm-objdump -d --no-show-raw-insn own.dll >> own.txt
# RUN: FileCheck --check-prefix=OWN --input-file=own.txt %s
# OWN:      Name: __ovr_offer$_Znwy
# OWN-NEXT: RVA: 0x[[#%X,OWNBODY:]]
# OWN:      <.text>:
# OWN-NEXT: bl      0x[[#%x,0x180000000+OWNBODY]]

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

#--- offer.s
        .text
        .globl dllmain
dllmain:
        bl _Znwy
        adrp x0, __imp__Znwy
        ldr x0, [x0, :lo12:__imp__Znwy]
        b _Znwy
## A function, whose references keep their relocations, its own unit's
## branches too.
        .def _Znwy; .scl 2; .type 32; .endef
        .globl _Znwy
_Znwy:
        mov x0, #0
        ret
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy /EXPORT:_Znwy"
