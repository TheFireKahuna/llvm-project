# REQUIRES: x86, aarch64

## For a fork object, /alternatename:A=B with only __imp_A undefined binds
## __imp_A to __imp_B or a local pointer to B; an undefined alternate leaves
## __imp_A undefined, and a foreign reference keeps link.exe's error.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console
# DEFINE: %{run} = %{mc} %{a}/x.s -o x.obj && \
# DEFINE:   sed s/__imp_x/__imp_y/ %{a}/x.s | %{mc} -o y.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/x.s \
# DEFINE:     -o foreign.obj && \
# DEFINE:   %{mc} start.s -o start.obj && %{mc} impl.s -o impl.obj && \
# DEFINE:   llvm-lib -machine:%{a} start.obj impl.obj -out:rt.lib && \
# DEFINE:   %{link} -out:local.exe x.obj rt.lib -verbose 2>&1 | \
# DEFINE:     FileCheck --check-prefix=LOCAL %s && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:impl.dll impl.obj \
# DEFINE:     -export:x_impl -implib:impl.lib && \
# DEFINE:   %{link} -out:dll.exe x.obj start.obj impl.lib -map:dll.map && \
# DEFINE:   FileCheck --check-prefix=MAP %s < dll.map && \
# DEFINE:   llvm-readobj --coff-imports dll.exe | \
# DEFINE:     FileCheck --check-prefix=IMPORT %s && \
# DEFINE:   not %{link} -out:missing.exe y.obj rt.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=MISSING %s && \
# DEFINE:   not %{link} -out:foreign.exe foreign.obj rt.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=FOREIGN %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## __imp_x asks for x_impl, which binds to it.
# LOCAL:     Loaded rt.lib(impl.obj) for x_impl
# LOCAL-NOT: undefined symbol

## No thunk or local pointer: __imp_x is x_impl's IAT entry.
# MAP-NOT:  0001:{{.*}} x_impl
# MAP:      __imp_x [[IAT:[0-9a-f]+]] impl:impl.dll
# MAP-NEXT: __imp_x_impl [[IAT]] impl:impl.dll

# IMPORT:      Name: impl.dll
# IMPORT:      Symbol: x_impl

# MISSING:     error: undefined symbol: __declspec(dllimport) y
# MISSING-NOT: error: undefined symbol: y

# FOREIGN: error: undefined symbol: __declspec(dllimport) x

#--- x64/x.s
.globl main
main:
  call *__imp_x(%rip)
  call start
  ret

#--- arm64/x.s
.globl main
main:
  adrp x16, __imp_x
  ldr x16, [x16, :lo12:__imp_x]
  blr x16
  bl start
  ret

## The member that the image loads anyway names the alternates.
#--- start.s
.globl start
start:
  ret
.section .drectve,"yn"
.ascii " /alternatename:x=x_impl /alternatename:y=y_impl"

#--- impl.s
.globl x_impl
x_impl:
  ret
