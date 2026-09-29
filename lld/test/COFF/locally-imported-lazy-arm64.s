# REQUIRES: aarch64

## Under -import-slots, an undefined __imp_X that no input provides under that
## name loads the archive member defining X, as a reference to X would, and a
## member loaded this way may ask for another.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc f.s -o f.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc g.s -o g.obj
# RUN: llvm-lib -machine:arm64 f.obj g.obj -out:fg.lib
# RUN: lld-link -import-slots -machine:arm64 -entry:main -subsystem:console \
# RUN:   -out:main.exe main.obj fg.lib -verbose 2>&1 | FileCheck %s

# CHECK-DAG: Loading lazy f from fg.lib for __imp_f
# CHECK-DAG: Loading lazy g from fg.lib for __imp_g
# CHECK-DAG: warning: main.obj: locally defined symbol imported: f (defined in fg.lib(f.obj)) [LNK4217]
# CHECK-DAG: warning: fg.lib(f.obj): locally defined symbol imported: g (defined in fg.lib(g.obj)) [LNK4217]

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
