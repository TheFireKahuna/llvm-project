# REQUIRES: x86

## Beside a fork object a foreign object's classified reference to an import
## binds as a fork one would (lea loads the entry, words are slots, calls keep
## the thunk); an unclassified one is an error unless addresses are ignored.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=x86_64-windows-msvc
# DEFINE: %{link} = lld-link -dll -noentry -include:main imp.lib fork.obj \
# DEFINE:   -out:out.dll
# DEFINE: %{dump} = llvm-objdump -d out.dll > out.txt && \
# DEFINE:   llvm-readobj --coff-basereloc out.dll >> out.txt && \
# DEFINE:   FileCheck %s < out.txt

# RUN: lld-link -machine:x64 -def:imp.def -out:imp.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium fork.s -o fork.obj
# RUN: %{mc} --defsym TYPED=1 site.s -o proven.obj
# RUN: %{mc} --defsym TYPED=0 site.s -o unproven.obj
# RUN: rm -f lib.lib && llvm-lib -out:lib.lib unproven.obj

# RUN: %{link} proven.obj && %{dump} --check-prefix=LOAD
# LOAD:      movq {{.*}}(%rip), %rax
# LOAD-NEXT: callq
# LOAD:      BaseReloc [
# LOAD-NEXT: ]

# RUN: not %{link} unproven.obj 2>&1 | FileCheck %s --check-prefix=ERR \
# RUN:   -DIN=unproven.obj -DOBJ=unproven.obj
# RUN: not %{link} lib.lib 2>&1 | FileCheck %s --check-prefix=ERR \
# RUN:   -DIN=lib.lib -DOBJ='lib.lib(unproven.obj)'
# RUN: not %{link} unproven.obj -ignore-function-address-equality:proven.obj \
# RUN:   2>&1 | FileCheck %s --check-prefix=ERR -DIN=unproven.obj \
# RUN:   -DOBJ=unproven.obj
# ERR: error: [[OBJ]]: the reference to fn at .text+0x3 is not classified: no proven path reaches it, decoded from proven starts
# ERR: error: [[OBJ]]: the reference to fn at .text+0x8 is not classified:

# RUN: %{link} unproven.obj -ignore-function-address-equality:Unproven.obj \
# RUN:   && %{dump} --check-prefix=THUNK
# RUN: %{link} lib.lib -ignore-function-address-equality:lib.lib \
# RUN:   && %{dump} --check-prefix=THUNK
# RUN: %{link} proven.obj -ignore-function-address-equality \
# RUN:   && %{dump} --check-prefix=THUNK
# THUNK:      leaq {{.*}}(%rip), %rax
# THUNK-NEXT: callq
# THUNK:      Type: DIR64

## The sites that the object lists take an address, and no other does.
# RUN: %{mc} listed.s -o listed.obj
# RUN: %{link} listed.obj -verbose 2>&1 | FileCheck %s --check-prefix=LISTED \
# RUN:   --implicit-check-not="reference to"
# RUN: %{dump} --check-prefix=LISTED-LOAD
# LISTED-LOAD:      movq {{.*}}(%rip), %rax
# LISTED-LOAD-NEXT: leal {{.*}}(%rip), %eax
# LISTED:      listed.obj: the reference to fn at .text+0x3 takes its address, by the object's tables
# LISTED-NEXT: warning: listed.obj: the reference to fn at .text+0x9 is another operand, by the object's tables
# LISTED-NEXT: listed.obj: the reference to fn at .text+0xe is a branch, by the object's tables

## In a compiler's object a function the tables do not list is only called;
## one they list is decoded.
# RUN: %{mc} --defsym GUARD=1 --defsym ID=1 tabled.s -o guarded.obj
# RUN: %{mc} --defsym GUARD=0 --defsym ID=1 tabled.s -o addrsig.obj
# RUN: %{mc} --defsym GUARD=0 --defsym ID=0 tabled.s -o unknown.obj
# RUN: not %{link} guarded.obj -verbose 2>&1 | FileCheck %s \
# RUN:   --check-prefix=TABLED --implicit-check-not="reference to"
# RUN: not %{link} addrsig.obj -verbose 2>&1 | FileCheck %s \
# RUN:   --check-prefix=TABLED --implicit-check-not="reference to"
# RUN: not %{link} unknown.obj 2>&1 | FileCheck %s --check-prefix=UNKNOWN
# TABLED:      .obj: the reference to fn at .text+0x2 in main is a branch, by the object's tables
# TABLED-NEXT: error: {{.*}}.obj: the reference to fn2 at .text+0x7 in main is not classified: an unknown or divergent encoding, decoded from the function's start
# UNKNOWN: error: unknown.obj: the reference to fn at .text+0x2 in main is not classified: an unknown or divergent encoding, decoded from proven starts
# UNKNOWN: error: unknown.obj: the reference to fn2 at .text+0x7

#--- imp.def
LIBRARY imp.dll
EXPORTS
fn
fn2

#--- fork.s
.data
  .quad 0

#--- site.s
.if TYPED
.def main; .scl 2; .type 32; .endef
.endif
.text
.globl main
main:
  leaq fn(%rip), %rax
  callq fn
  retq
.data
  .quad fn

#--- listed.s
.def fn; .scl 2; .type 32; .endef
.text
.globl main
main:
  leaq fn(%rip), %rax
  leal fn(%rip), %eax
  callq fn
  retq
## .text is section 1, and has two such sites.
.section .unqRlcs,"n"
  .long 1, 2, 3, 9

#--- tabled.s
.if ID
"@comp.id" = 0x01048da7
.endif
.def fn; .scl 2; .type 32; .endef
.def fn2; .scl 2; .type 32; .endef
.def main; .scl 2; .type 32; .endef
.text
.globl main
main:
## No instruction.
  .byte 0x06
  callq fn
  callq fn2
  retq
.if GUARD
"@feat.00" = 0x800
.section .gfids$y,"dr"
  .symidx fn2
.else
.addrsig
.addrsig_sym fn2
.endif
