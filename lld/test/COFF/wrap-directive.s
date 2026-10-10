# REQUIRES: x86

## /wrap in an object's .drectve section wraps as -wrap on the command line
## does, whichever input comes first, and an object loaded from an archive
## after the references still renames them. A dllimport reference to the
## wrapped function reaches the wrapper, whether a DLL exports it or an
## archive defines it, and the import of the original is left out.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc start.s -o start.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s -o wrap.obj
# RUN: llvm-lib -machine:x64 -def:crt.def -out:crt.lib
# RUN: lld-link -dll -noentry -out:rt.dll wrap.obj -export:__wrap_exit \
# RUN:   -implib:rt.lib
# RUN: llvm-lib -out:start.lib start.obj
# RUN: llvm-lib -out:wrap.lib wrap.obj

## The wrapper exported by a DLL, with the directive in an object read after
## the import library, and in an archive member that the entry point loads.
# RUN: lld-link -entry:start -subsystem:console -out:a.exe main.obj crt.lib \
# RUN:   start.obj rt.lib
# RUN: llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=DLL %s
# RUN: lld-link -entry:start -subsystem:console -out:b.exe main.obj crt.lib \
# RUN:   start.lib rt.lib
# RUN: llvm-readobj --coff-imports b.exe | FileCheck --check-prefix=DLL %s

# DLL-NOT:     Name: crt.dll
# DLL:         Name: rt.dll
# DLL-NEXT:    ImportLookupTableRVA:
# DLL-NEXT:    ImportAddressTableRVA:
# DLL-NEXT:    Symbol: __wrap_exit
# DLL-NOT:     Name: crt.dll

## The wrapper in an archive.
# RUN: lld-link -entry:start -subsystem:console -out:c.exe main.obj crt.lib \
# RUN:   start.lib wrap.lib -debug:symtab -opt:ref
# RUN: llvm-readobj --coff-imports c.exe | FileCheck --check-prefix=STATIC \
# RUN:   --allow-empty %s
# RUN: llvm-nm c.exe | FileCheck --check-prefix=STATIC-SYMS %s

## The directive in an object read before the import library.
# RUN: lld-link -entry:start -subsystem:console -out:d.exe start.obj \
# RUN:   main.obj crt.lib wrap.lib -debug:symtab -opt:ref
# RUN: llvm-readobj --coff-imports d.exe | FileCheck --check-prefix=STATIC \
# RUN:   --allow-empty %s
# RUN: llvm-nm d.exe | FileCheck --check-prefix=STATIC-SYMS %s

# STATIC-NOT: Name: crt.dll

## Without garbage collection too, since nothing reaches the import.
# RUN: lld-link -entry:start -subsystem:console -out:e.exe main.obj crt.lib \
# RUN:   start.lib wrap.lib -opt:noref
# RUN: llvm-readobj --coff-imports e.exe | FileCheck --check-prefix=STATIC \
# RUN:   --allow-empty %s

# STATIC-SYMS: R __imp___wrap_exit
# STATIC-SYMS: T __wrap_exit

#--- main.s
  .text
  .globl main
main:
  callq *__imp_exit(%rip)
  retq

#--- start.s
  .text
  .globl start
start:
  callq main
  callq *__imp_exit(%rip)
  retq
  .section .drectve,"yn"
  .ascii " /wrap:exit"

#--- wrap.s
  .text
  .globl __wrap_exit
__wrap_exit:
  retq

#--- crt.def
LIBRARY crt.dll
EXPORTS
exit
