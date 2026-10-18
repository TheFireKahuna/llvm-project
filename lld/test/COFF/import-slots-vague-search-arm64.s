# REQUIRES: aarch64
## On AArch64, as on x86-64, a DLL's own references to a vague entity it keeps
## preemptible read a cell, which the fill of its overridable-default cells, the
## builtins' __llvm_fill, writes from the word the executable's table names for
## the entity's key, and which a search of the loaded modules writes. The DLL
## names its offer table in its MZ header at 0x30.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll
# RUN: od -A x -t x4 -j 0x30 -N 8 a.dll > a.txt
# RUN: llvm-readobj --coff-imports a.dll >> a.txt
# RUN: llvm-objdump -d a.dll >> a.txt
# RUN: FileCheck --input-file=a.txt %s

## The MZ header names the offer table.
# CHECK:      000030 6c8e9cf5 {{[0-9a-f]+}}

## The search pins a module it binds to with LdrAddRefDll and seals with
## NtProtectVirtualMemory.
# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,NTDLL:]]
# CHECK-NEXT: Symbol: LdrAddRefDll
# CHECK-NEXT: Symbol: NtProtectVirtualMemory

## The reference reads the cell.
# CHECK:      <fa>:
# CHECK-NEXT: adrp x0
# CHECK-NEXT: ldr x0, [x0

## The fill's list holds the entity's cell, read-only, keyed by the entity and
## of a kind that is the RVA of the name its export holds, never zero, by which
## the search binds it.
# RUN: llvm-objdump -s -j .rdata a.dll | FileCheck --check-prefix=BIND %s
# BIND:      {{.*}} b75fec27 d80a3472
# BIND-NEXT: {{[0-9a-f]+}} {{[0-9a-f]*[2-9a-f][0-9a-f]*}}

#--- a.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl fa
fa:
  adrp x0, __imp__ZN1n1gE
  ldr x0, [x0, :lo12:__imp__ZN1n1gE]
  ret
  .section .drectve,"yni"
  .ascii " -export:fa"

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the ntdll functions it pins a searched module and seals with.
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
  adrp x2, __imp_LdrAddRefDll
  ldr x2, [x2, :lo12:__imp_LdrAddRefDll]
  blr x1
  ret

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
