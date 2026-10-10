# REQUIRES: x86

## -wrap:foo renames every reference to foo and __imp_foo. An import of foo
## that such a reference loaded before the wrap applied is then left out of
## the image, without garbage collection too, unless __real_foo or a root
## still reaches it.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc call.s -o call.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc imp.s -o imp.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc real.s -o real.obj
# RUN: llvm-lib -machine:x64 -def:crt.def -out:crt.lib
# RUN: llvm-lib -machine:x64 -def:rt.def -out:rt.lib

# RUN: lld-link -entry:start -subsystem:console -out:call.exe call.obj \
# RUN:   crt.lib rt.lib -wrap:foo -opt:noref -debug
# RUN: llvm-readobj --coff-imports call.exe | FileCheck --check-prefix=WRAP %s
# RUN: lld-link -entry:start -subsystem:console -out:imp.exe imp.obj \
# RUN:   crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports imp.exe | FileCheck --check-prefix=WRAP %s

# WRAP-NOT:  Name: crt.dll
# WRAP:      Name: rt.dll
# WRAP-NEXT: ImportLookupTableRVA:
# WRAP-NEXT: ImportAddressTableRVA:
# WRAP-NEXT: Symbol: __wrap_foo (0)
# WRAP-NEXT: }
# WRAP-NOT:  Name: crt.dll

## __real_foo, or /include:foo, keeps the import.
# RUN: lld-link -entry:start -subsystem:console -out:real.exe real.obj \
# RUN:   crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports real.exe | FileCheck --check-prefix=KEEP %s
# RUN: lld-link -entry:start -subsystem:console -out:include.exe call.obj \
# RUN:   crt.lib rt.lib -wrap:foo -opt:noref -include:foo
# RUN: llvm-readobj --coff-imports include.exe | FileCheck --check-prefix=KEEP %s

# KEEP:      Name: crt.dll
# KEEP-NEXT: ImportLookupTableRVA:
# KEEP-NEXT: ImportAddressTableRVA:
# KEEP-NEXT: Symbol: foo (0)

#--- crt.def
LIBRARY crt.dll
EXPORTS
  foo

#--- rt.def
LIBRARY rt.dll
EXPORTS
  __wrap_foo

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

#--- real.s
  .globl start
start:
  callq foo
  callq __real_foo
  retq
