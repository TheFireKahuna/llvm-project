# REQUIRES: x86
## Under -import-slots, a DLL that offers COMDAT data preempts the image's copy,
## as an ELF shared object's definition preempts an executable's copy of
## vague-linkage data. The variable, the other external symbols of its group
## and its guard, paired by name, resolve to the DLL's imports, and the group's
## sections are discarded with the initializer and its .CRT$XCU entry, whatever
## roots name them. Import-form references load the DLL's entries, a described
## lea of the variable becomes a load of its entry, words of static data become
## in-place slots, and an export of the variable forwards to the DLL.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium guard.s -o guard.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium direct.s \
# RUN:   -o direct.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium helper.s \
# RUN:   -o helper.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium hidden.s \
# RUN:   -o hidden.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -def:noguard.def -out:noguard.lib -machine:x64
# RUN: lld-link -def:other.def -out:other.lib -machine:x64

# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -include:iv -out:out.exe -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d out.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports out.exe | FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-objdump -s -j .data out.exe | FileCheck --check-prefix=DATA %s

# LOG-DAG: Preempting A::v in main.obj with its instance in lib.dll
# LOG-DAG: Preempting _ZN1A1vE$so8 in main.obj with its instance in lib.dll
# LOG-DAG: Preempting iv in main.obj with its instance in lib.dll

# CHECK:      <.text>:
# CHECK-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax # 0x[[#%x,V:]]
# CHECK-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx # 0x[[#V]]
# CHECK-NEXT: 48 8b 15 {{.*}} movq {{.*}}(%rip), %rdx # 0x[[#%x,IV:]]
# CHECK-NEXT: c3 retq
# CHECK-NOT:  12345678

## The guard's entry, which only the discarded initializer used, is left out.
# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA:
# IMPORTS-NEXT: Symbol: _ZN1A1vE (1)
# IMPORTS-NEXT: Symbol: iv (3)
# IMPORTS-NEXT: }
# IMPORTS:      Name: lib.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA: 0x3000
# IMPORTS-NEXT: Symbol: _ZN1A1vE (1)
# IMPORTS-NEXT: Symbol: _ZN1A1vE$so8 (2)
# IMPORTS-NEXT: Symbol: iv (3)
# IMPORTS-NEXT: }

## The words of static data hold the lookup entries until the loader writes
## the addresses in place.
# DATA:      140003000 {{[0-9a-f]+}} 00000000 {{[0-9a-f]+}} 00000000
# DATA-NEXT: 140003010 {{[0-9a-f]+}} 00000000

## The guard is preempted with the variable where the image keeps it, and a
## root does not keep the copy.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   guard.obj lib.lib -include:_ZN1A1vE -out:guard.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=GUARD %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -opt:noref -out:noref.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=GUARD %s
# RUN: llvm-objdump -d noref.exe | FileCheck --check-prefix=NOINIT %s
# GUARD-DAG: Preempting A::v in main.obj with its instance in lib.dll
# GUARD-DAG: Preempting guard variable for A::v in main.obj with its instance in lib.dll
# NOINIT-NOT: 12345678

## An export of a preempted variable, renamed or not, and of the names inside
## it, forwards to the DLL's instance.
# RUN: lld-link -import-slots -dll -noentry main.obj lib.lib \
# RUN:   -export:_ZN1A1vE,data -export:ivx=iv,data -out:fwd.dll
# RUN: llvm-readobj --coff-exports fwd.dll | FileCheck --check-prefix=FWD %s
# FWD:      Name: _ZN1A1vE
# FWD-NEXT: ForwardedTo: lib._ZN1A1vE
# FWD:      Name: _ZN1A1vE$so8
# FWD-NEXT: ForwardedTo: lib._ZN1A1vE$so8
# FWD:      Name: ivx
# FWD-NEXT: ForwardedTo: lib.iv

## A member of the group that the DLL does not offer, or that another DLL
## offers, keeps the group in the image and is an error.
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   guard.obj noguard.lib -out:err.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=NOGUARD %s
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   guard.obj noguard.lib other.lib -out:err.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=OTHER %s
# NOGUARD: error: main.obj: A::v is provided by lib.dll, but guard variable for A::v, which belongs with it, is not provided by lib.dll
# OTHER:   error: main.obj: A::v is provided by lib.dll, but guard variable for A::v, which belongs with it, is provided by other.dll

