# REQUIRES: x86
## Under -import-slots, the names that a word of static data may take for an
## address inside imported data are loaded only from an import. A regular
## archive member that defines such a name is not loaded: the word is the
## residual fill's to write, as if nothing defined the name.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium interior.s \
# RUN:   -o interior.obj
# RUN: llvm-lib -out:interior.lib interior.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj rw.obj \
# RUN:   interior.lib lib.lib
# RUN: llvm-readobj --coff-imports --sections out.dll | FileCheck %s
# RUN: lld-link -dll -noentry -import-slots -opt:noref -out:noref.dll crt.obj \
# RUN:   rw.obj interior.lib lib.lib
# RUN: llvm-readobj --coff-imports --sections noref.dll | FileCheck %s

# CHECK-NOT:  Name: .member
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA:
# CHECK-NEXT: Symbol: obj ({{[0-9]+}})
# CHECK-NEXT: }
# CHECK-NOT:  Symbol:

#--- lib.def
LIBRARY lib.dll
EXPORTS
  obj DATA

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0

## A stand-in for the builtins' __llvm_fill, which writes the residual word.
  .def __llvm_fill
  .scl 2
  .type 32
  .endef
  .section .text,"xr",one_only,__llvm_fill
  .globl __llvm_fill
__llvm_fill:
  leaq __llvm_fill_list(%rip), %rax
  retq

#--- rw.s
  .data
  .p2align 3
  .quad obj+8

#--- interior.s
  .section .member,"dw"
  .globl "__imp_obj$so8"
"__imp_obj$so8":
  .quad 0
