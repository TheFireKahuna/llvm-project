# REQUIRES: x86, aarch64

## Weakness belongs to the reference: an object with no weak reference to fin
## refers to it strongly through __imp_fin (loading the member, no null
## default), whichever producer made it, unless the link has no fork object;
## a hidden or protected weak reference keeps its null default.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console \
# DEFINE:   -debug:symtab -verbose -out:a.exe
# DEFINE: %{check} = FileCheck --check-prefix=LOAD %s && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a} %s
# DEFINE: %{own} = \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a}-weak %s && \
# DEFINE:   llvm-readobj --coff-imports a.exe | FileCheck --allow-empty \
# DEFINE:     --implicit-check-not=def.dll %s
# DEFINE: %{run} = %{mc} %{a}/strong.s -o strong.obj && \
# DEFINE:   %{mc} %{a}/weak.s -o weak.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/strong.s \
# DEFINE:     -o foreign.obj && %{mc} def.s -o def.obj && \
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
# DEFINE:     FileCheck --check-prefix=LOAD %s && \
# DEFINE:   %{link} weak.obj foreign.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=LOAD %s && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/weak.s \
# DEFINE:     -o foreign-weak.obj && \
# DEFINE:   %{link} foreign.obj foreign-weak.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=WEAK %s && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry def.obj -export:fin \
# DEFINE:     -out:def.dll -implib:imp.lib && \
# DEFINE:   %{mc} %{a}/weak.s --defsym VIS=3 -o hidden.obj && \
# DEFINE:   %{mc} %{a}/weak.s --defsym VIS=2 -o protected.obj && \
# DEFINE:   %{link} -entry:other hidden.obj imp.lib && %{own} && \
# DEFINE:   %{link} -entry:other imp.lib hidden.obj && %{own} && \
# DEFINE:   %{link} -entry:other protected.obj imp.lib && %{own}

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
# arm64-NEXT:  adr x16, 0x[[#%x,FIN:]]
# arm64-NEXT:  blr x16
# arm64:       {{0*}}[[#FIN]] <fin>:

# arm64-weak-LABEL: <other>:
# arm64-weak-NEXT:  mov x0, #0x0

#--- x64/strong.s
.globl main
main:
  callq *__imp_fin(%rip)
  retq

## VIS states the weak reference's visibility.
#--- x64/weak.s
.globl other
other:
  movq __imp_fin(%rip), %rax
  retq
.weak fin
.ifdef VIS
.link_fact fin, 1, VIS
.endif

#--- def.s
.globl fin
fin:
  ret

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
.ifdef VIS
.link_fact fin, 1, VIS
.endif