## A reference that cannot follow the variable to the DLL is an error.
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   direct.obj lib.lib -out:err.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=DIRECT %s
# DIRECT: error: direct.obj: iv is provided by lib.dll, but is referenced as if it were local; recompile the object, or give the symbol hidden visibility

## Data is never delay-bound: the image keeps its copies, which import-form
## references reach directly. -ignore-offers keeps them as well.
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   helper.obj lib.lib -delayload:lib.dll -out:delay.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=DELAYLOG %s
# RUN: llvm-objdump -d delay.exe | FileCheck --check-prefix=KEPT %s
# RUN: llvm-readobj --coff-imports delay.exe | \
# RUN:   FileCheck --check-prefix=NOIMPORTS %s
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -ignore-offers:LIB.dll -out:ignore.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=DELAYLOG %s
# RUN: llvm-objdump -d ignore.exe | FileCheck --check-prefix=KEPT %s
# DELAYLOG-DAG: Binding __imp__ZN1A1vE to A::v in main.obj
# DELAYLOG-DAG: Binding __imp__ZGVN1A1vE to guard variable for A::v in main.obj
# DELAYLOG-NOT: Preempting
# KEPT:      <.text>:
# KEPT-NEXT: 48 8d 05 {{.*}} leaq
# KEPT-NEXT: 48 8d 0d {{.*}} leaq
# KEPT:      12345678
# NOIMPORTS-NOT: lib.dll

## A copy that an object lists no default visibility for is hidden, and stays
## in the image, as an ELF copy with hidden visibility does.
# RUN: lld-link -import-slots -entry:main -subsystem:console hidden.obj \
# RUN:   lib.lib -out:hidden.exe -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=HIDDEN %s
# RUN: llvm-objdump -d hidden.exe | FileCheck --check-prefix=HIDDENCODE %s
# HIDDEN-NOT: Preempting
# HIDDENCODE:      <.text>:
# HIDDENCODE-NEXT: 48 8d 05 {{.*}} leaq

## Without -import-slots, the copy stays.
# RUN: lld-link -entry:main -subsystem:console main.obj lib.lib -out:plain.exe
# RUN: llvm-objdump -d plain.exe | FileCheck --check-prefix=PLAIN %s
# PLAIN: 12345678

#--- lib.def
LIBRARY lib.dll
EXPORTS
  _ZN1A1vE DATA
  _ZGVN1A1vE DATA
  _ZN1A1vE$so8 DATA
  iv DATA

#--- noguard.def
LIBRARY lib.dll
EXPORTS
  _ZN1A1vE DATA
  _ZN1A1vE$so8 DATA

#--- other.def
LIBRARY other.dll
EXPORTS
  _ZGVN1A1vE DATA

#--- main.s
  .section .bss,"bw",discard,_ZN1A1vE
  .globl _ZN1A1vE
  .p2align 3
_ZN1A1vE:
  .zero 16
  .globl "_ZN1A1vE$so8"
  .set "_ZN1A1vE$so8", _ZN1A1vE+8

  .section .bss,"bw",discard,_ZGVN1A1vE
  .globl _ZGVN1A1vE
_ZGVN1A1vE:
  .quad 0

  .section .data,"dw",discard,iv
  .globl iv
iv:
  .long 3

  .section .text,"xr",associative,_ZN1A1vE
init:
  movq __imp__ZGVN1A1vE(%rip), %rax
  movl $0x12345678, %ecx
  movq __imp__ZN1A1vE(%rip), %rdx
  movl %ecx, (%rdx)
  retq

  .section .CRT$XCU,"dr",associative,_ZN1A1vE
  .quad init

  .text
  .globl main
main:
  movq __imp__ZN1A1vE(%rip), %rax
  leaq _ZN1A1vE(%rip), %rcx
  leaq iv(%rip), %rdx
  retq

  .data
  .quad _ZN1A1vE
  .quad _ZN1A1vE+8
  .quad iv

#--- guard.s
## Code that the image keeps references the guard directly.
  .data
  .quad _ZGVN1A1vE

#--- direct.s
  .text
  .globl f
f:
  movl iv(%rip), %eax
  retq

#--- hidden.s
  .linkbindings
  .section .data,"dw",discard,iv
  .globl iv
iv:
  .long 3

  .text
  .globl main
main:
  leaq iv(%rip), %rax
  retq

#--- helper.s
  .text
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq
