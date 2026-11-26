# REQUIRES: x86, aarch64

## A fork object's address-take of an import loads it from the entry (lea to
## mov, adrp+add to one literal ldr in reach, else adrp+ldr), listing the entry
## as address-taken; delay-loaded and -ignore-function-address-equality
## addresses stay the thunk's.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -dll -noentry -guard:cf -include:main \
# DEFINE:   -call-graph-profile-sort:none

# RUN: lld-link -machine:x64 -def:imp.def -out:imp.lib
# RUN: %{mc} -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium %S/Inputs/loadconfig-cfg-iat.s -o config.obj
# RUN: %{link} imp.lib x64.obj config.obj -out:x64.dll
# RUN: llvm-objdump -d x64.dll > x64.txt
# RUN: llvm-objdump -s -j .data x64.dll >> x64.txt
# RUN: llvm-readobj --coff-load-config x64.dll >> x64.txt
# RUN: FileCheck --check-prefix=X64 %s < x64.txt

# X64:      48 8b 05 {{.*}} movq 0x[[#%x,]](%rip), %rax {{.*}}# 0x[[#%x,FN:]]
# X64-NEXT: d5 48 8b 25 {{.*}} movq {{.*}}(%rip), %r20 {{.*}}# 0x[[#FN]]
# X64-NEXT: 48 8b 0d {{.*}} movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#FN+16]]
# X64-NEXT: 48 8b 15 {{.*}} movq {{.*}}(%rip), %rdx {{.*}}# 0x[[#FN+24]]
# X64-NEXT: e8 {{.*}} callq 0x[[#%x,THUNK:]]
# X64-NEXT: retq
## fn2's thunk is the only one, and the call-only fields hold its address.
# X64:      [[#THUNK]]: ff 25 {{.*}} jmpq *{{.*}}(%rip) {{.*}}# 0x[[#FN+8]]
# X64-NOT:  jmpq
# X64:      Contents of section .data:
# X64-NEXT: 180003000 {{[0-9a-f]+}} {{[0-9a-f]+}}
# X64:      GuardAddressTakenIatEntryCount: 2
# X64:      GuardIatTable [
# X64-NEXT:   0x[[#%X,FN]]
# X64-NEXT:   0x180003008
# X64-NEXT: ]

# RUN: lld-link -machine:arm64 -def:imp.def -out:imp-a64.lib
# RUN: %{mc} -triple=aarch64-windows-itanium arm64.s -o arm64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium %S/Inputs/loadconfig-cfg-iat.s \
# RUN:   -o config-a64.obj
# RUN: %{link} imp-a64.lib arm64.obj config-a64.obj -out:arm64.dll
# RUN: llvm-objdump -d arm64.dll > arm64.txt
# RUN: llvm-readobj --coff-load-config arm64.dll >> arm64.txt
# RUN: FileCheck --check-prefix=A64 %s < arm64.txt

# A64:      ldr x0, 0x[[#%x,PAGE:]]
# A64-NEXT: ldr x1, 0x[[#PAGE+0x10]]
# A64-NEXT: bl 0x[[#%x,THUNK:]]
# A64-NEXT: ret
# A64-NEXT: ldr x16, 0x[[#PAGE+8]]
# A64-NEXT: br x16
# A64-NOT:  br x16
# A64:      GuardAddressTakenIatEntryCount: 1
# A64:      GuardIatTable [
# A64-NEXT:   0x[[#%X,PAGE]]
# A64-NEXT: ]

## A foreign object's proven address-take loads the address too, unless
## -ignore-function-address-equality names it.
# RUN: %{mc} -triple=x86_64-windows-itanium fn.s -o fn.obj
# RUN: %{link} imp.lib fn.obj config.obj -out:delay.dll -delayload:imp.dll \
# RUN:   -alternatename:__delayLoadHelper2=main
# RUN: llvm-objdump -d delay.dll | FileCheck --check-prefix=KEPT %s
# RUN: %{mc} -triple=x86_64-windows-msvc fn.s -o foreign.obj
# RUN: %{link} imp.lib foreign.obj config.obj -out:foreign.dll
# RUN: llvm-objdump -d foreign.dll | FileCheck --check-prefix=LOADED %s
# RUN: %{link} imp.lib foreign.obj config.obj -out:foreign.dll \
# RUN:   -ignore-function-address-equality
# RUN: llvm-objdump -d foreign.dll | FileCheck --check-prefix=KEPT %s
# RUN: %{mc} -triple=aarch64-windows-msvc fn-a64.s -o foreign-a64.obj
# RUN: %{link} imp-a64.lib foreign-a64.obj config-a64.obj -out:foreign-a64.dll
# RUN: llvm-objdump -d foreign-a64.dll | FileCheck --check-prefix=LOADED-A64 %s
# RUN: %{link} imp-a64.lib foreign-a64.obj config-a64.obj -out:foreign-a64.dll \
# RUN:   -ignore-function-address-equality:foreign-a64.obj
# RUN: llvm-objdump -d foreign-a64.dll | FileCheck --check-prefix=KEPT-A64 %s

