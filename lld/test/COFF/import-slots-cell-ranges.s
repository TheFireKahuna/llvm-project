# REQUIRES: x86
## Under -import-slots, a DLL's fill list names one read-only range per output
## section that holds read-only cells, {rva, size} each, so the fill makes each
## writable and read-only again on its own: here the pointer its import-form
## reference reads, in .rdata, and a word of .myro, which follows .data. One
## range across both would leave .data, between them, read-only.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll
# RUN: llvm-readobj --sections a.dll > a.txt
# RUN: llvm-objdump -d --print-imm-hex a.dll >> a.txt
# RUN: llvm-objdump -s -j .rdata a.dll >> a.txt
# RUN: FileCheck --input-file=a.txt %s

# CHECK:      Name: .data
# CHECK:      Name: .myro
# CHECK-NEXT: VirtualSize: 0x8
# CHECK-NEXT: VirtualAddress: 0x[[#%x,MYRO:]]

## __llvm_fill loads its list.
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]

## The list header names two read-only ranges and two cells; then, after the
## 40-byte header, the ranges, {rva, size}: the pointer's in .rdata, then
## .myro's at 0x5000, kept apart so .data between them stays read-only.
# CHECK:      [[#%x,LIST]]      02000000 02000000 00000000 00000000
# CHECK-NEXT: [[#%x,LIST+0x10]] 00000000 00000000 {{[0-9a-f]+ [0-9a-f]+}}
# CHECK-NEXT: [[#%x,LIST+0x20]] 01000000 00000000 a0200000 08000000
# CHECK-NEXT: [[#%x,LIST+0x30]] 00500000 08000000 {{.*}}

## Merged into .rdata, the two cells share one range, which the list repeats
## just once.
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib \
# RUN:   -merge:.myro=.rdata -out:merged.dll
# RUN: llvm-objdump -d --print-imm-hex merged.dll > merged.txt
# RUN: llvm-objdump -s -j .rdata merged.dll >> merged.txt
# RUN: FileCheck --check-prefix=MERGED --input-file=merged.txt %s
# MERGED:      leaq {{.*}}(%rip), %rax # 0x[[#%x,LIST:]]
# MERGED:      [[#%x,LIST]]      01000000 02000000 00000000 00000000
# MERGED-NEXT: [[#%x,LIST+0x10]] 00000000 00000000 {{[0-9a-f]+ [0-9a-f]+}}
# MERGED-NEXT: [[#%x,LIST+0x20]] 01000000 00000000 a0200000 d8000000
# MERGED-NEXT: [[#%x,LIST+0x30]] 00000000 00000000 {{.*}}

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
  movq __imp__ZN1n1gE(%rip), %rax
  retq

  .section .myro,"dr"
  .p2align 3
  .quad _ZN1n1gE
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
## the NtProtectVirtualMemory it makes read-only ranges writable with.
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
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
