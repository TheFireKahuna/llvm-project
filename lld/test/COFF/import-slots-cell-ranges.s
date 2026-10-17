# REQUIRES: x86
## Under -import-slots, a DLL's fill makes each output section's range of its
## read-only cells writable and then read-only again, with protection calls of
## its own: here the pointer its import-form reference reads, in .rdata, and a
## word of .myro, which follows .data. One range across both would leave .data
## read-only.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll
# RUN: llvm-readobj --sections a.dll > a.txt
# RUN: llvm-objdump -d --print-imm-hex a.dll >> a.txt
# RUN: FileCheck --input-file=a.txt %s

# CHECK:      Name: .data
# CHECK:      Name: .myro
# CHECK-NEXT: VirtualSize: 0x8
# CHECK-NEXT: VirtualAddress: 0x[[#%x,MYRO:]]

## At the first read-only cell to write, each range made writable: the
## pointer's, in .rdata, then .myro's, past .data; at the end each made
## read-only again.
# CHECK:      testb $0x1, (%rbx)
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,FIRST:]]
# CHECK-NEXT: movq %rax, 0x30(%rsp)
# CHECK-NEXT: movq $0x8, 0x38(%rsp)
# CHECK-NEXT: movl $0x4, %r9d
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,0x180000000 + MYRO]]
# CHECK-NEXT: movq %rax, 0x30(%rsp)
# CHECK-NEXT: movq $0x8, 0x38(%rsp)
# CHECK-NEXT: movl $0x4, %r9d
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#FIRST]]
# CHECK-NEXT: movq %rax, 0x30(%rsp)
# CHECK-NEXT: movq $0x8, 0x38(%rsp)
# CHECK-NEXT: movl $0x2, %r9d
# CHECK:      leaq {{.*}}(%rip), %rax # 0x[[#%x,0x180000000 + MYRO]]
# CHECK-NEXT: movq %rax, 0x30(%rsp)
# CHECK-NEXT: movq $0x8, 0x38(%rsp)
# CHECK-NEXT: movl $0x2, %r9d

## Merged into .rdata, the word shares the pointer's range, which the second
## range repeats.
# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib \
# RUN:   -merge:.myro=.rdata -out:merged.dll
# RUN: llvm-objdump -d --print-imm-hex merged.dll | \
# RUN:   FileCheck --check-prefix=MERGED %s
# MERGED:      testb $0x1, (%rbx)
# MERGED:      leaq {{.*}}(%rip), %rax # 0x[[#%x,BASE:]]
# MERGED-NEXT: movq %rax, 0x30(%rsp)
# MERGED-NEXT: movq $0x[[#%x,SIZE:]], 0x38(%rsp)
# MERGED-NEXT: movl $0x4, %r9d
# MERGED:      leaq {{.*}}(%rip), %rax # 0x[[#BASE]]
# MERGED-NEXT: movq %rax, 0x30(%rsp)
# MERGED-NEXT: movq $0x[[#SIZE]], 0x38(%rsp)
# MERGED-NEXT: movl $0x4, %r9d

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

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
