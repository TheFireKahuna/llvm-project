# REQUIRES: x86

## A symbol that two archives define comes from the archive read first, both
## for a reference made before the archives are read and for one that a member
## loaded after both are read makes.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc f1.s -o f1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc h1.s -o h1.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc f2.s -o f2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc g2.s -o g2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc h2.s -o h2.obj
# RUN: llvm-lib -out:one.lib f1.obj h1.obj
# RUN: llvm-lib -out:two.lib f2.obj g2.obj h2.obj

# RUN: lld-link -entry:main -subsystem:console -out:main.exe main.obj one.lib \
# RUN:   two.lib -verbose 2>&1 | FileCheck %s \
# RUN:   --implicit-check-not=f2.obj --implicit-check-not=h2.obj

# CHECK-DAG: Loaded one.lib(f1.obj) for f
# CHECK-DAG: Loaded two.lib(g2.obj) for g
# CHECK-DAG: Loaded one.lib(h1.obj) for h

#--- main.s
.globl main
main:
  call f
  call g
  ret

#--- f1.s
.globl f
f:
  ret

#--- h1.s
.globl h
h:
  ret

#--- f2.s
.globl f
f:
  ret

#--- g2.s
.globl g
g:
  call h
  ret

#--- h2.s
.globl h
h:
  ret
