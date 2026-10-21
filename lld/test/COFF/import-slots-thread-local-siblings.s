# REQUIRES: x86
## Under -import-slots, a DLL keeps a vague thread-local variable it offers
## preemptible as it keeps other vague data, through its record: its loads of
## the record's pointer read a cell, which its fill writes with the record of
## the program's instance. An executable that sees two DLLs keep the variable
## preemptible picks its owner, imports the owner's record, and names the
## record in its table's block.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium vague.s -o vague.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium check.s -o check.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry a.obj vague.obj crt.obj ntdll.lib \
# RUN:   -out:a.dll
# RUN: lld-link -import-slots -dll -noentry b.obj vague.obj crt.obj ntdll.lib \
# RUN:   -out:b.dll
# RUN: llvm-objdump -d b.dll | FileCheck --check-prefix=CELL %s
# CELL:      <fb>:
# CELL-NEXT: movq {{.*}}(%rip), %rax

## With -Bsymbolic, the load reaches the DLL's own record directly.
# RUN: lld-link -import-slots -dll -noentry b.obj vague.obj crt.obj ntdll.lib \
# RUN:   -Bsymbolic -out:local.dll
# RUN: llvm-objdump -d local.dll | FileCheck --check-prefix=LOCAL %s
# LOCAL:      <fb>:
# LOCAL-NEXT: leaq {{.*}}(%rip), %rax

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   check.obj a.lib b.lib -out:m.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=OWNER %s
# RUN: llvm-readobj --coff-imports m.exe | FileCheck --check-prefix=IMPORT %s
# OWNER:       Importing _ZGV1x$tls from a.dll, which owns it for the program
# OWNER:       Importing x$tls from a.dll, which owns it for the program
# IMPORT:      Name: a.dll
# IMPORT:      Symbol: _ZGV1x$tls
# IMPORT:      Symbol: fa
# IMPORT:      Symbol: x$tls
# IMPORT:      Name: b.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA:
# IMPORT-NEXT: Symbol: fb
# IMPORT-NEXT: }

#--- vague.s
## An inline thread-local variable and its guard, as clang emits them.
  .linkbindings
  .section .tls$,"dw",discard,x
  .globl x
  .p2align 2
x:
  .linkbinding x, default
  .long 1

  .section .tls$,"dw",discard,_ZGV1x
  .globl _ZGV1x
  .p2align 3
_ZGV1x:
  .linkbinding _ZGV1x, default
  .quad 0

  .data
  .globl _tls_index
_tls_index:
  .long 0

#--- a.s
  .text
  .globl fa
fa:
  movq "__imp_x$tls"(%rip), %rax
  movq "__imp__ZGV1x$tls"(%rip), %rcx
  retq
  .section .drectve,"yni"
  .ascii " -export:fa"

#--- b.s
  .text
  .globl fb
fb:
  movq "__imp_x$tls"(%rip), %rax
  movq "__imp__ZGV1x$tls"(%rip), %rcx
  retq
  .section .drectve,"yni"
  .ascii " -export:fb"

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## The image's start-up, which calls its fill directly.
  .section .text$start,"xr"
  .globl __start
__start:
  callq __llvm_run_fill
  retq

## A stand-in for the builtins' __llvm_fill, which writes the vague cells.
  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  leaq __llvm_fill_list(%rip), %rax
  retq

#--- check.s
## Stands in for the C runtime's validation of the table, which the link
## names.
  .data
  .globl __ovr_check
__ovr_check:
  .quad 0

#--- main.s
  .text
  .globl main
main:
  callq *__imp_fa(%rip)
  callq *__imp_fb(%rip)
  retq

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  LdrAddRefDll
  LdrGetProcedureAddressForCaller
  NtProtectVirtualMemory
