# REQUIRES: x86

## /wrap in .drectve wraps as -wrap does where only a library offers the
## symbol, before or after the wrapped symbols exist, and yields to a definition
## among the inputs, which every reference then binds.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{exe} = lld-link -entry:main -subsystem:console -debug:symtab
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s -o wrap.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s --defsym LATE=1 \
# RUN:   -o late.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc crt.s -o crt.obj
# RUN: llvm-lib -out:wrap.lib wrap.obj
# RUN: llvm-lib -out:late.lib late.obj
# RUN: llvm-lib -out:own.lib crt.obj
# RUN: lld-link -dll -noentry -export:exit -export:abort -out:crt.dll \
# RUN:   -implib:crt.lib crt.obj

# RUN: %{exe} -out:a.exe main.obj wrap.lib crt.lib
# RUN: llvm-objdump -d a.exe | FileCheck %s

# RUN: %{exe} -out:b.exe main.obj wrap.lib crt.obj
# RUN: llvm-objdump -d b.exe | FileCheck %s --check-prefix=OWN
# RUN: %{exe} -out:c.exe main.obj wrap.lib own.lib
# RUN: llvm-objdump -d c.exe | FileCheck %s --check-prefix=OWN
# RUN: %{exe} -out:d.exe main.obj late.lib
# RUN: llvm-objdump -d d.exe | FileCheck %s --check-prefix=OWN

## A reference through an exit import pointer loaded before the definition
## binds the definition, and the import goes, with or without GC.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ptr.s -o ptr.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc both.s -o both.obj
# RUN: llvm-lib -out:both.lib both.obj
# RUN: %{exe} -out:f.exe ptr.obj crt.lib both.lib
# RUN: llvm-objdump -d f.exe | FileCheck %s --check-prefix=PTR
# RUN: llvm-readobj --coff-imports f.exe | FileCheck %s --check-prefix=NONE
# RUN: %{exe} -out:g.exe ptr.obj crt.lib both.lib -opt:noref
# RUN: llvm-objdump -d g.exe | FileCheck %s --check-prefix=PTR
# RUN: llvm-readobj --coff-imports g.exe | FileCheck %s --check-prefix=NONE

# RUN: %{exe} -out:e.exe main.obj wrap.lib crt.obj -wrap:exit -wrap:abort
# RUN: llvm-objdump -d e.exe | FileCheck %s

# CHECK:      <main>:
# CHECK-NEXT: callq {{.*}} <__wrap_exit>
# CHECK-NEXT: callq {{.*}} <__wrap_abort>
# CHECK:      <__wrap_exit>:
# CHECK-NEXT: callq {{.*}} <exit>

# OWN:      <main>:
# OWN-NEXT: callq {{.*}} <exit>
# OWN-NEXT: callq {{.*}} <abort>

# PTR:      <main>:
# PTR-NEXT: callq {{.*}} <exit>
# PTR-NEXT: callq {{.*}} <exit>
# PTR-NEXT: leaq {{.*}} <exit>

# NONE-NOT: Import

#--- ptr.s
  .text
  .globl main
main:
  callq *__imp_exit(%rip)
  callq *__imp___real_exit(%rip)
  movq __imp_exit(%rip), %rax
  retq
  .section .drectve,"yn"
  .ascii " /wrap:exit"

#--- both.s
  .text
  .globl exit, __wrap_exit
exit:
  retq
__wrap_exit:
  nop
  retq

#--- main.s
  .text
  .globl main
main:
  callq exit
  callq abort
  retq
  .section .drectve,"yn"
  .ascii " /wrap:exit"

## With LATE it names the library defining exit and abort, which is loaded for
## a wrap, so they are defined after it was added.
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
.ifdef LATE
  .ascii " -defaultlib:own.lib"
.endif

#--- crt.s
  .text
  .globl exit, abort
exit:
  retq
abort:
  retq
