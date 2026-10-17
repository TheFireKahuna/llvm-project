# REQUIRES: x86

## The symbol table that /debug:symtab writes names a local import pointer
## only while a reference still reads it; one that every reference was
## rewritten to bypass is left out of the image and of the table.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium keep.s -o keep.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:a.exe \
# RUN:   main.obj defs.obj
# RUN: llvm-readobj --symbols a.exe | FileCheck --check-prefix=DROP %s
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:b.exe \
# RUN:   keep.obj defs.obj 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-readobj --symbols b.exe | FileCheck --check-prefix=KEEP %s

# DROP-NOT: Name: __imp_f
# DROP:     Name: f
# DROP-NOT: Name: __imp_f

# WARN: warning: keep.obj: locally defined symbol imported: f (defined in defs.obj) [LNK4217]

# KEEP:      Name: __imp_f
# KEEP-NEXT: Value: 0
# KEEP-NEXT: Section: .rdata

#--- main.s
  .text
  .globl main
main:
  movq __imp_f(%rip), %rax
  callq *__imp_f(%rip)
  retq

#--- keep.s
  .text
  .globl main
main:
  movq __imp_f(%rip), %rax
  retq

  .data
  .quad __imp_f

#--- defs.s
  .text
  .globl f
f:
  retq
