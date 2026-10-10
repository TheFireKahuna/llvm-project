# REQUIRES: x86

## As with GNU ld, -wrap:foo does nothing when nothing references foo, its
## import __imp_foo or __real_foo: neither an import library nor an archive
## gives up __wrap_foo, so the image has no import of it. A reference wraps as
## before, including one in a member that loading another wrapper brings in.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc none.s -o none.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc call.s -o call.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc imp.s -o imp.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc bar.s -o bar.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wfoo.s -o wfoo.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wbar.s -o wbar.obj
# RUN: llvm-lib -machine:x64 -def:crt.def -out:crt.lib
# RUN: lld-link -dll -noentry -out:rt.dll wfoo.obj -export:__wrap_foo \
# RUN:   -implib:rt.lib
# RUN: llvm-lib -out:wrap.lib wfoo.obj wbar.obj

## Unreferenced, with the wrapper in an import library or an archive, and
## without garbage collection, which would hide an unused import.
# RUN: lld-link -entry:start -subsystem:console -out:none1.exe \
# RUN:   none.obj crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports none1.exe | FileCheck --check-prefix=NONE %s
# RUN: lld-link -entry:start -subsystem:console -out:none2.exe \
# RUN:   none.obj crt.lib wrap.lib -wrap:foo -opt:noref -debug:symtab
# RUN: llvm-readobj --coff-imports none2.exe | FileCheck --check-prefix=NONE %s
# RUN: llvm-nm none2.exe | FileCheck --check-prefix=NONE-SYMS %s

# NONE-NOT: Name: rt.dll
# NONE-NOT: Name: crt.dll
# NONE-NOT: __wrap_foo
# NONE-SYMS-NOT: wrap

## Referenced directly or through the import.
# RUN: lld-link -entry:start -subsystem:console -out:call.exe \
# RUN:   call.obj crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports call.exe | FileCheck --check-prefix=WRAP %s
# RUN: lld-link -entry:start -subsystem:console -out:imp.exe \
# RUN:   imp.obj crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports imp.exe | FileCheck --check-prefix=WRAP %s

# WRAP:      Name: rt.dll
# WRAP-NEXT: ImportLookupTableRVA:
# WRAP-NEXT: ImportAddressTableRVA:
# WRAP-NEXT: Symbol: __wrap_foo

## Referenced only by the member that defines __wrap_bar.
# RUN: lld-link -entry:start -subsystem:console -out:late.exe \
# RUN:   bar.obj crt.lib wrap.lib -wrap:foo -wrap:bar -opt:ref -debug:symtab
# RUN: llvm-readobj --coff-imports late.exe | FileCheck --check-prefix=LATE \
# RUN:   --allow-empty %s
# RUN: llvm-nm late.exe | FileCheck --check-prefix=LATE-SYMS %s

# LATE-NOT: Name: crt.dll
# LATE-SYMS: T __wrap_bar
# LATE-SYMS: T __wrap_foo

#--- crt.def
LIBRARY crt.dll
EXPORTS
  foo
  bar

#--- none.s
  .globl start
start:
  retq

#--- call.s
  .globl start
start:
  callq foo
  retq

#--- imp.s
  .globl start
start:
  callq *__imp_foo(%rip)
  retq

#--- bar.s
  .globl start
start:
  callq bar
  retq

#--- wfoo.s
  .globl __wrap_foo
__wrap_foo:
  retq

#--- wbar.s
  .globl __wrap_bar
__wrap_bar:
  callq foo
  retq
