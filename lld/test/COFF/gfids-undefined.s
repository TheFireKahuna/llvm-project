# REQUIRES: x86, aarch64

## A fork object lists every undefined address-taken symbol in .gfids$y; the
## guard function table keeps only those a function defines.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium -I %S/Inputs
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc} refs.s -o refs.obj && %{mc} defs.s -o defs.obj && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -guard:cf imp.lib refs.obj \
# DEFINE:     defs.obj -out:a.dll -map:a.map && \
# DEFINE:   llvm-readobj --coff-load-config a.dll > a.txt && \
# DEFINE:   cat a.map >> a.txt && FileCheck %s < a.txt

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# CHECK:      GuardCFFunctionCount: 1
# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x[[#%X,FN:]]
# CHECK-NEXT: ]
# CHECK:      fn {{0*}}[[#FN]] defs.obj

#--- imp.def
LIBRARY imp.dll
EXPORTS
var DATA

#--- refs.s
.data
  .quad fn
  .quad object
  .quad label
  .quad var
.include "loadconfig-cfg-iat.s"

#--- defs.s
.text
.def fn; .scl 2; .type 32; .endef
.globl fn
fn:
  ret
.data
.globl object
object:
  .quad 5
.globl label
label:
  .quad 6
