# REQUIRES: x86
## Under -import-slots, the loader only reads a lookup table, so identical
## lookup tables are written once, and a table that is the end of another,
## the DLL's own included, is written as that end. A table is not written as
## the start of another, whose terminator would not end it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -import-slots -opt:noref -entry:main -subsystem:console \
# RUN:   main.obj a.lib -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | FileCheck %s

## Every import keeps its entry in the DLL's own table, whose lookup table is
## written first. The tables written are [f1, f2, f3, g] at L, [f1, f2] at
## L+40 and [f2, f1] at L+64.
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L:]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: Symbol: f3 (2)
# CHECK-NEXT: Symbol: g (3)
# CHECK-NEXT: }
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L + 40]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: }
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L + 48]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: }
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L + 72]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: }
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L + 40]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: }
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L + 24]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: g (3)
# CHECK-NEXT: }
# CHECK:      ImportLookupTableRVA: 0x[[#%X,L + 64]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: }

#--- a.def
LIBRARY a.dll
EXPORTS
  f1
  f2
  f3
  g

#--- main.s
  .text
  .globl main
main:
  leaq __imp_f1(%rip), %rax
  leaq __imp_f2(%rip), %rax
  leaq __imp_f3(%rip), %rax
  leaq __imp_g(%rip), %rax
  retq

  .data
  .quad f1
  .quad f2
  .quad 0
  .quad f2
  .quad 0
  .quad f1
  .quad 0
  .quad f1
  .quad f2
  .quad 0
  .quad g
  .quad 0
  .quad f2
  .quad f1
