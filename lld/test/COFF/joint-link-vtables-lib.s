# REQUIRES: x86
## A member whose link line names the owner's import library, beside the static
## library that defines a strong vtable both members load, resolves in its final
## link as in its scan: the static library's object still defines the vtable,
## which the member then gives up for the owner's, reusing the import the
## owner's library already gave.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium k.s -o k.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-lib -out:k.lib k.obj

# RUN: lld-link -import-slots -dll -noentry a.obj k.lib -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib k.lib -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: lld-link -import-slots -dll -noentry a.obj k.lib -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib k.lib -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts
# RUN: llvm-readobj --coff-imports --coff-exports b.dll | FileCheck %s

# CHECK:      Name: a.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: _ZTV1K (0)
# CHECK-NEXT: Symbol: __llvm_private${{[0-9a-f]+}} (0)
# CHECK-NEXT: }
# CHECK:      Name: _ZTV1K
# CHECK-NEXT: ForwardedTo: a._ZTV1K

#--- k.s
  .section .rdata,"dr",one_only,_ZTV1K
  .globl _ZTV1K
  .p2align 4
_ZTV1K:
  .quad 0
  .quad 0
  .quad kh

  .section .text,"xr",one_only,kh
  .globl kh
kh:
  retq

  .section .drectve,"yni"
  .ascii " -export:_ZTV1K,DATA"

#--- a.s
  .text
  .globl af
af:
  callq kh
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .text
  .globl bf
bf:
  movq __imp__ZTV1K(%rip), %rax
  callq kh
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"
