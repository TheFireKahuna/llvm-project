# REQUIRES: x86, aarch64

## A fork image starts .rdata with the IAT so ARM64 code reaches import
## pointers by one literal load; other images keep the table where it was.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib
# DEFINE: %{dump} = llvm-readobj --sections --coff-imports
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   %{mc}-itanium use.s -o fork.obj && \
# DEFINE:   %{mc}-msvc use.s -o other.obj && %{link} fork.obj -out:fork.dll && \
# DEFINE:   %{link} other.obj -out:other.dll && \
# DEFINE:   %{dump} fork.dll | FileCheck --check-prefix=FIRST %s && \
# DEFINE:   %{dump} other.dll | FileCheck --check-prefix=AFTER %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# FIRST:      Name: .rdata
# FIRST:      VirtualAddress: 0x[[#%X,RDATA:]]
# FIRST:      ImportAddressTableRVA: 0x[[#RDATA]]{{$}}

# AFTER:      Name: .rdata
# AFTER:      VirtualAddress: 0x[[#%X,RDATA:]]
# AFTER-NOT:  ImportAddressTableRVA: 0x[[#RDATA]]{{$}}

#--- imp.def
LIBRARY imp.dll
EXPORTS
  fn

#--- use.s
  .section .rdata,"dr"
  .quad 1
  .data
  .quad __imp_fn
