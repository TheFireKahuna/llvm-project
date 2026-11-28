# REQUIRES: x86, aarch64

## A jump through a pointer, or a fused jump through a register that held
## one, whose type says that the unwinder allows its deletion is a BRANCH_TAIL
## once the link has made it a direct jump: it is deleted where its target's
## section follows it, and the function falls through. One without that type
## stays a jump, and so does the jump that is all of a function whose address
## is significant, which would else take its target's. In a section that keeps
## its size the jump stays direct.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab -verbose

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: llvm-readobj -r x64.obj | FileCheck --check-prefix=TYPES %s
# RUN: %{link} x64.obj -out:x64.exe 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d x64.exe | FileCheck --check-prefix=X64 %s

# TYPES: IMAGE_REL_LLVM_FUSED_JUMP_TAIL __imp_f
# TYPES: IMAGE_REL_LLVM_MEM_JUMP_TAIL __imp_g
# TYPES: IMAGE_REL_LLVM_FUSED_JUMP __imp_h

# LOG: deleting 2 tail jumps

# X64:       <main>:
# X64-NEXT:  xorl %ecx, %ecx
# X64-EMPTY:
# X64-NEXT:  <f>:
# X64-NEXT:  xorl %eax, %eax
# X64-EMPTY:
# X64-NEXT:  <g>:
# X64-NEXT:  retq
# X64-EMPTY:
# X64-NEXT:  <framed>:
# X64-NEXT:  pushq %rbx
# X64-NEXT:  popq %rbx
# X64-NEXT:  : eb 00 jmp {{.*}} <h>
# X64:       <forwarder>:
# X64-NEXT:  : eb 00 jmp {{.*}} <last>

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium keep.s -o keep.obj
# RUN: %{link} keep.obj -out:keep.exe 2>&1 | FileCheck --check-prefix=KEEP-LOG %s
# RUN: llvm-objdump -d keep.exe | FileCheck --check-prefix=KEEP %s

# KEEP-LOG: keep.obj: .text$k keeps its size: a value would not fit once it is resized

# KEEP:      <main>:
# KEEP:      : e9 {{.*}} jmp {{.*}} <f>
# KEEP-NEXT: : cc int3
# KEEP-EMPTY:
# KEEP-NEXT: <f>:

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{link} a64.obj -out:a64.exe 2>&1 | FileCheck --check-prefix=A64-LOG %s
# RUN: llvm-objdump -d a64.exe | FileCheck --check-prefix=A64 %s

# A64-LOG: deleting 1 tail jumps

# A64:       <main>:
# A64-NEXT:  mov w0, #0x1
# A64-EMPTY:
# A64-NEXT:  <f>:
# A64-NEXT:  ret
# A64-EMPTY:
# A64-NEXT:  <framed>:
# A64-NEXT:  sub sp, sp, #0x10
# A64-NEXT:  add sp, sp, #0x10
# A64-NEXT:  b {{.*}} <h>

#--- fn.inc
.macro fn name, sect
  .section \sect,"xr"
  .globl \name
\name:
.endm

#--- x64.s
  .include "fn.inc"
  fn main, .text$a
  movq __imp_f@FUSED(%rip), %rax
  xorl %ecx, %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  rex64 jmpq *%rax
  int3; int3; int3; int3
  fn f, .text$b
  xorl %eax, %eax
  jmpq *__imp_g(%rip)
  fn g, .text$c
  ret
## After an epilogue that pops, the unwinder needs the jump.
  .section .text$d,"xr"
  .globl framed
  .seh_proc framed
framed:
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  movq __imp_h@FUSED(%rip), %rax
  .seh_startepilogue
  popq %rbx
  .seh_endepilogue
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_h
  rex64 jmpq *%rax
  int3; int3; int3; int3
  .seh_endproc
  fn h, .text$e
  ret
  fn forwarder, .text$f
  jmpq *__imp_last(%rip)
  fn last, .text$g
  ret

## Without the dead read the padding grows and the je no longer reaches.
#--- keep.s
  .section .text$k,"xr"
  .globl main
  .p2align 4
main:
  movq __imp_f@FUSED(%rip), %rax
.Lj:
  .fill 36, 1, 0x90
  .p2align 4
  .fill 82, 1, 0x90
.Lt:
  jmpq *__imp_f(%rip)
  .section .text$l,"xr"
  .globl f
f:
  ret
## The distance fits its byte only while the read is there.
  .section .rdata,"dr"
  .byte .Lt - .Lj

#--- a64.s
  .include "fn.inc"
  fn main, .text$a
  mov w0, #1
  adrp x16, __imp_f
  ldr x16, [x16, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  br x16
  fn f, .text$b
  ret
  .section .text$c,"xr"
  .globl framed
  .seh_proc framed
framed:
  sub sp, sp, #16
  .seh_stackalloc 16
  .seh_endprologue
  adrp x16, __imp_h
  ldr x16, [x16, :fused_lo12:__imp_h]
  .seh_startepilogue
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_h
  br x16
  .seh_endproc
  fn h, .text$d
  ret
