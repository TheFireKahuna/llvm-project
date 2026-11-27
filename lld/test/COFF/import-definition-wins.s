# REQUIRES: x86, aarch64

## A fork object's reference through __imp_f binds a definition in the link
## over an import library's pointer in every input order, except a vague
## variable the DLL offers; a foreign reference keeps the usual errors.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console \
# DEFINE:   -debug:symtab -out:a.exe
# DEFINE: %{check} = count 0 && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a} %s && \
# DEFINE:   llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=NONE %s
# DEFINE: %{run} = %{mc} %{a}/main.s -o main.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/main.s \
# DEFINE:     -o foreign.obj && \
# DEFINE:   %{mc} def.s -o def.obj && %{mc} %{a}/usev.s -o usev.obj && \
# DEFINE:   %{mc} data.s -o data.obj && %{mc} vague.s -o vague.obj && \
# DEFINE:   %{mc} comdat.s -o comdat.obj && %{mc} %{a}/usew.s -o usew.obj && \
# DEFINE:   llvm-lib -machine:%{a} def.obj -out:def.lib && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:f.dll def.obj data.obj \
# DEFINE:     -export:f -export:v,data -implib:f.lib && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:w.dll comdat.obj \
# DEFINE:     -export:w,data -export:_ZTV1X,data -implib:w.lib && \
# DEFINE:   %{link} main.obj def.obj f.lib 2>&1 | %{check} && \
# DEFINE:   %{link} main.obj f.lib def.obj 2>&1 | %{check} && \
# DEFINE:   %{link} def.obj main.obj f.lib 2>&1 | %{check} && \
# DEFINE:   %{link} def.obj f.lib main.obj 2>&1 | %{check} && \
# DEFINE:   %{link} f.lib main.obj def.obj 2>&1 | %{check} && \
# DEFINE:   %{link} f.lib def.obj main.obj 2>&1 | %{check} && \
# DEFINE:   %{link} main.obj def.lib f.lib 2>&1 | %{check} && \
# DEFINE:   %{link} main.obj f.lib def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} def.lib main.obj f.lib 2>&1 | %{check} && \
# DEFINE:   %{link} def.lib f.lib main.obj 2>&1 | %{check} && \
# DEFINE:   %{link} f.lib main.obj def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} f.lib def.lib main.obj 2>&1 | %{check} && \
# DEFINE:   %{link} -opt:noref main.obj f.lib def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} main.obj usev.obj def.obj data.obj f.lib 2>&1 | \
# DEFINE:     %{check} && \
# DEFINE:   %{link} -opt:noref main.obj usew.obj def.obj comdat.obj w.lib \
# DEFINE:     2>&1 | %{check} && \
# DEFINE:   %{link} -opt:noref main.obj usew.obj def.obj w.lib comdat.obj \
# DEFINE:     2>&1 | %{check} && \
# DEFINE:   %{link} -opt:noref main.obj usev.obj def.obj f.lib vague.obj && \
# DEFINE:   llvm-readobj --coff-imports a.exe | \
# DEFINE:     FileCheck --check-prefix=VAGUE %s && \
# DEFINE:   not %{link} foreign.obj def.obj f.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=DUP %s && \
# DEFINE:   env LLD_IN_TEST=1 not %{link} foreign.obj f.lib def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=REPLACED %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

# x64-LABEL: <main>:
# x64-NEXT:  callq {{.*}} <f>
# x64-NEXT:  callq {{.*}} <g>

# arm64-LABEL: <main>:
# arm64-NEXT:  adr x16, {{.*}} <f>
# arm64-NEXT:  blr x16
# arm64-NEXT:  bl {{.*}} <g>

# NONE-NOT: Import
# VAGUE:     Symbol: v
# VAGUE-NOT: Symbol: f
# DUP:      error: duplicate symbol: f
# REPLACED: error: f was replaced

## The call of g loads the archive member, for which nothing else asks.
#--- x64/main.s
.globl main
main:
  callq *__imp_f(%rip)
  callq g
  retq

#--- x64/usev.s
.globl u
u:
  movq __imp_v(%rip), %rax
  retq

#--- x64/usew.s
.globl u2
u2:
  movq __imp_w(%rip), %rax
  movq __imp__ZTV1X(%rip), %rax
  retq

#--- arm64/main.s
.globl main
main:
  adrp x16, __imp_f
  ldr x16, [x16, :lo12:__imp_f]
  blr x16
  bl g
  ret

#--- arm64/usev.s
.globl u
u:
  adrp x0, __imp_v
  ldr x0, [x0, :lo12:__imp_v]
  ret

#--- arm64/usew.s
.globl u2
u2:
  adrp x0, __imp_w
  ldr x0, [x0, :lo12:__imp_w]
  adrp x1, __imp__ZTV1X
  ldr x1, [x1, :lo12:__imp__ZTV1X]
  ret

## Shared by both machines.
#--- def.s
.globl f, g
f:
  ret
g:
  ret

#--- data.s
  .data
  .globl v
v:
  .quad 1

#--- vague.s
  .section .data$v,"dw",discard,v
  .globl v
v:
  .quad 1
  .link_fact v, 1, 1
  .link_fact v, 2, 1

#--- comdat.s
  .section .data$w,"dw",one_only,w
  .globl w
w:
  .quad 2
  .section .rdata$_ZTV1X,"dr",discard,_ZTV1X
  .globl _ZTV1X
_ZTV1X:
  .quad 0
