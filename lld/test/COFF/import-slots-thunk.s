# REQUIRES: x86, aarch64

## A thunk only in-place slots refer to leaves the image and its guard table,
## the slots being listed as address-taken IAT entries; a call, a jump, a root,
## an export or a foreign local import pointer keeps it.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry -guard:cf imp.lib \
# DEFINE:   data.obj
# DEFINE: %{dump} = llvm-readobj --sections --coff-load-config
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc}-itanium -I %S/Inputs data.s -o data.obj && \
# DEFINE:   %{mc}-itanium var.s -o var.obj && \
# DEFINE:   %{mc}-msvc foreign.s -o foreign.obj && \
# DEFINE:   %{mc}-itanium %{a}.s -o call.obj && \
# DEFINE:   %{mc}-msvc alias.s -o alias.obj && \
# DEFINE:   %{link} var.obj -out:slots.dll && \
# DEFINE:   %{dump} slots.dll | FileCheck --check-prefix=SLOTS %s && \
# DEFINE:   %{link} foreign.obj -out:foreign.dll && \
# DEFINE:   %{dump} foreign.dll | FileCheck --check-prefix=FOREIGN %s && \
# DEFINE:   %{link} call.obj -out:call.dll && \
# DEFINE:   %{dump} call.dll | FileCheck --check-prefix=KEPT %s && \
# DEFINE:   %{link} -include:fn -out:root.dll && \
# DEFINE:   %{dump} root.dll | FileCheck --check-prefix=KEPT %s && \
# DEFINE:   %{link} -export:fn -out:export.dll && \
# DEFINE:   %{dump} export.dll | FileCheck --check-prefix=KEPT %s && \
# DEFINE:   %{link} alias.obj -out:alias.dll && \
# DEFINE:   %{dump} alias.dll | FileCheck --check-prefix=KEPT %s

# RUN: %{run}
# RUN: %{mc}-itanium jump.s -o jump.obj
# RUN: %{link} jump.obj -out:jump.dll
# RUN: llvm-objdump -d jump.dll | FileCheck --check-prefix=JUMP %s
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## The two slots that hold functions, in .rdata and .data, are listed; the one
## that holds data is not.
# SLOTS-NOT:  Name: .text
# SLOTS:      GuardAddressTakenIatEntryCount: 2
# SLOTS:      GuardIatTable [
# SLOTS-NEXT:   0x180001{{.*}}
# SLOTS-NEXT:   0x180002000
# SLOTS-NEXT: ]

## An object without guard metadata takes the address of what its relocations
## name, which a slot's thunk no longer is.
# FOREIGN-NOT: Name: .text
# FOREIGN:     GuardCFFunctionCount: 0
# FOREIGN:     GuardAddressTakenIatEntryCount: 3

# KEPT:      Name: .text
# KEPT:      GuardAddressTakenIatEntryCount: 2

## A jump to the import reaches the thunk, which stays.
# JUMP: : 0f 84 {{.*}} je 0x[[#%x,THUNK:]]
# JUMP: [[#THUNK]]: ff 25 {{.*}} jmpq *

#--- imp.def
LIBRARY imp.dll
EXPORTS
fn
var DATA

#--- data.s
.section .rdata,"dr"
  .quad fn
.data
  .quad fn
.include "loadconfig-cfg-iat.s"

#--- var.s
.section .rdata,"dr"
  .quad var

#--- foreign.s
.data
  .quad fn

#--- alias.s
.weak alias
alias = fn
.data
  .quad __imp_alias

#--- x64.s
.text
.globl caller
caller:
  callq fn
  ret

#--- jump.s
.text
.globl jumper
jumper:
  je fn
  ret

#--- arm64.s
.text
.globl caller
caller:
  bl fn
  ret
