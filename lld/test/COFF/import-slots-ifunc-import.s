# REQUIRES: x86
## Under -import-slots, a DLL exports each indirect function it exports with
## the function's cell, as X$cell. An importer's calls of the function read a
## copy of the cell, a word of .sealed that the fill writes with the word the
## import of X$cell holds; until then the copy holds a thunk through the
## function's import, which reaches the stub, and a base relocation moves it.
## A call or jump through the import reads the copy, as does the import's
## thunk; an address load keeps the import.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ifunc.s -o ifunc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s -o helper.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

## The exporter.
# RUN: lld-link -import-slots -dll -noentry -export:f ifunc.obj crt.obj \
# RUN:   ntdll.lib -out:lib.dll -implib:lib.lib
# RUN: llvm-readobj --coff-exports lib.dll | FileCheck --check-prefix=EXPORTS %s
# RUN: llvm-nm lib.lib | FileCheck --check-prefix=IMPLIB %s
# EXPORTS:      Name: f
# EXPORTS-NEXT: RVA: 0x[[#%X,STUB:]]
# EXPORTS:      Name: f$cell
# EXPORTS-NEXT: RVA: 0x[[#%X,CELL:]]
# IMPLIB: D __imp_f$cell

## The importer.
# RUN: lld-link -import-slots -dll -noentry -export:use use.obj crt.obj \
# RUN:   lib.lib ntdll.lib -out:use.dll -map:use.map
# RUN: llvm-readobj --sections --coff-basereloc use.dll > use.txt
# RUN: llvm-objdump -d --no-show-raw-insn use.dll >> use.txt
# RUN: llvm-objdump -s -j .sealed -j .rdata use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s
# RUN: FileCheck --check-prefix=MAP --input-file=use.map %s

# MAP: __imp_f$cell

# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x[[#%X,SEALED:]]

## The copy, after the done word, is relocated.
# CHECK:      BaseReloc [
# CHECK:        Address: 0x[[#%X,SEALED+8]]
# CHECK:      ]

# CHECK:      <use>:
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,0x180000008+SEALED]]
# CHECK-NEXT: callq 0x[[#%x,THUNK:]]
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#%x,IMP:]]
# CHECK-NEXT: jmpq *{{.*}}(%rip) # 0x[[#%x,0x180000008+SEALED]]

## The import's thunk reads the copy; the copy's first value reads the import.
# CHECK:      [[#%x,THUNK]]: jmpq *{{.*}}(%rip) # 0x[[#%x,0x180000008+SEALED]]
# CHECK:      [[#%x,EARLY:]]: jmpq *{{.*}}(%rip) # 0x[[#%x,IMP]]

## The copy holds an address in the image, the early thunk's, which only the
## copy names.
# CHECK:      Contents of section .sealed:
# CHECK-NEXT: [[#%x,0x180000000+SEALED]] 00000000 00000000 {{[0-9a-f]+}} 01000000

## Delay-loaded, the cell is written only once a call loads the DLL, so the
## importer keeps calling through the import.
# RUN: lld-link -import-slots -dll -noentry -export:use use.obj crt.obj \
# RUN:   helper.obj lib.lib ntdll.lib -delayload:lib.dll -out:delay.dll \
# RUN:   -map:delay.map
# RUN: FileCheck --check-prefix=NOCOPY --input-file=delay.map %s
# NOCOPY-NOT: $cell

## So too for an executable's cell, which its start-up writes after the DLLs
## it loads have run their fills.
# RUN: lld-link -def:host.def -out:host.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry -export:use use.obj crt.obj \
# RUN:   host.lib ntdll.lib -out:plugin.dll -map:plugin.map
# RUN: FileCheck --check-prefix=NOCOPY --input-file=plugin.map %s

#--- ifunc.s
  .section .text,"xr",one_only,resolve_f
  .globl resolve_f
resolve_f:
  leaq impl(%rip), %rax
  retq

  .section .text,"xr",one_only,impl
  .globl impl
impl:
  retq

  .section .text,"xr",one_only,f
  .globl f
  .p2align 4
f:
  jmpq *__imp_f(%rip)
  .linkifunc f, resolve_f

#--- use.s
  .section .text$use,"xr"
  .globl use
use:
  callq *__imp_f(%rip)
  callq f
  movq __imp_f(%rip), %rax
  rex64 jmpq *__imp_f(%rip)

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

#--- helper.s
  .section .text$helper,"xr"
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- host.def
LIBRARY host.exe
EXPORTS
  f
  f$cell DATA
