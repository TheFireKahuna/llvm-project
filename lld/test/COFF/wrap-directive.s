# REQUIRES: x86

## /wrap in an object's .drectve section wraps as -wrap on the command line
## does: from an object given before the wrapped symbols are created, and from
## an archive member loaded after them.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s -o wrap.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc crt.s -o crt.obj
# RUN: llvm-lib -out:wrap.lib wrap.obj
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:a.exe \
# RUN:   main.obj wrap.lib crt.obj
# RUN: llvm-objdump -d a.exe | FileCheck %s

# CHECK:      <main>:
# CHECK-NEXT: callq {{.*}} <__wrap_exit>
# CHECK-NEXT: callq {{.*}} <__wrap_abort>
# CHECK:      <__wrap_exit>:
# CHECK-NEXT: callq {{.*}} <exit>

#--- main.s
  .text
  .globl main
main:
  callq exit
  callq abort
  retq
  .section .drectve,"yn"
  .ascii " /wrap:exit"

#--- wrap.s
  .text
  .globl __wrap_exit, __wrap_abort
__wrap_exit:
  callq __real_exit
  retq
__wrap_abort:
  retq
  .section .drectve,"yn"
  .ascii " -wrap:abort"

#--- crt.s
  .text
  .globl exit, abort
exit:
  retq
abort:
  retq
