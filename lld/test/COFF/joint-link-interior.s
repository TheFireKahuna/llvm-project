# REQUIRES: x86
## A member of a joint link whose static data holds an address inside an
## object another member exports takes the exporter's name for that address,
## X$soK, from its fact file, as it would from the exporter's import library,
## which its link line need not name, and by the ordinal the merge gives it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: llvm-readobj --coff-exports a.dll > out.txt
# RUN: llvm-readobj --coff-imports b.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s

# CHECK:      Ordinal: 1
# CHECK-NEXT: Name: obj
# CHECK:      Ordinal: 2
# CHECK-NEXT: Name: obj$so8
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: (1)
# CHECK-NEXT: Symbol: (2)
# CHECK-NEXT: }

#--- a.s
  .data
  .p2align 3
  .globl obj
obj:
  .quad 0, 0
  .globl "obj$so8"
  .set "obj$so8", obj+8

  .section .drectve,"yni"
  .ascii " /export:obj,data /export:obj$so8,data"

#--- b.s
  .section .rdata,"dr"
  .p2align 3
  .quad obj
  .quad obj+8
