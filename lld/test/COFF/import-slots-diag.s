# REQUIRES: x86
## Under -import-slots, data that resolved to its import is reached only by a
## word of static data holding its address. A reference from code is an error
## naming the fix, as is a relocation in data that cannot reach another image,
## a read-only word in a section that cannot be moved to the import address
## table, and an export of the data. Debug sections are not checked.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc code.s -o code.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc data.s -o data.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc debug.s -o debug.obj

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll code.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=CODE %s
# CODE:     error: code.obj: variable is imported from lib.dll, but the object was compiled as if it were local; mark its declaration, or compile the object with -fauto-import
# CODE-NOT: error:

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll data.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=DATA %s
# DATA: error: data.obj: variable is imported from lib.dll, but .data refers to it with relocation type IMAGE_REL_AMD64_ADDR32NB, which cannot reach another image

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll ro.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=RO %s
# RO: error: ro.obj: .myro is read-only and holds the address of variable, imported from lib.dll, but cannot be laid out with the import address table

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll debug.obj \
# RUN:   lib.lib -export:variable 2>&1 | FileCheck --check-prefix=EXPORT %s
# EXPORT: error: cannot export variable: it is imported from lib.dll; export a forwarder to it instead

## A reference from a debug section is not checked.
# RUN: lld-link -dll -noentry -import-slots -out:out.dll debug.obj lib.lib

#--- lib.def
LIBRARY lib.dll
EXPORTS
  variable DATA

#--- code.s
  .text
  .globl f
f:
  movl variable(%rip), %eax
  addl variable(%rip), %eax
  retq

#--- data.s
  .data
  .rva variable

#--- ro.s
  .section .myro,"dr"
  .quad variable

#--- debug.s
  .data
  .quad variable
  .section .debug_info,"dr"
  .quad variable
