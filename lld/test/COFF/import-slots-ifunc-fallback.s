# REQUIRES: x86
## An indirect function's cell holds its fallback's address, which a base
## relocation moves with the image, until the fill writes what the resolver
## returns; one with no fallback holds zero and takes no relocation. The record
## keeps the fallback.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ifunc.s -o ifunc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry -opt:ref -export:use ifunc.obj \
# RUN:   crt.obj ntdll.lib -out:use.dll -map:use.map
# RUN: llvm-readobj --sections --coff-basereloc use.dll > use.txt
# RUN: llvm-objdump -s -j .sealed use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s
# RUN: FileCheck --check-prefix=MAP --input-file=use.map %s

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x18
# CHECK-NEXT: VirtualAddress: 0x[[#%X,SEALED:]]

## f's cell, after the done word, is relocated; g's is not.
# CHECK:      BaseReloc [
# CHECK:        Address: 0x[[#%X,SEALED+8]]
# CHECK-NOT:    Address: 0x[[#%X,SEALED+0x10]]
# CHECK:      ]

## f's cell holds the fallback's address; g's zero.
# CHECK:      Contents of section .sealed:
# CHECK-NEXT: [[#%x,0x180000000+SEALED]] 00000000 00000000 {{[0-9a-f]+}} 01000000
# CHECK-NEXT: [[#%x,0x180000010+SEALED]] 00000000 00000000 ........

## The fallback that only the record names is kept.
# MAP: fallback_f

#--- ifunc.s
  .section .text,"xr",one_only,resolve_f
  .globl resolve_f
resolve_f:
  leaq fallback_f(%rip), %rax
  retq

  .section .text,"xr",one_only,fallback_f
  .globl fallback_f
fallback_f:
  retq

  .section .text,"xr",one_only,f
  .globl f
  .p2align 4
f:
  jmpq *__imp_f(%rip)
  .linkifunc f, resolve_f
  .linkifuncfallback f, fallback_f

  .section .text,"xr",one_only,g
  .globl g
  .p2align 4
g:
  jmpq *__imp_g(%rip)
  .linkifunc g, resolve_f

  .section .text$use,"xr"
  .globl use
use:
  callq f
  callq g
  retq

#--- crt.s
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq

  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  leaq __llvm_fill_list(%rip), %rax
  callq *__imp_NtProtectVirtualMemory(%rip)
  retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory
