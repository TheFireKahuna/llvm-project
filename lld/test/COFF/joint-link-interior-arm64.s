# REQUIRES: aarch64
## On ARM64, a member's static data that holds an address inside data another
## member exports binds to the export the merge names for that address, which
## the exporter exports with no name and the member imports by ordinal.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium b.s -o b.obj
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: llvm-readobj --coff-exports a.dll > out.txt
# RUN: llvm-readobj --coff-imports --sections b.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s

# CHECK:      Ordinal: 1
# CHECK-NEXT: Name: arr
# CHECK-NEXT: RVA: 0x[[#%X,ARR:]]
# CHECK:      Ordinal: 2
# CHECK-NEXT: Name: {{$}}
# CHECK-NEXT: RVA: 0x[[#%X,ARR+8]]
# CHECK:      File: b.dll
# CHECK-NOT:  Name: .sealed
# CHECK:      Symbol: (2)
# CHECK-NEXT: }

#--- a.s
  .section .data,"dw",one_only,arr
  .globl arr
  .p2align 4
arr:
  .word 1, 2, 3, 4

  .section .drectve,"yni"
  .ascii " -export:arr,DATA"

#--- b.s
  .section .rdata,"dr"
  .globl p
p:
  .xword arr+8

  .section .drectve,"yni"
  .ascii " -export:p,DATA"
