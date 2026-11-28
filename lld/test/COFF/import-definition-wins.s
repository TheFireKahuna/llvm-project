# REQUIRES: x86, aarch64

## For a reference from an object for Windows Itanium or NT-POSIX, which
## reaches a function that may be imported through __imp_f, a definition of f
## in the link beats an import library's pointer, as an executable's
## definition beats a shared object's on ELF: in every order of the referring
## object, the definition, in an object or in an archive member that the link
## loads, and the import library, the reference binds the definition and
## nothing is imported. Data binds so too, except vague data, which keeps the
## import. A foreign object's reference keeps the errors of a link without
## such objects.

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
# DEFINE:   %{mc} %{a}/def.s -o def.obj && %{mc} %{a}/usev.s -o usev.obj && \
# DEFINE:   %{mc} data.s -o data.obj && %{mc} vague.s -o vague.obj && \
# DEFINE:   llvm-lib -machine:%{a} def.obj -out:def.lib && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry -out:f.dll def.obj data.obj \
# DEFINE:     -export:f -export:v,data -implib:f.lib && \
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

#--- x64/def.s
.globl f, g
f:
  retq
g:
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

#--- arm64/def.s
.globl f, g
f:
  ret
g:
  ret

## Shared by both machines.
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
