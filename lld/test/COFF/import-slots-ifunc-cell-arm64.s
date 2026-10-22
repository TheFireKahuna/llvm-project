# REQUIRES: aarch64
## On ARM64 an indirect function's cell is read by the stub and by a load of
## it whose register only the calls the object describes read, so that the
## call reaches the chosen function without the stub's jump; where every load
## of the cell in a section is such a load, the adrp and loads stay. Any other
## load takes the stub's address, with its adrp.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium ifunc.s -o ifunc.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry -export:call \
# RUN:   -export:addr ifunc.obj use.obj crt.obj ntdll.lib -out:use.dll \
# RUN:   -map:use.map
# RUN: cat use.map > use.txt
# RUN: llvm-readobj --sections use.dll >> use.txt
# RUN: llvm-objdump -d --no-show-raw-insn use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s

# CHECK:      {{ }}f [[#%x,F:]] ifunc.obj
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x[[#%X,SEALED:]]

## The stub reads f's cell, after the done word.
# CHECK:      [[#%x,F]]: adrp x16, 0x[[#%x,0x180000000+SEALED]]
# CHECK-NEXT: ldr x16, [x16, #0x8]
# CHECK-NEXT: br x16

## The address load becomes the stub's address.
# CHECK:      <addr>:
# CHECK-NEXT: adrp x0, 0x[[#%x,mul(div(F,0x1000),0x1000)]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,sub(F,mul(div(F,0x1000),0x1000))]]

## The call reads the cell, as the stub does.
# CHECK:      <call>:
# CHECK-NEXT: adrp x8, 0x[[#%x,0x180000000+SEALED]]
# CHECK-NEXT: ldr x8, [x8, #0x8]
# CHECK-NEXT: blr x8

#--- ifunc.s
  .section .text,"xr",one_only,resolve_f
  .globl resolve_f
resolve_f:
  adrp x0, impl
  add x0, x0, :lo12:impl
  ret

  .section .text,"xr",one_only,impl
  .globl impl
impl:
  ret

  .section .text,"xr",one_only,f
  .globl f
  .p2align 4
f:
  adrp x16, __imp_f
  ldr x16, [x16, :lo12:__imp_f]
  br x16
  .linkifunc f, resolve_f

#--- use.s
  .section .text$call,"xr"
  .globl call
call:
  adrp x8, __imp_f
.Lfused0:
  .linkfusedread
  ldr x8, [x8, :lo12:__imp_f]
  .linkfusedcall .Lfused0, x8
  blr x8
  ret

  .section .text$addr,"xr"
  .globl addr
addr:
  adrp x0, __imp_f
  ldr x0, [x0, :lo12:__imp_f]
  ret

#--- crt.s
## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  bl __llvm_run_fill
  ret

## A stand-in for the builtins' __llvm_fill: it loads its list and references
## the NtProtectVirtualMemory it opens .sealed with.
  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  adrp x0, __llvm_fill_list
  add x0, x0, :lo12:__llvm_fill_list
  adrp x16, __imp_NtProtectVirtualMemory
  ldr x16, [x16, :lo12:__imp_NtProtectVirtualMemory]
  br x16

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory
