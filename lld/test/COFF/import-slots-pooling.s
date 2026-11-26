# REQUIRES: x86, aarch64

## With in-place slots every descriptor's lookup table is pooled: equal tables
## are one and a table ending another is its tail; one that only begins
## another, or names another import, is its own.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{run} = lld-link -machine:%{a} -def:a.def -out:a.lib && \
# DEFINE:   lld-link -machine:%{a} -def:b.def -out:b.lib && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium data.s \
# DEFINE:     -o data.obj && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry a.lib b.lib data.obj \
# DEFINE:     -out:pool.dll -include:__imp_f1 -include:__imp_f2 \
# DEFINE:     -include:__imp_f3 && \
# DEFINE:   llvm-readobj --coff-imports pool.dll | FileCheck %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## a.dll's own table.
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x[[#%X,OWN:]]
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,IAT:]]
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: Symbol: f3 (2)
# CHECK-NEXT: }
## b.dll's import is another DLL's. The pool starts with its table.
# CHECK:      Name: b.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x[[#%X,POOL:]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: g1 (0)
# CHECK-NEXT: }
## f3, then f2 and f3, end a.dll's table.
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x[[#OWN+16]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f3 (2)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x[[#OWN+8]]
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: f2 (1)
# CHECK-NEXT: Symbol: f3 (2)
# CHECK-NEXT: }
## f1 alone begins it, and its two runs share one table.
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x[[#%X,F1:]]
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,SLOT:]]
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA: 0x[[#F1]]
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#SLOT+16]]
# CHECK-NEXT: Symbol: f1 (0)
# CHECK-NEXT: }
# CHECK-NOT:  Name:

#--- a.def
LIBRARY a.dll
EXPORTS
  f1 f2 f3

#--- b.def
LIBRARY b.dll
EXPORTS
g1

#--- data.s
.data
  .quad f3
  .quad 0
  .quad f2
  .quad f3
  .quad 0
  .quad f1
  .quad 0
  .quad f1
  .quad 0
  .quad g1
