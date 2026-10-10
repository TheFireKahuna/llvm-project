# REQUIRES: x86

## A reference to __real_foo loads foo when only an archive or an import
## library offers it, as with GNU ld and lld's ELF port. That includes a
## reference in a wrapper that an archive gives up only after the wrap.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc real.s -o real.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc imp.s -o imp.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc wrap.s -o wrap.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foo.s -o foo.obj
# RUN: llvm-lib -out:foo.lib foo.obj
# RUN: llvm-lib -out:wrap.lib wrap.obj
# RUN: llvm-lib -machine:x64 -def:crt.def -out:crt.lib

## __real_foo referenced directly, foo in an archive or an import library.
# RUN: lld-link -entry:start -subsystem:console -out:real1.exe -debug:symtab \
# RUN:   real.obj wrap.lib foo.lib -wrap:foo
# RUN: llvm-objdump -d real1.exe | FileCheck --check-prefix=REAL %s
# RUN: lld-link -entry:start -subsystem:console -out:real2.exe \
# RUN:   real.obj wrap.lib crt.lib -wrap:foo
# RUN: llvm-readobj --coff-imports real2.exe | FileCheck --check-prefix=IMP %s

# REAL:      <start>:
# REAL-NEXT:   callq {{.*}} <foo>

# IMP:      Name: crt.dll
# IMP-NEXT: ImportLookupTableRVA:
# IMP-NEXT: ImportAddressTableRVA:
# IMP-NEXT: Symbol: foo (0)

## foo referenced through __imp_foo, __real_foo only by the wrapper.
# RUN: lld-link -entry:start -subsystem:console -out:late.exe -debug:symtab \
# RUN:   imp.obj wrap.lib foo.lib -wrap:foo
# RUN: llvm-objdump -d late.exe | FileCheck --check-prefix=LATE %s

# LATE:      <start>:
# LATE-NEXT:   callq *{{.*}} <__imp___wrap_foo>
# LATE:      <__wrap_foo>:
# LATE-NEXT:   callq {{.*}} <foo>

## A weak __real_foo loads nothing, as a weak external loads no member.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc weak.s -o weak.obj
# RUN: lld-link -entry:start -subsystem:console -out:weak.exe weak.obj \
# RUN:   crt.lib -wrap:foo
# RUN: llvm-readobj --coff-imports weak.exe | FileCheck --check-prefix=WEAK \
# RUN:   --allow-empty %s

# WEAK-NOT: crt.dll

#--- crt.def
LIBRARY crt.dll
EXPORTS
  foo

#--- real.s
  .globl start
start:
  callq __real_foo
  retq

#--- weak.s
  .globl start
start:
  movq ref_foo(%rip), %rax
  retq
  .globl __wrap_foo
__wrap_foo:
  retq
  .data
ref_foo:
  .quad __real_foo
  .weak __real_foo

#--- imp.s
  .globl start
start:
  callq *__imp_foo(%rip)
  retq

#--- wrap.s
  .globl __wrap_foo
__wrap_foo:
  callq __real_foo
  retq

#--- foo.s
  .globl foo
foo:
  retq
