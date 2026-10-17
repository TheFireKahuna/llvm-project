# REQUIRES: x86
## Position-independent ELF assembly reaches external data through the GOT,
## which X@GOTPCREL names; on Windows Itanium the assembler makes it a load of
## X's import pointer. Under -import-slots the load reads the import address
## table entry of a variable another image provides, and becomes the address
## itself when the image defines the variable, as an ELF linker relaxes a GOT
## load.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium code.s -o code.obj
# RUN: lld-link -dll -noentry -import-slots -out:out.dll code.obj lib.lib
# RUN: llvm-objdump -d out.dll | FileCheck %s
# RUN: llvm-readobj --coff-imports out.dll | FileCheck %s --check-prefix=IMPORTS

# CHECK:      movq {{.*}}(%rip), %rax
# CHECK-NEXT: movl (%rax), %eax
# CHECK-NEXT: leaq {{.*}}(%rip), %rcx
# CHECK-NEXT: addl (%rcx), %eax

# IMPORTS: Name: lib.dll
# IMPORTS: Symbol: variable

#--- lib.def
LIBRARY lib.dll
EXPORTS
  variable DATA

#--- code.s
  .text
  .globl f
f:
  movq variable@GOTPCREL(%rip), %rax
  movl (%rax), %eax
  movq local@GOTPCREL(%rip), %rcx
  addl (%rcx), %eax
  retq

  .data
  .globl local
local:
  .long 7
