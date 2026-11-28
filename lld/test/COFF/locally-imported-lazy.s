# REQUIRES: x86, aarch64

## An unresolved __imp_X from a fork object loads the archive member defining X
## after ordinary loads settle (an import library keeps precedence), applying
## its directives; a foreign reference keeps link.exe's error.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console
# DEFINE: %{lib} = llvm-lib -machine:%{a}
# DEFINE: %{ro} = llvm-readobj --coff-imports
# DEFINE: %{build} = %{mc} %{a}/main.s -o main.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/main.s \
# DEFINE:     -o foreign.obj && \
# DEFINE:   %{mc} %{a}/f.s -o f.obj && %{mc} g.s -o g.obj && \
# DEFINE:   %{mc} h.s -o h.obj && %{lib} f.obj g.obj h.obj -out:fgh.lib && \
# DEFINE:   %{mc} foo.s -o foo.obj && %{lib} foo.obj -out:foo-static.lib && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:foo.dll foo.obj \
# DEFINE:     -export:foo -implib:foo.lib && %{mc} %{a}/usefoo.s -o usefoo.obj
# DEFINE: %{run} = %{build} && \
# DEFINE:   %{link} -out:main.exe main.obj fgh.lib -verbose 2>&1 | \
# DEFINE:     FileCheck --check-prefix=LOAD %s && \
# DEFINE:   %{link} -out:main.exe main.obj -start-lib f.obj g.obj h.obj \
# DEFINE:     -end-lib -verbose 2>&1 | FileCheck --check-prefix=STARTLIB %s && \
# DEFINE:   not %{link} -out:foreign.exe foreign.obj g.obj fgh.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=FOREIGN %s && \
# DEFINE:   %{link} -out:use1.exe usefoo.obj foo-static.lib foo.lib \
# DEFINE:     -verbose 2>&1 | FileCheck --check-prefix=IMPLIB %s && \
# DEFINE:   %{ro} use1.exe | FileCheck --check-prefix=IMPORT %s && \
# DEFINE:   %{link} -out:use2.exe usefoo.obj foo.lib foo-static.lib \
# DEFINE:     -verbose 2>&1 | FileCheck --check-prefix=IMPLIB %s && \
# DEFINE:   %{ro} use2.exe | FileCheck --check-prefix=IMPORT %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# LOAD-DAG: Loading lazy f from fgh.lib for __imp_f
# LOAD-DAG: Loading lazy g from fgh.lib for __imp_g
# LOAD-DAG: Loaded fgh.lib(h.obj) for h

# STARTLIB-DAG: Loading lazy f from f.obj for __imp_f
# STARTLIB-DAG: Loading lazy g from g.obj for __imp_g

# FOREIGN: error: undefined symbol: __declspec(dllimport) f

# IMPLIB-NOT: for __imp_foo
# IMPORT:      Name: foo.dll
# IMPORT-NEXT: ImportLookupTableRVA:
# IMPORT-NEXT: ImportAddressTableRVA:
# IMPORT-NEXT: Symbol: foo

#--- x64/main.s
.globl main
main:
  call *__imp_f(%rip)
  call c
  ret

## f's member names the alternate for c, which the loop that loads it for
## __imp_f then applies.
#--- x64/f.s
.globl f
f:
  call *__imp_g(%rip)
  ret
.section .drectve,"yn"
.ascii " /alternatename:c=h"

#--- x64/usefoo.s
.globl main
main:
  call *__imp_foo(%rip)
  ret

#--- arm64/main.s
.globl main
main:
  adrp x16, __imp_f
  ldr x16, [x16, :lo12:__imp_f]
  blr x16
  bl c
  ret

#--- arm64/f.s
.globl f
f:
  adrp x16, __imp_g
  ldr x16, [x16, :lo12:__imp_g]
  br x16
.section .drectve,"yn"
.ascii " /alternatename:c=h"

#--- arm64/usefoo.s
.globl main
main:
  adrp x16, __imp_foo
  ldr x16, [x16, :lo12:__imp_foo]
  br x16

## Shared by both machines.
#--- g.s
.globl g
g:
  ret

#--- h.s
.globl h
h:
  ret

#--- foo.s
.globl foo
foo:
  ret
