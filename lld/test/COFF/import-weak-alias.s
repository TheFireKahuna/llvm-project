# REQUIRES: x86, aarch64

## __imp_ of a fork object's weak ALIAS external (a derived destructor equal to
## its base's) binds as a reference to it: the base's IAT entry, or the base
## directly when defined; a foreign object's keeps link.exe's thunk and warning.
## A DLL's export of it forwards to the base's export, with no thunk or import of
## its own, and its importers import the base's; a foreign object's keeps the
## thunk.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console
# DEFINE: %{run} = \
# DEFINE:   %{mc} derived.s -o derived.obj && %{mc} %{a}/user.s -o user.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/user.s \
# DEFINE:     -o foreign.obj && %{mc} base.s -o base.obj && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:base.dll base.obj \
# DEFINE:     -export:_ZN4BaseD2Ev -implib:base.lib && \
# DEFINE:   %{link} -out:imp.exe user.obj derived.obj base.lib -map:imp.map \
# DEFINE:     2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s && \
# DEFINE:   FileCheck --check-prefix=IMP %s < imp.map && \
# DEFINE:   llvm-readobj --coff-imports imp.exe | \
# DEFINE:     FileCheck --check-prefix=IMPORT %s && \
# DEFINE:   %{link} -out:local.exe user.obj derived.obj base.obj -map:local.map \
# DEFINE:     2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s && \
# DEFINE:   FileCheck --check-prefix=LOCAL %s < local.map && \
# DEFINE:   %{link} -out:foreign.exe foreign.obj derived.obj base.lib \
# DEFINE:     -map:foreign.map 2>&1 | FileCheck --check-prefix=WARN-THUNK %s && \
# DEFINE:   FileCheck --check-prefix=FOREIGN %s < foreign.map && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:derived.dll derived.obj \
# DEFINE:     base.lib -export:_ZN7DerivedD2Ev -implib:derived.lib && \
# DEFINE:   llvm-readobj --coff-exports --coff-imports derived.dll | \
# DEFINE:     FileCheck --check-prefix=FORWARD %s && \
# DEFINE:   %{link} -out:third.exe user.obj derived.lib && \
# DEFINE:   llvm-readobj --coff-imports third.exe | \
# DEFINE:     FileCheck --check-prefix=IMPORT %s && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc derived.s \
# DEFINE:     -o fderived.obj && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:fderived.dll fderived.obj \
# DEFINE:     base.lib -export:_ZN7DerivedD2Ev && \
# DEFINE:   llvm-readobj --coff-exports fderived.dll | \
# DEFINE:     FileCheck --check-prefix=FOREIGN-EXPORT %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# QUIET-NOT: locally defined symbol imported

## __imp__ZN7DerivedD2Ev is the base's entry; there is no local pointer.
# IMP:      __imp__ZN7DerivedD2Ev [[IAT:[0-9a-f]+]] base:base.dll
# IMP-NEXT: __imp__ZN4BaseD2Ev [[IAT]] base:base.dll

# IMPORT:      Name: base.dll
# IMPORT:      Symbol: _ZN4BaseD2Ev
# IMPORT-NOT:  Symbol:

## With the base in the image, both calls reach it directly.
# LOCAL:     _ZN4BaseD2Ev {{[0-9a-f]+}} base.obj
# LOCAL-NOT: __imp_

# WARN-THUNK: warning: foreign.obj: locally defined symbol imported: Base::~Base() (defined in <internal>) [LNK4217]

## A foreign object's __imp__ZN7DerivedD2Ev is no entry of base.dll.
# FOREIGN:     __imp__ZN4BaseD2Ev {{[0-9a-f]+}} base:base.dll
# FOREIGN-NOT: __imp__ZN7DerivedD2Ev {{.*}} base:base.dll

# FORWARD-NOT:  Import {
# FORWARD:      Name: _ZN7DerivedD2Ev
# FORWARD-NEXT: ForwardedTo: base.dll._ZN4BaseD2Ev
# FORWARD-NOT:  Import {

# FOREIGN-EXPORT:      Name: _ZN7DerivedD2Ev
# FOREIGN-EXPORT-NEXT: RVA: 0x

#--- derived.s
.weak _ZN7DerivedD2Ev
.set _ZN7DerivedD2Ev, _ZN4BaseD2Ev

#--- x64/user.s
.def main; .scl 2; .type 32; .endef
.globl main
main:
  call *__imp__ZN7DerivedD2Ev(%rip)
  call _ZN7DerivedD2Ev
  ret

#--- arm64/user.s
.def main; .scl 2; .type 32; .endef
.globl main
main:
  adrp x16, __imp__ZN7DerivedD2Ev
  ldr x16, [x16, :lo12:__imp__ZN7DerivedD2Ev]
  blr x16
  bl _ZN7DerivedD2Ev
  ret

#--- base.s
.globl _ZN4BaseD2Ev
_ZN4BaseD2Ev:
  ret
