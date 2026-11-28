# REQUIRES: x86, aarch64

## Weakness belongs to the reference. An object for Windows Itanium or
## NT-POSIX reaches fin through __imp_fin, so one that keeps no weak reference
## to fin refers to fin strongly: beside another object's weak reference, in
## either order, it drops the null default and loads the archive member that
## defines fin, wherever the archive is read, and an absent fin is an undefined
## symbol, never a call through null. The object that keeps fin weak refers to
## it weakly through its pointer too, and a foreign object's __imp_fin is no
## reference to fin.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console \
# DEFINE:   -debug:symtab -verbose -out:a.exe
# DEFINE: %{check} = FileCheck --check-prefix=LOAD %s && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a} %s
# DEFINE: %{run} = \
# DEFINE:   %{mc} %{a}/strong.s -o strong.obj && \
# DEFINE:   %{mc} %{a}/weak.s -o weak.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/strong.s \
# DEFINE:     -o foreign.obj && \
# DEFINE:   %{mc} %{a}/def.s -o def.obj && \
# DEFINE:   llvm-lib -machine:%{a} def.obj -out:def.lib && \
# DEFINE:   %{link} strong.obj weak.obj def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} weak.obj strong.obj def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} def.lib weak.obj strong.obj 2>&1 | %{check} && \
# DEFINE:   %{link} weak.obj def.lib strong.obj 2>&1 | %{check} && \
# DEFINE:   %{link} weak.obj strong.obj def.obj && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a} %s && \
# DEFINE:   not %{link} strong.obj weak.obj 2>&1 | \
# DEFINE:     FileCheck --check-prefix=UNDEF %s && \
# DEFINE:   not %{link} weak.obj strong.obj 2>&1 | \
# DEFINE:     FileCheck --check-prefix=UNDEF %s && \
# DEFINE:   %{link} -entry:other weak.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=WEAK %s && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a}-weak %s && \
# DEFINE:   %{link} foreign.obj weak.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=WEAK %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# LOAD: Loaded def.lib(def.obj) for fin
# WEAK-NOT: for fin
# UNDEF: error: undefined symbol: __declspec(dllimport) fin

# x64-LABEL: <main>:
# x64-NEXT:  callq {{.*}} <fin>

# x64-weak-LABEL: <other>:
# x64-weak-NEXT:  movq $0x0, %rax

# arm64-LABEL: <main>:
# arm64-NEXT:  adrp x16, 0x[[#%x,PAGE:]]
# arm64-NEXT:  add x16, x16, #0x[[#%x,OFF:]]
# arm64-NEXT:  blr x16
# arm64:       {{0*}}[[#%x,PAGE+OFF]] <fin>:

# arm64-weak-LABEL: <other>:
# arm64-weak-NEXT:  nop
# arm64-weak-NEXT:  mov x0, #0x0

#--- x64/strong.s
.globl main
main:
  callq *__imp_fin(%rip)
  retq

#--- x64/weak.s
.globl other
other:
  movq __imp_fin(%rip), %rax
  retq
.weak fin

#--- x64/def.s
.globl fin
fin:
  retq

#--- arm64/strong.s
.globl main
main:
  adrp x16, __imp_fin
  ldr x16, [x16, :lo12:__imp_fin]
  blr x16
  ret

#--- arm64/weak.s
.globl other
other:
  adrp x0, __imp_fin
  ldr x0, [x0, :lo12:__imp_fin]
  ret
.weak fin

#--- arm64/def.s
.globl fin
fin:
  ret
