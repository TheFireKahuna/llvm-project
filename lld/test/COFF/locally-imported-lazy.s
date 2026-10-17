# REQUIRES: x86

## Under -import-slots, an undefined __imp_X that no input provides under that
## name loads the archive member defining X, as a reference to X would, and
## binds to it through a local pointer. A member loaded this way may ask for
## another, and its directives apply during input resolution.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc f.s -o f.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc g.s -o g.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc h.s -o h.obj
# RUN: llvm-lib f.obj g.obj h.obj -out:fgh.lib
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:main.exe \
# RUN:   main.obj fgh.lib -verbose 2>&1 | FileCheck %s

## Objects between -start-lib and -end-lib are loaded the same way.
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:main.exe \
# RUN:   main.obj -start-lib f.obj g.obj h.obj -end-lib -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=STARTLIB %s

## Without -import-slots, the reference is an error, as with link.exe.
# RUN: not lld-link -entry:main -subsystem:console -out:main.exe main.obj \
# RUN:   fgh.lib 2>&1 | FileCheck --check-prefix=NOFLAG %s

# CHECK-DAG: Loading lazy f from fgh.lib for __imp_f
# CHECK-DAG: Loading lazy g from fgh.lib for __imp_g
# CHECK-DAG: warning: main.obj: locally defined symbol imported: f (defined in fgh.lib(f.obj)) [LNK4217]
# CHECK-DAG: warning: fgh.lib(f.obj): locally defined symbol imported: g (defined in fgh.lib(g.obj)) [LNK4217]

# STARTLIB-DAG: Loading lazy f from f.obj for __imp_f
# STARTLIB-DAG: Loading lazy g from g.obj for __imp_g

# NOFLAG: error: undefined symbol: __declspec(dllimport) f

## An import library that provides __imp_foo takes precedence over a static
## library that defines foo, in either order.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foo.s -o foo.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc usefoo.s -o usefoo.obj
# RUN: llvm-lib foo.obj -out:foo-static.lib
# RUN: lld-link -dll -noentry -out:foo.dll foo.obj -export:foo -implib:foo.lib
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:usefoo1.exe \
# RUN:   usefoo.obj foo-static.lib foo.lib -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=IMPLIB %s
# RUN: llvm-readobj --coff-imports usefoo1.exe | FileCheck --check-prefix=IMPORT %s
# RUN: lld-link -import-slots -entry:main -subsystem:console -out:usefoo2.exe \
# RUN:   usefoo.obj foo.lib foo-static.lib -verbose 2>&1 | \
# RUN:   FileCheck --check-prefix=IMPLIB %s
# RUN: llvm-readobj --coff-imports usefoo2.exe | FileCheck --check-prefix=IMPORT %s

# IMPLIB-NOT: for __imp_foo
# IMPLIB-NOT: locally defined symbol imported

# IMPORT:      Name: foo.dll
# IMPORT:        Symbol: foo

#--- main.s
.text
.globl main
main:
  call *__imp_f(%rip)
  call c
  ret

## f's member names the alternate for c, which the loop that loads it for
## __imp_f then applies.
#--- f.s
.text
.globl f
f:
  call *__imp_g(%rip)
  ret
.section .drectve,"yn"
.ascii " /alternatename:c=h"

#--- g.s
.text
.globl g
g:
  ret

#--- h.s
.text
.globl h
h:
  ret

#--- foo.s
.text
.globl foo
foo:
  ret

#--- usefoo.s
.text
.globl main
main:
  call *__imp_foo(%rip)
  ret
