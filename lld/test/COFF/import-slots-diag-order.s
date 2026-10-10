# REQUIRES: x86
## The diagnostics about in-place import slots in the final image follow the
## order of the inputs, whatever the addresses of the linker's own objects.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ptr.s -o one.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ptr.s -o two.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ptr.s -o three.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ptr.s -o four.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ptr.s -o five.obj
# RUN: not lld-link -import-slots -entry:main -subsystem:console main.obj \
# RUN:   one.obj two.obj three.obj four.obj five.obj a.lib -out:x.exe \
# RUN:   -merge:.rdata=.text 2>&1 | FileCheck %s

# CHECK:      error: one.obj: the address of an import at offset 0x0 in .rdata is in executable section .text
# CHECK-NEXT: error: two.obj: the address of an import at offset 0x0 in .rdata is in executable section .text
# CHECK-NEXT: error: three.obj: the address of an import at offset 0x0 in .rdata is in executable section .text
# CHECK-NEXT: error: four.obj: the address of an import at offset 0x0 in .rdata is in executable section .text
# CHECK-NEXT: error: five.obj: the address of an import at offset 0x0 in .rdata is in executable section .text

#--- a.def
LIBRARY a.dll
EXPORTS
  func1

#--- main.s
  .text
  .globl main
main:
  retq

#--- ptr.s
  .section .rdata,"dr"
  .quad func1
