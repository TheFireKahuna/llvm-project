# REQUIRES: x86
## A member of a joint link imports what it imports from a DLL member by the
## ordinal the merge gives each export, so the loader finds it with no search
## of the DLL's names: the ordinal the DLL's link line gives an export, else
## the lowest no export of the DLL has, in the order of their names. The names
## stay for images outside the set, which import by name.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium c.s -o c.obj
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry c.obj a.lib -out:c.dll
# RUN: llvm-readobj --coff-exports a.dll > out.txt
# RUN: llvm-readobj --coff-imports b.dll c.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s

# CHECK:      Ordinal: 1
# CHECK-NEXT: Name: other
# CHECK:      Ordinal: 2
# CHECK-NEXT: Name: {{$}}
# CHECK:      Ordinal: 3
# CHECK-NEXT: Name: g
# CHECK:      Ordinal: 4
# CHECK-NEXT: Name: h
# CHECK:      Ordinal: 7
# CHECK-NEXT: Name: k

# CHECK:      File: b.dll
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# CHECK-NEXT: Symbol: (3)
# CHECK-NEXT: Symbol: (4)
# CHECK-NEXT: Symbol: (7)
# CHECK-NEXT: }

# CHECK:      File: c.dll
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: g (1)
# CHECK-NEXT: }

#--- a.s
  .text
  .globl other, hidden, g, h, k
other:
  retq
hidden:
  retq
g:
  retq
h:
  retq
k:
  retq

  .section .drectve,"yni"
  .ascii " -export:other,@1 -export:hidden,@2,NONAME -export:g -export:h"
  .ascii " -export:k,@7"

#--- b.s
  .text
  .globl bf
bf:
  callq h
  callq g
  callq k
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"

#--- c.s
  .text
  .globl cf
cf:
  callq g
  retq

  .section .drectve,"yni"
  .ascii " -export:cf"
