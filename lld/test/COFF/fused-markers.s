# REQUIRES: x86, aarch64

## The marker of a fused site that the link does not bind writes nothing and
## uses no pointer, whatever became of the pointer that it names, and keeps
## the fused reads of that pointer in its section: such a read is the load
## that it is, the address itself where the pointer's symbol is in the image.
## The call through another pointer beside it is bound all the same.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab
# DEFINE: %{dump} = llvm-objdump -d --no-show-raw-insn

# RUN: %{mc} -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium defs.s -o defs-x64.obj
# RUN: lld-link -dll -noentry -export:f -export:v,data defs-x64.obj \
# RUN:   -out:d-x64.dll -implib:d-x64.lib
# RUN: %{link} -out:x64.exe x64.obj defs-x64.obj 2>&1 | count 0
# RUN: %{dump} x64.exe | FileCheck --check-prefix=X64 %s
# RUN: llvm-readobj --sections x64.exe | FileCheck --check-prefix=NOPTR %s
# RUN: %{link} -out:x64-imp.exe x64.obj d-x64.lib
# RUN: %{dump} x64-imp.exe | FileCheck --check-prefix=X64-IMP %s

# X64-LABEL: <main>:
# X64-NEXT:    leaq {{.*}} <v>
# X64-NEXT:    movl (%rax), %ecx
# X64-NEXT:    callq {{.*}} <f>
# X64-NEXT:    leaq {{.*}} <v>
# X64-NEXT:    leaq 0x4(%rax), %rax
# X64-NEXT:    retq

## The image holds no pointer to v or to f.
# NOPTR-NOT: Name: .rdata

# X64-IMP-LABEL: <main>:
# X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
# X64-IMP-NEXT:    movl (%rax), %ecx
# X64-IMP-NEXT:    movq {{.*}}(%rip), %rdx
# X64-IMP-NEXT:    callq *%rdx
# X64-IMP-NEXT:    movq {{.*}}(%rip), %rax
# X64-IMP-NEXT:    leaq 0x4(%rax), %rax
# X64-IMP-NEXT:    retq

# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium defs.s -o defs-a64.obj
# RUN: lld-link -dll -noentry -export:f -export:v,data defs-a64.obj \
# RUN:   -out:d-a64.dll -implib:d-a64.lib
# RUN: %{link} -out:a64.exe a64.obj defs-a64.obj 2>&1 | count 0
# RUN: %{dump} a64.exe | FileCheck --check-prefix=A64 %s
# RUN: llvm-readobj --sections a64.exe | FileCheck --check-prefix=NOPTR %s
# RUN: %{link} -out:a64-imp.exe a64.obj d-a64.lib
# RUN: %{dump} a64-imp.exe | FileCheck --check-prefix=A64-IMP %s

# A64-LABEL: <main>:
# A64-NEXT:    adrp x8, 0x[[#%x,]] <v>
# A64-NEXT:    add x8, x8, #0x0
# A64-NEXT:    ldr w9, [x8]
# A64-NEXT:    bl 0x[[#%x,]] <f>
# A64-NEXT:    adrp x8, 0x[[#%x,]] <v>
# A64-NEXT:    add x8, x8, #0x0
# A64-NEXT:    add x0, x8, #0x4
# A64-NEXT:    ret

# A64-IMP-LABEL: <main>:
# A64-IMP-NEXT:    ldr x8, 0x[[#%x,]] <__imp_v>
# A64-IMP-NEXT:    ldr w9, [x8]
# A64-IMP-NEXT:    ldr x10, 0x[[#%x,]] <__imp_f>
# A64-IMP-NEXT:    blr x10
# A64-IMP-NEXT:    ldr x8, 0x[[#%x,]] <__imp_v>
# A64-IMP-NEXT:    add x0, x8, #0x4
# A64-IMP-NEXT:    ret

#--- x64.s
  .text
  .globl main
main:
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax), %ecx
  movq __imp_f@FUSED(%rip), %rdx
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  cs
  cs
  cs
  cs
  callq *%rdx
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 4(%rax), %rax
  retq

#--- a64.s
  .text
  .globl main
main:
  adrp x8, __imp_v
  ldr x8, [x8, :fused_lo12:__imp_v]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  ldr w9, [x8]
  adrp x10, __imp_f
  ldr x10, [x10, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x10
  adrp x8, __imp_v
  ldr x8, [x8, :fused_lo12:__imp_v]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  add x0, x8, #4
  ret

#--- defs.s
  .text
  .globl f
f:
  ret
  .data
  .globl v
v:
  .long 1, 2
