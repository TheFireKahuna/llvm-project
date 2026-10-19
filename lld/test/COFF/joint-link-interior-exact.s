# REQUIRES: x86
## In a joint link, a member's static data that holds an address inside data
## another member exports, or keeps for it, binds to an export of that exact
## address, which the loader writes whole, as ELF's dynamic relocation writes
## the symbol's address plus the addend. The merge names each from the scans,
## which read the addends of the member's relocations: a public subobject name
## where the exporter has one, else a name the exporter exports for the others
## alone, with no name in its export table, past the end of an array as well.
## Every import is by the ordinal the merge gives it, and an import that only
## such words read is left out; nothing is written at start-up.

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

## a.dll exports arr+8, arr+16 and w+4, which w's owner keeps for b.dll, with
## no names; b.dll's words of obj+8 bind to obj$so8, which it exports already.
# RUN: llvm-readobj --coff-exports a.dll > out.txt
# RUN: llvm-readobj --coff-imports --sections b.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s
# CHECK:      Ordinal: 1
# CHECK-NEXT: Name: arr
# CHECK-NEXT: RVA: 0x[[#%X,ARR:]]
# CHECK:      Ordinal: 2
# CHECK-NEXT: Name: {{$}}
# CHECK-NEXT: RVA: 0x[[#%X,ARR+16]]
# CHECK:      Ordinal: 3
# CHECK-NEXT: Name: {{$}}
# CHECK-NEXT: RVA: 0x[[#%X,ARR+8]]
# CHECK:      Ordinal: 4
# CHECK-NEXT: Name: obj
# CHECK:      Ordinal: 5
# CHECK-NEXT: Name: obj$so8
# CHECK:      Ordinal: 6
# CHECK-NEXT: Name: w
# CHECK-NEXT: RVA: 0x[[#%X,W:]]
# CHECK:      Ordinal: 7
# CHECK-NEXT: Name: {{$}}
# CHECK-NEXT: RVA: 0x[[#%X,W+4]]

# CHECK:      File: b.dll
# CHECK-NOT:  Name: .sealed
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# CHECK-NEXT: }
# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: (3)
# CHECK-NEXT: Symbol: (2)
# CHECK-NEXT: Symbol: (5)
# CHECK-NEXT: Symbol: (7)
# CHECK-NEXT: }
# CHECK-NOT:  NtProtectVirtualMemory

## The merge is a function of the summaries.
# RUN: cp -r facts facts1
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: diff -r facts facts1

## A directive names a symbol and a positive offset.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium bad.s -o bad.obj
# RUN: not lld-link -import-slots -dll -noentry bad.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD %s
# BAD: error: bad.obj: malformed /interior: directive: arr,0
# BAD: error: bad.obj: malformed /interior: directive: arr

#--- a.s
  .linkbindings
  .section .data,"dw",one_only,arr
  .globl arr
  .p2align 4
arr:
  .long 1, 2, 3, 4

  .section .data,"dw",one_only,obj
  .globl obj, obj$so8
obj:
  .quad 5
obj$so8:
  .quad 6

  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 7, 8

  .text
  .globl af
af:
  movq __imp_w(%rip), %rax
  retq

  .section .drectve,"yni"
  .ascii " -export:af -export:arr,DATA -export:obj,DATA -export:obj$so8,DATA"

#--- b.s
  .linkbindings
  .section .rdata,"dr"
  .globl p1, p2, p3, p4
p1:
  .quad arr+8
p2:
  .quad arr+16
p3:
  .quad obj+8
p4:
  .quad w+4

  .section .data,"dw",discard,w
  .globl w
w:
  .linkbinding w, default
  .long 7, 8

  .section .drectve,"yni"
  .ascii " -export:p1,DATA -export:p2,DATA -export:p3,DATA -export:p4,DATA"

#--- bad.s
  .section .drectve,"yni"
  .ascii " /interior:arr,0 /interior:arr"
