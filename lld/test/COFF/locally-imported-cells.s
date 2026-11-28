# REQUIRES: x86, aarch64

## The scan keeps a local import pointer only for data, reading, untyped or
## foreign references, reports LNK4217 for a foreign object's alone, and on
## x86-64 binds .refptr cells, dropping one only rewritten loads read.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab

## Every fork reference is rewritten: no pointer, no warning.
# RUN: %{mc} -triple=x86_64-windows-itanium typed.s -o typed.obj
# RUN: %{mc} -triple=x86_64-windows-itanium defs.s -o defs.obj
# RUN: %{link} -out:typed.exe typed.obj defs.obj 2>&1 | count 0
# RUN: llvm-readobj --sections typed.exe | FileCheck --check-prefix=NORDATA %s
# NORDATA:      Name: .rdata
# NORDATA-NEXT: VirtualSize: 0x18

## A pointer only a discarded section names goes with it and its target.
# RUN: %{mc} -triple=x86_64-windows-itanium dead.s -o dead.obj
# RUN: %{link} -opt:ref -out:dead.exe dead.obj 2>&1 | count 0
# RUN: llvm-readobj --sections dead.exe | FileCheck --check-prefix=NORDATA %s

## A data reference keeps the pointer, with no warning, and so does an
## untyped reference from a fork object; a foreign one is reported.
# RUN: %{mc} -triple=x86_64-windows-itanium data.s -o data.obj
# RUN: %{link} -out:data.exe typed.obj data.obj defs.obj 2>&1 | count 0
# RUN: llvm-readobj --sections data.exe | FileCheck --check-prefix=PTR %s
# RUN: %{mc} -triple=x86_64-windows-itanium untyped.s -o untyped.obj
# RUN: %{mc} -triple=x86_64-windows-msvc untyped.s -o foreign.obj
# RUN: %{link} -out:untyped.exe typed.obj untyped.obj defs.obj 2>&1 | count 0
# RUN: llvm-readobj --sections untyped.exe | FileCheck --check-prefix=PTR %s
# RUN: %{link} -out:foreign.exe typed.obj foreign.obj defs.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN-FOREIGN %s
# RUN: llvm-objdump -d foreign.exe | FileCheck --check-prefix=MIXED %s

## The pointer, then the empty offer table.
# PTR:      Name: .rdata
# PTR-NEXT: VirtualSize: 0x20{{$}}

# WARN-FOREIGN:     warning: foreign.obj: locally defined symbol imported: f (defined in defs.obj) [LNK4217]
# WARN-FOREIGN-NOT: typed.obj:

## The foreign call reads the pointer; the typed one beside it goes direct.
# MIXED-LABEL: <main>:
# MIXED:       : 67 e8 {{.*}} callq {{.*}} <f>
# MIXED-LABEL: <g>:
# MIXED-NEXT:  ff 15 {{.*}} callq *{{.*}}(%rip) {{.*}} <__imp_f>

## A jump's pointer to a weak reference's null default stays, unreported: the
## default is no local definition.
# RUN: %{mc} -triple=x86_64-windows-itanium weak.s -o weak.obj
# RUN: %{link} -out:weak.exe weak.obj 2>&1 | count 0
# RUN: llvm-objdump -d weak.exe | FileCheck --check-prefix=WEAK %s
# RUN: llvm-readobj --sections weak.exe | FileCheck --check-prefix=PTR %s

# WEAK-LABEL: <main>:
# WEAK-NEXT:  ff 25 {{.*}} jmpq *{{.*}}(%rip)

## .refptr cells: the one only rewritten loads read goes; another name keeps
## the other. Neither is reported.
# RUN: %{mc} -triple=x86_64-windows-itanium refptr.s -o refptr.obj
# RUN: %{link} -out:refptr.exe refptr.obj defs.obj 2>&1 | count 0
# RUN: llvm-objdump -d refptr.exe | FileCheck --check-prefix=REFPTR %s
# RUN: llvm-readobj --sections refptr.exe | FileCheck --check-prefix=PTR %s

# REFPTR-LABEL: <main>:
# REFPTR-NEXT:  48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax {{.*}} <d>
# REFPTR-NEXT:  48 8d 0d {{.*}} leaq {{.*}}(%rip), %rcx {{.*}} <e>

## On ARM64 a data reference keeps the pointer too while loads reach the
## symbol, unreported.
# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium defs.s -o defs-a64.obj
# RUN: %{link} -out:a64.exe a64.obj defs-a64.obj 2>&1 | count 0
# RUN: llvm-objdump -d a64.exe | FileCheck --check-prefix=A64 %s
# RUN: llvm-readobj --sections a64.exe | FileCheck --check-prefix=PTR %s

# A64-LABEL: <main>:
# A64-NEXT:  adr x0, {{.*}} <d>

#--- typed.s
.globl main
main:
  callq *__imp_f(%rip)
  movq __imp_d(%rip), %rax
  ret

#--- dead.s
.globl main
main:
  ret
.section .text$unused,"xr",discard,unused
.globl unused
unused:
  cmpq $0, __imp_gone(%rip)
  ret
.section .text$gone,"xr",discard,gone
.globl gone
gone:
  ret

#--- data.s
.data
.quad __imp_d

#--- untyped.s
.globl g
g:
  callq *__imp_f(%rip)
  cmpq $0, __imp_d(%rip)
  ret

#--- weak.s
.globl main
main:
  jmpq *__imp_w(%rip)
.weak w

#--- defs.s
.globl f
f:
  ret
.data
.globl d, e
d:
  .long 7
e:
  .long 8

#--- refptr.s
.globl main
main:
  movq .refptr.d(%rip), %rax
  movq .refptr.e(%rip), %rcx
  ret
.section .rdata$.refptr.d,"dr",discard,.refptr.d
.globl .refptr.d
.refptr.d:
  .quad d
.section .rdata$.refptr.e,"dr",discard,.refptr.e
.globl .refptr.e, e_alias
.refptr.e:
e_alias:
  .quad e

#--- a64.s
.globl main
main:
  adrp x0, __imp_d
  ldr x0, [x0, :lo12:__imp_d]
  ret
.data
.quad __imp_d
