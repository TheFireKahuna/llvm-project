# REQUIRES: x86
## Under -import-slots, a DLL keeps the mutable vague data it offers
## preemptible, as an ELF shared object reaches its own default-visibility data
## through its GOT: its import-form references and the words of its static
## data that hold the data's address read cells, which its fill writes from
## the word that the executable's override table names for the entity, and its
## import library's record lists the entity as preemptible. An executable that
## sees two images keep an entity preemptible, true siblings that no import
## path joins, picks its owner in breadth-first load order, itself first,
## a delay-loaded DLL after every eagerly loaded one, imports the owner's
## instance, and names the entity in its table's block with a word that holds
## the instance's address. -Bsymbolic keeps a DLL's references local, and the
## pair is then reported as before.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium keep.s -o keep.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium check.s -o check.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64

# RUN: lld-link -import-slots -dll -noentry a.obj crt.obj ntdll.lib -out:a.dll
# RUN: lld-link -import-slots -dll -noentry b.obj crt.obj ntdll.lib -out:b.dll
# RUN: llvm-objdump -s -j .rdata b.dll > b.txt
# RUN: llvm-objdump -d --print-imm-hex b.dll >> b.txt
# RUN: FileCheck --check-prefix=CELL --input-file=b.txt %s
## b's own reference reads its cell, which its fill writes from the word the
## executable's block names for the entity's key: its list holds the cell,
## read-only, with the name its export holds.
# CELL:      b75fec27 d80a3472 {{[0-9a-f]+}} {{[0-9a-f]*[2-9a-f][0-9a-f]*}}
# CELL:      <fb>:
# CELL-NEXT: movq {{.*}}(%rip), %rax
# CELL:      movq 0x8(%rax), %r13
# CELL-NEXT: cmpl $0x1, 0xc(%rbx)
# CELL-NEXT: je
# CELL-NEXT: cmpl $0x1000, 0xc(%rbx)
# CELL-NEXT: jb
# CELL-NEXT: movq (%r13), %r13

## The executable picks a.dll, first in link order, and imports its instance.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   check.obj a.lib b.lib -out:m.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=OWNER %s
# RUN: llvm-readobj --coff-imports m.exe | FileCheck --check-prefix=IMPORT %s
# RUN: od -A x -t x4 -j 0x80 -N 4 m.exe | FileCheck --check-prefix=TABLE %s
## Immutable data too, whose address is the program's one.
# OWNER:  Importing _ZN1n1gE from a.dll, which owns it for the program
# OWNER:  Importing _ZN1n1kE from a.dll, which owns it for the program
# IMPORT:      Name: a.dll
# IMPORT:      Symbol: _ZN1n1gE
# IMPORT:      Symbol: _ZN1n1kE
# TABLE: 000080 7b3ac2d5

## The executable owns an entity it keeps, here as it takes no delay-loaded
## DLL's offer, and imports none.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   keep.obj check.obj a.lib b.lib -delayload:a.dll -delayload:b.dll \
# RUN:   -out:k.exe
# RUN: llvm-readobj --coff-imports k.exe | FileCheck --check-prefix=KEEP %s
# RUN: od -A x -t x4 -j 0x80 -N 4 k.exe | FileCheck --check-prefix=TABLE %s
# KEEP-NOT: Symbol: _ZN1n1gE

## A delay-loaded DLL owns nothing that an eagerly loaded image keeps.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   check.obj b.lib a.lib -delayload:b.dll -out:d.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=OWNER %s

## Nothing is imported for an entity only delay-loaded DLLs keep: the first
## to load keeps its instance, which the other's search finds.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   check.obj a.lib b.lib -delayload:a.dll -delayload:b.dll -out:l.exe
# RUN: llvm-readobj --coff-imports l.exe | FileCheck --check-prefix=KEEP %s

## A DLL linked -Bsymbolic keeps its copy, and the pair is reported.
# RUN: mkdir bsym
# RUN: lld-link -import-slots -dll -noentry b.obj crt.obj ntdll.lib \
# RUN:   -out:bsym/b.dll -Bsymbolic
# RUN: llvm-objdump -d bsym/b.dll | FileCheck --check-prefix=LOCAL %s
# LOCAL:      <fb>:
# LOCAL-NEXT: leaq
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   a.lib bsym/b.lib -out:e.exe 2>&1 | FileCheck --check-prefix=ERROR %s
# ERROR: error: _ZN1n1gE is defined by both a.dll and b.dll, neither of which imports from the other

#--- a.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1
  .section .rdata,"dr",discard,_ZN1n1kE
  .globl _ZN1n1kE
_ZN1n1kE:
  .linkbinding _ZN1n1kE, default
  .long 5

  .text
  .globl fa
fa:
  movq __imp__ZN1n1gE(%rip), %rax
  movq __imp__ZN1n1kE(%rip), %rcx
  retq
  .section .drectve,"yni"
  .ascii " -export:fa"

#--- b.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1
  .section .rdata,"dr",discard,_ZN1n1kE
  .globl _ZN1n1kE
_ZN1n1kE:
  .linkbinding _ZN1n1kE, default
  .long 5

  .text
  .globl fb
fb:
  movq __imp__ZN1n1gE(%rip), %rax
  movq __imp__ZN1n1kE(%rip), %rcx
  retq
  .section .drectve,"yni"
  .ascii " -export:fb"

#--- keep.s
  .linkbindings
  .section .data,"dw",discard,_ZN1n1gE
  .globl _ZN1n1gE
_ZN1n1gE:
  .linkbinding _ZN1n1gE, default
  .long 1

  .text
  .globl keep
keep:
  movq __imp__ZN1n1gE(%rip), %rax
  retq

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

#--- check.s
## Stands in for the C runtime's validation of the table, which the link
## names, and its delay-load helper.
  .data
  .globl __ovr_check
__ovr_check:
  .quad 0
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

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