# KEPT:      <.text>:
# KEPT-NEXT: 48 8d 05 {{.*}} leaq
# KEPT-A64:      <.text>:
# KEPT-A64-NEXT: adrp x0
# KEPT-A64-NEXT: add x0, x0
# LOADED:      <.text>:
# LOADED-NEXT: 48 8b 05 {{.*}} movq
# LOADED-A64:      <.text>:
# LOADED-A64-NEXT: adrp x0
# LOADED-A64-NEXT: ldr x0, [x0

## Nor is a hybrid image's code rewritten.
# RUN: llvm-mc -filetype=obj -triple=arm64ec-windows \
# RUN:   %S/Inputs/loadconfig-arm64ec.s -o loadconfig.obj
# RUN: %{mc} -triple=arm64ec-windows icall.s -o icall.obj
# RUN: llvm-lib -machine:arm64ec -def:imp.def -out:imp-ec.lib
# RUN: lld-link -machine:arm64ec -dll -noentry imp-ec.lib fn.obj icall.obj \
# RUN:   loadconfig.obj -out:hybrid.dll -include:main
# RUN: llvm-objdump -d hybrid.dll | FileCheck --check-prefix=HYBRID %s
# HYBRID: 48 8d 05 {{.*}} leaq {{.*}} <.text+0x1010>
# HYBRID: [[#%x,]]010: ff 25

# RUN: %{mc} -triple=x86_64-windows-itanium bad.s -o bad.obj
## A lea's relocation over a mov, a lea with an offset, and no REX.W.
# DEFINE: %{bytes} = %{mc} -triple=x86_64-windows-itanium bytes.s --defsym
# RUN: %{mc} -triple=x86_64-windows-itanium lea.s -o lea0.obj
# RUN: %{bytes} REX=0x48 --defsym OP=0x8b --defsym DISP=0 -o mov.o
# RUN: %{bytes} REX=0x48 --defsym OP=0x8d --defsym DISP=8 -o offset.o
# RUN: %{bytes} REX=0x90 --defsym OP=0x8d --defsym DISP=0 -o narrow.o
# RUN: llvm-objcopy --dump-section=.text=mov.bin mov.o
# RUN: llvm-objcopy --dump-section=.text=offset.bin offset.o
# RUN: llvm-objcopy --dump-section=.text=narrow.bin narrow.o
# RUN: llvm-objcopy --update-section=.text=mov.bin lea0.obj lea.obj
# RUN: llvm-objcopy --update-section=.text=offset.bin lea0.obj offset.obj
# RUN: llvm-objcopy --update-section=.text=narrow.bin lea0.obj narrow.obj
# RUN: not %{link} imp.lib bad.obj lea.obj offset.obj narrow.obj config.obj \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=BAD %s

# BAD-DAG: error: bad.obj: .text+0x3: addresses fn+8, but imp.dll exports neither fn$so8 nor fn$ap8
# BAD-DAG: error: bad.obj: var is provided by imp.dll, but was compiled as local
# BAD-DAG: error: lea.obj: .text+0x3: IMAGE_REL_LLVM_LEA
# BAD-DAG: error: offset.obj: .text+0x3: IMAGE_REL_LLVM_LEA
# BAD-DAG: error: narrow.obj: .text+0x3: IMAGE_REL_LLVM_LEA

# RUN: %{mc} -triple=aarch64-windows-itanium bad-a64.s -o bad-a64.obj
# RUN: not %{link} imp-a64.lib bad-a64.obj config-a64.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD-A64 %s

# BAD-A64-DAG: error: bad-a64.obj: var is provided by imp.dll, but was compiled as local

#--- imp.def
LIBRARY imp.dll
EXPORTS
fn
fn2
var DATA
var$so8 DATA

#--- x64.s
.text
.globl main
main:
  leaq fn(%rip), %rax
  leaq fn(%rip), %r20
  leaq var(%rip), %rcx
  leaq var+8(%rip), %rdx
  callq fn2
  ret
.data
  .long fn2@IMGREL_CALL
  .long fn2@REL32_CALL
  .quad fn
  .quad var+8

#--- arm64.s
.text
.globl main
main:
  adrp x0, fn
  add x0, x0, :lo12:fn
  adrp x1, var
  add x1, x1, :lo12:var
  bl fn2
  ret

#--- fn.s
.def main; .scl 2; .type 32; .endef
.text
.globl main
main:
  leaq fn(%rip), %rax
  ret

#--- fn-a64.s
.text
.globl main
main:
  adrp x0, fn
  add x0, x0, :lo12:fn
  ret

#--- icall.s
.globl __icall_helper_arm64ec
__icall_helper_arm64ec:
  ret

#--- bad.s
.text
.globl main
main:
  leaq fn+8(%rip), %rax
  leal var(%rip), %eax

#--- lea.s
.text
  leaq fn(%rip), %rax

#--- bytes.s
.text
  .byte REX, OP, 0x05
  .long DISP

#--- bad-a64.s
.text
.globl main
main:
  adrp x3, var
  nop
  add x3, x3, :lo12:var
