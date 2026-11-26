# REQUIRES: x86, aarch64

## A fork object's local access to data another image provides is an error
## naming symbol, object, DLL and fix; the GOT spelling reaches the import.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc} %{a}-local.s -o local.obj && %{mc} %{a}-got.s -o got.obj && \
# DEFINE:   not %{link} -out:local.dll local.obj 2>&1 | \
# DEFINE:     FileCheck --check-prefix=C7-%{a} %s && \
# DEFINE:   %{link} -out:got.dll got.obj && \
# DEFINE:   llvm-readobj --coff-imports got.dll | FileCheck --check-prefix=IMP %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# C7-x64:   error: local.obj: var is provided by imp.dll, but was compiled as local
# C7-arm64: error: local.obj: var is provided by imp.dll, but was compiled as local

# IMP:      Name: imp.dll
# IMP:      Symbol: var

#--- x64-local.s
.globl f
f:
  movl var(%rip), %eax
  ret

#--- x64-got.s
.globl f
f:
  movq var@GOTPCREL(%rip), %rax
  movl (%rax), %eax
  ret

#--- arm64-local.s
.globl f
f:
  adrp x8, var
  ldr w0, [x8, :lo12:var]
  ret

#--- arm64-got.s
.globl f
f:
  adrp x8, :got:var
  ldr x8, [x8, :got_lo12:var]
  ldr w0, [x8]
  ret

#--- imp.def
LIBRARY imp.dll
EXPORTS
  var DATA
