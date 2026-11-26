# REQUIRES: x86, aarch64

## Slots of one DLL a word apart form a run, one descriptor's address table;
## .rdata and .data sections holding slots are grouped by DLL so runs continue
## across them, other sections keep their place, and -order parts no run.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{roots} = -include:__imp_fa1 -include:__imp_fa2 \
# DEFINE:   -include:__imp_fa3 -include:__imp_fb1
# DEFINE: %{run} = lld-link -machine:%{a} -def:a.def -out:a.lib && \
# DEFINE:   lld-link -machine:%{a} -def:b.def -out:b.lib && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium data.s \
# DEFINE:     -o data.obj && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -opt:noref a.lib b.lib \
# DEFINE:     data.obj -out:runs.dll %{roots} && \
# DEFINE:   llvm-readobj --coff-imports runs.dll | FileCheck %s && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -opt:noref a.lib b.lib \
# DEFINE:     data.obj -out:order.dll -order:@order.txt %{roots} && \
# DEFINE:   llvm-readobj --coff-imports order.dll | FileCheck %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#%X,IAT:]]
# CHECK-NEXT: Symbol: fa1 (0)
# CHECK-NEXT: Symbol: fa2 (1)
# CHECK-NEXT: Symbol: fa3 (2)
# CHECK-NEXT: }
# CHECK:      Name: b.dll
# CHECK:      ImportAddressTableRVA: 0x[[#IAT+32]]
# CHECK-NEXT: Symbol: fb1 (0)
# CHECK-NEXT: }
## .data$z keeps its place: its three slots are two runs.
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#%X,Z:]]
# CHECK-NEXT: Symbol: fa1 (0)
# CHECK-NEXT: Symbol: fa2 (1)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#Z+24]]
# CHECK-NEXT: Symbol: fa3 (2)
# CHECK-NEXT: }
## w2 leads the sections of .data that hold slots, at its end.
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#%X,W:]]
# CHECK-NEXT: Symbol: fa1 (0)
# CHECK-NEXT: }
## p1 and p3 are one run after the tables; p4's alignment may pad, p5 starts
## with a word that is no slot.
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#IAT+48]]
# CHECK-NEXT: Symbol: fa1 (0)
# CHECK-NEXT: Symbol: fa2 (1)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#IAT+64]]
# CHECK-NEXT: Symbol: fa3 (2)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK:      ImportAddressTableRVA: 0x[[#IAT+88]]
# CHECK-NEXT: Symbol: fa1 (0)
# CHECK-NEXT: }
## b.dll's runs follow a.dll's: w1 and w3, then p2.
# CHECK:      Name: b.dll
# CHECK:      ImportAddressTableRVA: 0x[[#W+8]]
# CHECK-NEXT: Symbol: fb1 (0)
# CHECK-NEXT: Symbol: fb1 (0)
# CHECK-NEXT: }
# CHECK:      Name: b.dll
# CHECK:      ImportAddressTableRVA: 0x[[#IAT+96]]
# CHECK-NEXT: Symbol: fb1 (0)
# CHECK-NEXT: }
# CHECK-NOT:  Name:

#--- a.def
LIBRARY a.dll
EXPORTS
  fa1 fa2 fa3

#--- b.def
LIBRARY b.dll
EXPORTS
fb1

#--- order.txt
p3
w3

#--- data.s
.macro sect name, s=.rdata, f=dr
  .section \s,"\f",discard,\name
  .globl \name
\name:
.endm
  sect p1
  .quad fa1
  sect p2
  .quad fb1
  sect p3
  .quad fa2
  sect p4
  .p2align 4
  .quad fa3, 0
  sect p5
  .quad 0, fa1
  sect w1, .data, dw
  .quad fb1
  sect w2, .data, dw
  .quad fa1
  sect w3, .data, dw
  .quad fb1
.data
  .quad 7
.section .data$z,"dw"
  .quad fa1
  .quad fa2
  .quad 5
  .quad fa3
