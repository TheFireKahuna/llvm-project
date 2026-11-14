# REQUIRES: x86

## As with GNU ld, -wrap:foo renames references and loads nothing else.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc none.s -o none.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc call.s -o call.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc real.s -o real.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc weak.s -o weak.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s -o wrap.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foo.s -o foo.obj
# RUN: llvm-lib -machine:x64 -def:crt.def -out:crt.lib
# RUN: llvm-lib -machine:x64 -def:rt.def -out:rt.lib
# RUN: llvm-lib -out:wrap.lib wrap.obj
# RUN: llvm-lib -out:foo.lib foo.obj

## While nothing refers to foo, __imp_foo or __real_foo, nothing is loaded.
# RUN: lld-link -entry:start -subsystem:console -out:none.exe none.obj \
# RUN:   crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports none.exe | FileCheck --check-prefix=NONE %s
# NONE-NOT: Name:

## Without garbage collection too, the import of foo that the call loaded
## before the wrap applied is left out once the call is renamed.
# RUN: lld-link -entry:start -subsystem:console -out:call.exe call.obj \
# RUN:   crt.lib rt.lib -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports call.exe | FileCheck --check-prefix=CALL %s
# CALL-NOT:  Name: crt.dll
# CALL:      Name: rt.dll
# CALL-NEXT: ImportLookupTableRVA:
# CALL-NEXT: ImportAddressTableRVA:
# CALL-NEXT: Symbol: __wrap_foo (0)
# CALL-NEXT: }
# CALL-NOT:  Name: crt.dll

## A reference to __real_foo, here in a wrapper that an archive gives up
## after the wrap applied, loads the foo that only an archive offers.
# RUN: lld-link -entry:start -subsystem:console -out:real.exe real.obj \
# RUN:   wrap.lib foo.lib -wrap:foo -verbose 2>&1 | FileCheck --check-prefix=REAL %s
# REAL: Loaded wrap.lib(wrap.obj) for __wrap_foo
# REAL: Loaded foo.lib(foo.obj) for foo

## A weak __real_foo loads nothing, as a weak external loads no member.
# RUN: lld-link -entry:start -subsystem:console -out:weak.exe weak.obj \
# RUN:   crt.lib -wrap:foo
# RUN: llvm-readobj --coff-imports weak.exe | FileCheck --check-prefix=NONE %s

## The LTO code generator lowers llvm.memset to a call to memset after the
## wraps are decided, and no bitcode symbol names memset, so with bitcode a wrap
## of a runtime library function applies as if referenced. The imported wrapper
## of one that the generated code does not call, memcpy, is left out, and foo
## is not referenced and not wrapped.
# RUN: llvm-as lto.ll -o lto.obj
# RUN: lld-link -entry:start -subsystem:console -out:lto.exe lto.obj \
# RUN:   crt.lib rt.lib -wrap:memset -wrap:memcpy -wrap:foo -opt:noref
# RUN: llvm-readobj --coff-imports lto.exe | FileCheck --check-prefix=LTO --implicit-check-not=memcpy %s
# LTO-NOT:  Name: crt.dll
# LTO:      Symbol: __wrap_memset (0)
# LTO-NEXT: }
# LTO-NOT:  Name: crt.dll

#--- crt.def
LIBRARY crt.dll
EXPORTS
  foo
  memset
  memcpy

#--- rt.def
LIBRARY rt.dll
EXPORTS
  __wrap_foo
  __wrap_memset
  __wrap_memcpy

#--- none.s
  .globl start
start:
  retq

#--- call.s
  .globl start
start:
  callq foo
  retq

#--- real.s
  .globl start
start:
  callq *__imp_foo(%rip)
  retq

#--- wrap.s
  .globl __wrap_foo
__wrap_foo:
  jmp __real_foo

#--- foo.s
  .globl foo
foo:
  retq

#--- weak.s
  .globl start
start:
  movq __real_foo(%rip), %rax
  retq
  .weak __real_foo

#--- lto.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define void @start(ptr %p, i64 %n) {
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %n, i1 false)
  ret void
}
