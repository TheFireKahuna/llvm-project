# REQUIRES: x86

## The undefined symbols waiting when an archive is read load the members that
## define them in the order of the archive's symbol table, whether the symbol
## table is looked up for each waiting symbol (few symbols waiting) or the
## waiting symbols are looked up for each archive symbol (many symbols defined
## already).

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc many.s -o many.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc alpha.s -o alpha.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc mid.s -o mid.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc zed.s -o zed.obj
# RUN: llvm-lib -out:lib.lib zed.obj mid.obj alpha.obj

# RUN: lld-link -entry:main -subsystem:console -out:main.exe main.obj lib.lib \
# RUN:   -verbose 2>&1 | FileCheck %s
# RUN: lld-link -entry:main -subsystem:console -out:many.exe main.obj many.obj \
# RUN:   lib.lib -verbose 2>&1 | FileCheck %s

# CHECK:      Reading lib.lib
# CHECK-NEXT: Reading lib.lib(alpha.obj)
# CHECK-NEXT: Loaded lib.lib(alpha.obj) for alpha
# CHECK-NEXT: Reading lib.lib(mid.obj)
# CHECK-NEXT: Loaded lib.lib(mid.obj) for mid
# CHECK-NEXT: Reading lib.lib(zed.obj)
# CHECK-NEXT: Loaded lib.lib(zed.obj) for zed

#--- main.s
.globl main
main:
  call zed
  call mid
  call alpha
  ret

#--- many.s
.irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
.globl def\n
def\n:
.endr
  ret

#--- alpha.s
.globl alpha
alpha:
  ret

#--- mid.s
.globl mid
mid:
  ret

#--- zed.s
.globl zed
zed:
  ret
