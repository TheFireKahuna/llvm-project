# REQUIRES: x86

## A call, jump or pointer load through the import pointer of a symbol defined
## in the image, in an object that describes its instruction sites, becomes the
## direct instruction of the same length. A jump becomes jmp rel32 at its first
## byte, followed by int3, so that the Windows unwinder still recognises the
## epilogue it ends. A pointer that only such references read is left out of
## the image, and "locally defined symbol imported" is reported only for the
## references that still read a pointer.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc other.s -o other.obj

# RUN: lld-link -entry:main -subsystem:console -opt:ref -out:a.exe \
# RUN:   main.obj defs.obj 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d a.exe | FileCheck %s
# RUN: llvm-readobj --coff-basereloc a.exe | FileCheck --check-prefix=RELOC %s

## A reference that is not a described call, jump or load keeps the pointer.
# WARN-NOT: locally defined symbol imported: f
# WARN-DAG: warning: main.obj: locally defined symbol imported: g (defined in defs.obj) [LNK4217]
# WARN-DAG: warning: main.obj: locally defined symbol imported: h (defined in defs.obj) [LNK4217]
# WARN-NOT: locally defined symbol imported: f

# CHECK:      67 e8 {{.*}} addr32 callq 0x[[#%x,F:]]
# CHECK-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#F]]
# CHECK-NEXT: 4c 8d 0d {{.*}} leaq {{.*}}(%rip), %r9 # 0x[[#F]]
# CHECK-NEXT: 48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#%x,G:]]
# CHECK-NEXT: 48 8d 0d {{.*}} leaq {{.*}}(%rip), %rcx
# CHECK-NEXT: e9 {{.*}} jmp 0x[[#F]]
# CHECK-NEXT: cc int3
# CHECK-NEXT: e9 {{.*}} jmp 0x[[#F]]
# CHECK-NEXT: cc int3
# CHECK-NEXT: cc int3
# CHECK:      [[#%x,G]]: c3 retq
# CHECK:      [[#%x,F]]: c3 retq

## The pointers to g and h remain, and the .quad of the pointer to h; there is
## no pointer to f.
# RELOC-COUNT-3: Type: DIR64
# RELOC-NOT:     Type: DIR64

## An object that does not describe its sites keeps every pointer it reads,
## and its instructions as they are.
# RUN: lld-link -entry:main -subsystem:console -out:b.exe main.obj defs.obj \
# RUN:   other.obj 2>&1 | FileCheck --check-prefix=WARN-OTHER %s
# RUN: llvm-objdump -d b.exe | FileCheck --check-prefix=OTHER %s

# WARN-OTHER: warning: other.obj: locally defined symbol imported: f (defined in defs.obj) [LNK4217]

# OTHER:      67 e8 {{.*}} addr32 callq
# OTHER:      ff 15 {{.*}} callq *{{.*}}(%rip)

#--- main.s
  .text
  .globl main
main:
  callq *__imp_f(%rip)
  movq __imp_f(%rip), %rax
  movq __imp_f(%rip), %r9
  movq __imp_g(%rip), %rax
  leaq __imp_g(%rip), %rcx
  jmpq *__imp_f(%rip)

  .globl tail
tail:
  rex64 jmpq *__imp_f(%rip)

  .data
  .quad __imp_h

#--- defs.s
  .section .text$f,"xr",discard,f
  .globl f
f:
  ret

  .text
  .globl g, h
g:
  ret
h:
  ret

#--- other.s
  .text
  .globl other
other:
  callq *__imp_f(%rip)
