# REQUIRES: x86
## Under -import-slots, a definition in the image beats an import library's
## __imp_X, as a definition in an ELF executable beats a shared object's: the
## import-form references reach the definition, a function's import thunk
## takes no name, and nothing is imported. A class's vtable copy is such a
## definition, so constructing the class stores the copy's address. Without
## -import-slots, the import library's function is a duplicate, as with
## link.exe.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -out:out.exe -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d out.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports out.exe | FileCheck --check-prefix=IMPORTS %s

# LOG-DAG: Binding __imp_fn to fn in main.obj
# LOG-DAG: Binding __imp_var to var in main.obj
# LOG-DAG: Binding __imp__ZTV1B to vtable for B in main.obj

# CHECK:      <.text>:
# CHECK-NEXT: c3 retq
# CHECK-NEXT: e8 {{.*}} callq 0x140001000
# CHECK-NEXT: 48 8d 05 {{.*}} leaq
# CHECK-NEXT: 48 8d 0d {{.*}} leaq

# IMPORTS-NOT: lib.dll

# RUN: not lld-link -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -out:plain.exe 2>&1 | FileCheck --check-prefix=DUP %s
# DUP: error: duplicate symbol: fn

#--- lib.def
LIBRARY lib.dll
EXPORTS
  fn
  var DATA
  _ZTV1B DATA

#--- main.s
  .text
  .globl fn
fn:
  retq

  .data
  .globl var
var:
  .long 1

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 3
_ZTV1B:
  .quad 0, 0, fn

  .text
  .globl main
main:
  callq *__imp_fn(%rip)
  movq __imp_var(%rip), %rax
  movq __imp__ZTV1B(%rip), %rcx
  retq
