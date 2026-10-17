# REQUIRES: aarch64
## Under -import-slots, a reference from code to data that resolved to its
## import is an error naming the fix, which for assembly on ARM64 is the import
## pointer or the GOT spelling that reaches it.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:arm64
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc code.s -o code.obj
# RUN: not lld-link -dll -noentry -import-slots -out:out.dll code.obj \
# RUN:   lib.lib 2>&1 | FileCheck %s
# CHECK:     error: code.obj: variable is imported from lib.dll, but the object was compiled as if it were local; mark its declaration, or compile the object with -fno-direct-access-external-data; in assembly, reach it through __imp_variable or :got:variable
# CHECK-NOT: error:

#--- lib.def
LIBRARY lib.dll
EXPORTS
  variable DATA

#--- code.s
  .text
  .globl f
f:
  adrp x8, variable
  ldr w0, [x8, :lo12:variable]
  ret
