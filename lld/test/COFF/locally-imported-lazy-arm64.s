# REQUIRES: aarch64

## Under -import-slots, an undefined __imp_X that no input provides under that
## name loads the archive member defining X, as a reference to X would, and a
## member loaded this way may ask for another.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium f.s -o f.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium g.s -o g.obj
# RUN: llvm-lib -machine:arm64 f.obj g.obj -out:fg.lib
# RUN: lld-link -import-slots -machine:arm64 -entry:main -subsystem:console \
# RUN:   -out:main.exe main.obj fg.lib -verbose 2>&1 | FileCheck %s

## Each pointer is only loaded by adrp and ldr, which become adrp and add of
## its symbol, so neither is imported in the end.
# CHECK-NOT: locally defined symbol imported
# CHECK-DAG: Loading lazy f from fg.lib for __imp_f
# CHECK-DAG: Loading lazy g from fg.lib for __imp_g
# CHECK-NOT: locally defined symbol imported

#--- main.s
.text
.globl main
main:
  adrp x16, __imp_f
  ldr x16, [x16, :lo12:__imp_f]
  br x16

#--- f.s
.text
.globl f
f:
  adrp x16, __imp_g
  ldr x16, [x16, :lo12:__imp_g]
  br x16

#--- g.s
.text
.globl g
g:
  ret
