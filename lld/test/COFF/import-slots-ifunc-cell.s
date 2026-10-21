# REQUIRES: x86
## Under -import-slots, an indirect function, which a link record pairs with
## its resolver, is a stub that jumps through its import pointer, its cell: a
## word of .sealed that holds zero until the fill writes what the resolver
## returns, listed as a word whose source RVA carries kind 3. The stub's own
## jump, and a call or jump through the pointer elsewhere, read the cell; any
## other reference takes the stub's address. The record keeps the resolver.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ifunc.s -o ifunc.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry -opt:ref -export:use ifunc.obj \
# RUN:   use.obj crt.obj ntdll.lib -out:use.dll -map:use.map 2>&1 \
# RUN:   | FileCheck --allow-empty --check-prefix=WARN %s
# RUN: llvm-readobj --sections use.dll > use.txt
# RUN: llvm-objdump -d --no-show-raw-insn use.dll >> use.txt
# RUN: llvm-objdump -s -j .rdata use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s
# RUN: FileCheck --check-prefix=MAP --input-file=use.map %s

## Reading the cell is the design, so the import is not reported.
# WARN-NOT: locally defined symbol imported

## .sealed: the done word, then the cells of f and g.
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x18
# CHECK-NEXT: VirtualAddress: 0x[[#%X,SEALED:]]
# CHECK:      Characteristics [
# CHECK-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# CHECK-NEXT:   IMAGE_SCN_MEM_READ
# CHECK-NEXT: ]

## Each stub jumps through its own cell.
# CHECK:      [[#%x,F:]]: jmpq *{{.*}}(%rip) # 0x[[#%x,0x180000000+SEALED+8]]
# CHECK:      jmpq *{{.*}}(%rip) # 0x[[#%x,0x180000000+SEALED+0x10]]
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]

## The call in use reads f's cell, and its address load becomes the address
## of f's stub.
# CHECK:      <use>:
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#%x,0x180000000+SEALED+8]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rax # 0x[[#%x,F]]

## The list: no ranges or cells, two words, no run-time cells; after the flag
## words, each cell's word with its resolver's RVA | 3 << 30.
# CHECK:      [[#%x,LIST]] 00000000 00000000 02000000 00000000
# CHECK:      [[#%x,LIST+0x20]] 00000000 00000000 {{[0-9a-f]+}} {{[0-9a-f]+}}c0
# CHECK-NEXT: [[#%x,LIST+0x30]] 00000000 00000000 {{[0-9a-f]+}} {{[0-9a-f]+}}c0

## The resolver that only the record names is kept.
# MAP: resolve_g

## The record names the resolver, so one the image lacks is undefined.
# RUN: not lld-link -import-slots -dll -noentry -export:use bad.obj crt.obj \
# RUN:   ntdll.lib -out:bad.dll 2>&1 | FileCheck --check-prefix=UNDEF %s
# UNDEF: error: undefined symbol: elsewhere

## Without -import-slots nothing would write a cell.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium start.s -o start.obj
# RUN: not lld-link -dll -noentry -export:use ifunc.obj use.obj start.obj \
# RUN:   -out:noslots.dll 2>&1 | FileCheck --check-prefix=NOSLOTS %s
# NOSLOTS: error: ifunc.obj: defines the indirect function f, whose cell only a link with -import-slots writes

#--- ifunc.s
  .section .text,"xr",one_only,resolve_f
  .globl resolve_f
resolve_f:
  leaq impl(%rip), %rax
  retq

  .section .text,"xr",one_only,resolve_g
  .globl resolve_g
resolve_g:
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

  .section .text,"xr",one_only,g
  .globl g
  .p2align 4
g:
  jmpq *__imp_g(%rip)
  .linkifunc g, resolve_g

#--- use.s
  .section .text$use,"xr"
  .globl use
use:
  callq *__imp_f(%rip)
  movq __imp_f(%rip), %rax
  callq g
  retq

#--- bad.s
  .section .text,"xr",one_only,h
  .globl h
h:
  jmpq *__imp_h(%rip)
  .linkifunc h, elsewhere

  .section .text$use,"xr"
  .globl use
use:
  callq h
  retq

#--- crt.s
## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it opens .sealed with.
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

#--- start.s
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory
