# REQUIRES: x86, aarch64

## An object for Windows Itanium or NT-POSIX that refers to data an import
## offers only as __imp_X imports it automatically, but never takes a runtime
## pseudo relocation: each reference that would need one is an error, whatever
## -runtime-pseudo-reloc and MinGW mode say. A foreign MinGW reference keeps
## its pseudo relocation.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib
# DEFINE: %{run} = \
# DEFINE:   lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc}-itanium %{a}.s -o fork.obj && \
# DEFINE:   %{mc}-itanium data.s -o data.obj && \
# DEFINE:   %{mc}-gnu data.s -o gnu.obj && \
# DEFINE:   %{mc}-gnu relocator.s -o relocator.obj && \
# DEFINE:   not %{link} -out:a.dll fork.obj data.obj 2>&1 | FileCheck %s && \
# DEFINE:   not %{link} -out:a.dll fork.obj data.obj -runtime-pseudo-reloc \
# DEFINE:     2>&1 | FileCheck %s && \
# DEFINE:   not %{link} -out:a.dll fork.obj data.obj -lldmingw relocator.obj \
# DEFINE:     2>&1 | FileCheck %s && \
# DEFINE:   %{link} -out:gnu.dll gnu.obj relocator.obj -lldmingw -verbose \
# DEFINE:     2>&1 | FileCheck --check-prefix=GNU %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# CHECK-DAG: error: fork.obj: automatic import of var from imp.dll needs a runtime pseudo relocation; declare it dllimport, or refer to __imp_var
# CHECK-DAG: error: data.obj: automatic import of var from imp.dll needs a runtime pseudo relocation; declare it dllimport, or refer to __imp_var

# GNU: Writing 1 runtime pseudo relocations

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
  .quad var

#--- relocator.s
.globl _pei386_runtime_relocator
_pei386_runtime_relocator:
  ret

#--- imp.def
LIBRARY imp.dll
EXPORTS
  var DATA
