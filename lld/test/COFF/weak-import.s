# REQUIRES: x86

## A compiler reaches an extern_weak X through __imp_X and keeps X a weak
## external of the object. Under -import-slots an import library's offer
## satisfies the reference, as a shared object satisfies an ELF weak
## reference, and static data holding X's address holds the import's; an
## archive member defining X does not, since a weak external loads no
## archive member. An absent X reads zero, and a reference that reads its
## pointer is not reported as an imported local.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-lib -out:lib.lib lib.obj
# RUN: lld-link -dll -noentry -out:dll.dll -implib:dll.lib dll.obj \
# RUN:   -export:f -export:v,DATA

# RUN: lld-link -import-slots -entry:main -subsystem:console -out:a.exe \
# RUN:   main.obj dll.lib lib.lib 2>&1 | count 0
# RUN: llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-objdump -d a.exe | FileCheck %s

## The word of data that holds v's address is v's in-place import slot.
# IMPORTS:      Name: dll.dll
# IMPORTS:      Symbol: f
# IMPORTS-NEXT: Symbol: v
# IMPORTS:      Name: dll.dll
# IMPORTS:      ImportAddressTableRVA: 0x3000
# IMPORTS-NEXT: Symbol: v
# IMPORTS-NOT:  Symbol:

## f and v read their import address table entries; the archive's g is not
## loaded, so g is zero, as absent is.
# CHECK:      48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,F:]]
# CHECK-NEXT: ff 15 {{.*}} callq *{{.*}}(%rip) # 0x[[#F]]
# CHECK-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx
# CHECK-NEXT: 48 c7 c2 00 00 00 00 movq $0x0, %rdx
# CHECK-NEXT: 48 83 3d {{.*}} cmpq $0x0, {{.*}}(%rip)
# CHECK-NEXT: c3 retq

## Without -import-slots no member defining g is loaded either.
# RUN: lld-link -entry:main -subsystem:console -out:c.exe main.obj dll.lib \
# RUN:   lib.lib
# RUN: llvm-objdump -d c.exe | FileCheck --check-prefix=NOSLOTS %s

# NOSLOTS: 48 c7 c2 00 00 00 00 movq $0x0, %rdx

## Only under -import-slots is a reference that reads the pointer to an absent
## weak symbol's zero kept out of the locally-defined-import warning.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc absent.s -o absent.obj
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:d.exe \
# RUN:   absent.obj 2>&1 | count 0
# RUN: lld-link -entry:main -subsystem:console -out:e.exe absent.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=WARN %s

# WARN: warning: absent.obj: locally defined symbol imported: .weak.absent.default.main (defined in <internal>) [LNK4217]

#--- main.s
  .text
  .globl main
main:
  movq __imp_f(%rip), %rax
  callq *__imp_f(%rip)
  movq __imp_v(%rip), %rcx
  movq __imp_g(%rip), %rdx
  cmpq $0, __imp_absent(%rip)
  retq

  .data
  .quad v

  .weak f
  .weak v
  .weak g
  .weak absent

#--- dll.s
  .text
  .globl f
f:
  retq

  .data
  .globl v
v:
  .long 1

#--- lib.s
  .text
  .globl g
g:
  retq

#--- absent.s
  .text
  .globl main
main:
  cmpq $0, __imp_absent(%rip)
  retq

  .weak absent
