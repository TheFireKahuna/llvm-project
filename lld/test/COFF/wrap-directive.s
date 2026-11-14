# REQUIRES: x86

## /wrap in an object's .drectve section wraps as -wrap on the command line
## does where only a library offers the symbol: from an object given before
## the wrapped symbols are created, and from an archive member loaded after
## them. It yields to a definition among the inputs, an object's or a loaded
## archive member's, also one that is loaded after the wrap was added, and
## every reference then binds that definition. -wrap on the command line
## wraps such a symbol too.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s -o wrap.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc late.s -o late.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc crt.s -o crt.obj
# RUN: llvm-lib -out:wrap.lib wrap.obj
# RUN: llvm-lib -out:late.lib late.obj
# RUN: llvm-lib -out:own.lib crt.obj
# RUN: lld-link -dll -noentry -export:exit -export:abort -out:crt.dll \
# RUN:   -implib:crt.lib crt.obj

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:a.exe \
# RUN:   main.obj wrap.lib crt.lib
# RUN: llvm-objdump -d a.exe | FileCheck %s

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:b.exe \
# RUN:   main.obj wrap.lib crt.obj
# RUN: llvm-objdump -d b.exe | FileCheck %s --check-prefix=OWN
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:c.exe \
# RUN:   main.obj wrap.lib own.lib
# RUN: llvm-objdump -d c.exe | FileCheck %s --check-prefix=OWN
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:d.exe \
# RUN:   main.obj late.lib
# RUN: llvm-objdump -d d.exe | FileCheck %s --check-prefix=OWN

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:e.exe \
# RUN:   main.obj wrap.lib crt.obj -wrap:exit -wrap:abort
# RUN: llvm-objdump -d e.exe | FileCheck %s

# CHECK:      <main>:
# CHECK-NEXT: callq {{.*}} <__wrap_exit>
# CHECK-NEXT: callq {{.*}} <__wrap_abort>
# CHECK:      <__wrap_exit>:
# CHECK-NEXT: callq {{.*}} <exit>

# OWN:      <main>:
# OWN-NEXT: callq {{.*}} <exit>
# OWN-NEXT: callq {{.*}} <abort>

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

#--- late.s
## The wrappers again, in an object that names the library that defines exit
## and abort: it is loaded for a wrap, so they are defined after it was added.
  .text
  .globl __wrap_exit, __wrap_abort
__wrap_exit:
  callq __real_exit
  retq
__wrap_abort:
  retq
  .section .drectve,"yn"
  .ascii " -wrap:abort -defaultlib:own.lib"

#--- crt.s
  .text
  .globl exit, abort
exit:
  retq
abort:
  retq
