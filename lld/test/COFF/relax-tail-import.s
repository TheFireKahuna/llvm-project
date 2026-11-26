# REQUIRES: x86, aarch64

## A tail jump through a pointer, made direct, is deleted where its target's
## section follows; framed's (after a pop) and an address-significant
## forwarder's stay short, a jcc over one is one jcc of the opposite condition
## in a resized section (over, overmoved), and in a fixed section it stays
## direct.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{link} x64.obj -out:x64.exe
# RUN: llvm-objdump -d x64.exe | FileCheck --check-prefix=X64 %s

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
# X64-EMPTY:
# X64:       <over>:
# X64-NEXT:  testl %ecx, %ecx
# X64-NEXT:  : 74 {{..}} je {{.*}} <last>
# X64-NEXT:  retq
# X64:       <overmoved>:
# X64-NEXT:  testl %ecx, %ecx
# X64-NEXT:  : 74 {{..}} je {{.*}} <last>
# X64-NEXT:  retq

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium keep.s -o keep.obj
# RUN: %{link} keep.obj -out:keep.exe
# RUN: llvm-objdump -d keep.exe | FileCheck --check-prefix=KEEP %s

# KEEP:      <main>:
# KEEP:      : e9 {{.*}} jmp {{.*}} <f>
# KEEP-NEXT: : cc int3
# KEEP-EMPTY:
# KEEP-NEXT: <f>:

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{link} a64.obj -out:a64.exe
# RUN: llvm-objdump -d a64.exe | FileCheck --check-prefix=A64 %s

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

## The fixup of a jump made direct is the jump's own, where the jump is, also
## where an edit follows it in its section.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium fixup.s -o fixup.obj
# RUN: lld-link -entry:main -subsystem:console fixup.obj -out:fixup.exe -debug \
# RUN:   -debugtype:cv,fixup -pdb:fixup.pdb
# RUN: llvm-pdbutil dump -fixups fixup.pdb | FileCheck --check-prefix=FIXUP %s

# FIXUP: 8047 | 0000  | 00001000 | 0000100C

#--- fn.inc
.macro fn name, sect
  .section \sect,"xr"
  .globl \name
\name:
.endm

#--- x64.s
  .include "fn.inc"
  fn main, .text$a
  xorl %ecx, %ecx
  jmpq *__imp_f(%rip)
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
  jmpq *__imp_h(%rip)
  .seh_endproc
  fn h, .text$e
  ret
  fn forwarder, .text$f
  jmpq *__imp_last(%rip)
  fn last, .text$g
  ret
  .section .text$h,"xr"
  .globl over
over:
  testl %ecx, %ecx
  jne 1f
  jmpq *__imp_last(%rip)
1:
  ret
  .section .text$i,"xr"
  .globl overmoved
overmoved:
  movq __imp_last@FUSED(%rip), %rax
  testl %ecx, %ecx
  jne 1f
  jmpq *__imp_last(%rip)
1:
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

#--- fixup.s
  .section .text$a,"xr"
  .globl main
main:
  adrp x16, __imp_h
  ldr x16, [x16, :fused_lo12:__imp_h]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_h
  br x16
  adrp x0, h
  add x0, x0, :lo12:h
  ret
  .section .text$b,"xr"
  .globl h
h:
  ret
