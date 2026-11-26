# REQUIRES: x86, aarch64

## A fork object's reference to auto-imported data never takes a runtime pseudo
## relocation (an error, whatever the options); a whole data word is a slot; a
## foreign MinGW reference keeps its pseudo relocation unless it is such a word.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc}-itanium %{a}.s -o fork.obj && \
# DEFINE:   %{mc}-itanium data.s -o data.obj && %{mc}-gnu gnu.s -o gnu.obj && \
# DEFINE:   %{mc}-gnu relocator.s -o relocator.obj && \
# DEFINE:   not %{link} -out:a.dll fork.obj data.obj 2>&1 | FileCheck %s && \
# DEFINE:   not %{link} -out:a.dll fork.obj data.obj -runtime-pseudo-reloc \
# DEFINE:     2>&1 | FileCheck %s && \
# DEFINE:   not %{link} -out:a.dll fork.obj data.obj -lldmingw relocator.obj \
# DEFINE:     2>&1 | FileCheck %s && \
# DEFINE:   %{link} -out:gnu.dll gnu.obj relocator.obj -lldmingw -verbose \
# DEFINE:     2>&1 | FileCheck --check-prefix=GNU %s && \
# DEFINE:   %{mc}-itanium relocator.s -o empty.obj && \
# DEFINE:   %{link} -out:slot.dll gnu.obj empty.obj -lldmingw -verbose 2>&1 | \
# DEFINE:     FileCheck --check-prefix=SLOT %s && \
# DEFINE:   %{mc}-gnu narrow.s -o gnu-narrow.obj && \
# DEFINE:   %{link} -out:both.dll gnu.obj gnu-narrow.obj empty.obj \
# DEFINE:     -lldmingw && llvm-readobj --coff-imports both.dll | \
# DEFINE:     FileCheck --check-prefix=BOTH %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# CHECK-DAG: error: fork.obj: var is provided by imp.dll, but was compiled as local
# CHECK-DAG: error: data.obj: var is provided by imp.dll, but needs a runtime pseudo relocation

# GNU: Writing 1 runtime pseudo relocations

# SLOT:     Automatically importing var from imp.dll
# SLOT-NOT: runtime pseudo relocations

## A pseudo relocation reads the import's entry, which the slot does not take.
# BOTH:     Name: imp.dll
# BOTH:     Name: imp.dll
# BOTH-NOT: Name: imp.dll

#--- x64.s
.globl f
f:
  movl var(%rip), %eax
  ret

#--- arm64.s
.globl f
f:
  adrp x8, var
  ldr w0, [x8, :lo12:var]
  ret

#--- data.s
.data
.globl p
p:
  .long var

#--- narrow.s
.data
  .long var

#--- gnu.s
.data
.globl p
p:
  .quad var

#--- relocator.s
.globl _pei386_runtime_relocator
_pei386_runtime_relocator:
  ret

#--- imp.def
LIBRARY imp.dll
EXPORTS
  var DATA
