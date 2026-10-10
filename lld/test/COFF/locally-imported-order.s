# REQUIRES: x86

## The local import pointers that the image keeps are laid out by the names of
## the symbols they hold, whatever order the symbol table holds them in.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium defs.s -o defs.obj
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:a.exe \
# RUN:   main.obj defs.obj -ignore:4217
# RUN: llvm-nm -n a.exe | FileCheck %s

# CHECK:      R __imp_alpha
# CHECK-NEXT: R __imp_bravo
# CHECK-NEXT: R __imp_charlie
# CHECK-NEXT: R __imp_delta
# CHECK-NEXT: R __imp_echo
# CHECK-NEXT: R __imp_foxtrot

#--- main.s
  .text
  .globl main
main:
  retq

  .data
  .quad __imp_echo
  .quad __imp_bravo
  .quad __imp_foxtrot
  .quad __imp_alpha
  .quad __imp_delta
  .quad __imp_charlie

#--- defs.s
  .text
  .globl alpha, bravo, charlie, delta, echo, foxtrot
alpha:
bravo:
charlie:
delta:
echo:
foxtrot:
  retq
