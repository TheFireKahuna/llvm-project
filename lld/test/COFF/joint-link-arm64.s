# REQUIRES: aarch64
## A joint link of ARM64 members: b.dll references what a.dll exports with no
## import library, and a.dll, declared first, keeps v, which both keep, for
## b.dll, which imports it.

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
# RUN: llvm-readobj --coff-exports a.dll | FileCheck --check-prefix=A %s
# RUN: llvm-readobj --coff-imports b.dll | FileCheck --check-prefix=B %s

# A: Name: af
# A: Name: v

# B:      Name: a.dll
# B-NEXT: ImportLookupTableRVA:
# B-NEXT: ImportAddressTableRVA:
# B-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# B-NEXT: Symbol: (1)
# B-NEXT: Symbol: (2)
# B-NEXT: }

#--- a.s
  .section .data,"dw",discard,v
  .globl v
v:
  .word 1

  .text
  .globl af
af:
  ret

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .section .data,"dw",discard,v
  .globl v
v:
  .word 1

  .text
  .globl bf
bf:
  adrp x0, __imp_v
  ldr x0, [x0, :lo12:__imp_v]
  adrp x1, __imp_af
  ldr x1, [x1, :lo12:__imp_af]
  br x1
