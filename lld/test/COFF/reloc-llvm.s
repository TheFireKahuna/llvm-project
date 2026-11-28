# REQUIRES: x86, aarch64

## The linker applies each type from 0x8000 up that the assembler writes with
## its assembled meaning: a branch, a site, a call-only field or a fused read
## as the field of its instruction, a span as A - B of its width, and on
## AArch64 an absolute value's immediate. ALIGN, a fused call, a fused jump and
## a fused test write nothing, and a branch out of range takes a thunk.

# RUN: llvm-mc -triple=x86_64-pc-windows-ntposix -mattr=+egpr -filetype=obj \
# RUN:   --defsym=X64=1 %s -o %t.x.obj
# RUN: lld-link /entry:main /out:%t.x.exe %t.x.obj
# RUN: llvm-objdump -d --no-show-raw-insn -s -j .text -j .data %t.x.exe | \
# RUN:   FileCheck %s --check-prefix=X64
# RUN: llvm-mc -triple=aarch64-pc-windows-ntposix -filetype=obj %s -o %t.a.obj
# RUN: llvm-mc -triple=aarch64-pc-windows-ntposix -filetype=obj \
# RUN:   --defsym=ABS=1 %s -o %t.abs.obj
# RUN: lld-link /entry:main /out:%t.a.exe %t.a.obj %t.abs.obj
# RUN: llvm-objdump -d --no-show-raw-insn -s -j .text -j .data %t.a.exe | \
# RUN:   FileCheck %s --check-prefix=A64

## x86-64: the linker shrinks the jumps, which moves g to 0x61, and keeps .Lb
## aligned at ALIGN's padding.
# X64:      Contents of section .data:
# X64-NEXT: 140003000 61100000 5de0ffff 61610061 00000061
# X64-NEXT: 140003010 00000000 00000061 9fffffff ffffffff
# X64-NEXT: 140003020 ff7f0000
# X64:      <.text>:
# X64-NEXT: 1000: callq 0x140001061
# X64-NEXT: jmp 0x140001061
# X64-NEXT: jne 0x140001061
# X64-NEXT: callq *{{.*}} # 0x140002000
# X64-NEXT: jmpq *{{.*}} # 0x140002000
# X64-NEXT: jmpq *{{.*}} # 0x140002000
# X64-NEXT: movq {{.*}}, %rax # 0x140002000
# X64-NEXT: movq {{.*}}, %r16 # 0x140002000
# X64-NEXT: leaq {{.*}} # 0x140001061
# X64-NEXT: leaq {{.*}} # 0x140001069
# X64-NEXT: movq {{.*}}, %rax # 0x140002000
# X64-NEXT: callq *%rax
# X64-NEXT: movq {{.*}}, %rax # 0x140002000
# X64-NEXT: jmpq *%rax
# X64-COUNT-4: int3
# X64-NEXT: 1054: jmp 0x140001060
# X64-NEXT: 1056: nopw %cs:(%rax,%rax)
# X64-NEXT: 1060: retq
# X64-NEXT: 1051: retq
# X64-NEXT: leal {{.*}} # 0x140002000

# A64:      Contents of section .data:
# A64-NEXT: 140003000 30100000 2ce0ffff 30300030 00000030
# A64-NEXT: 140003010 00000000 0000000c 0c000000
# A64:      <.text>:
# A64-NEXT: b.ne 0x140001030
# A64-NEXT: adrp x8, 0x140002000
# A64-NEXT: ldr x8, [x8]
# A64-NEXT: blr x8
# A64-NEXT: mov w0, #0x1235
# A64-NEXT: movk w0, #0x5678
# A64-NEXT: mov w0, #0x12340000
# A64-NEXT: and x0, x1, #0xff0
# A64-NEXT: add x3, x3, #0x34
# A64-NEXT: ldr x4, [x3, #0x38]
# A64-NEXT: nop
# A64-NEXT: nop
# A64-NEXT: 1030: b 0x140001038
# A64-NEXT: ret
# A64-NEXT: 1038: adrp x16, 0x148004000

.ifdef ABS
  .globl abs16, abs32, mask
  abs16 = 0x1234
  abs32 = 0x12345678
  mask = 0xff0
.else
.ifdef X64
  .text
  .globl main
main:
.Lm:
  callq g
  jmp g
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_g
  jne g
  callq *__imp_g(%rip)
  jmpq *__imp_g(%rip)
  rex64 jmpq *__imp_g(%rip)
  movq __imp_g(%rip), %rax
  movq __imp_g(%rip), %r16
  leaq g(%rip), %rax
  leaq g+8(%rip), %rax
  movq __imp_g@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_g
  cs; cs; cs; cs
  callq *%rax
  movq __imp_g@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_g
  rex64 jmpq *%rax
  int3; int3; int3; int3
  jmp .Lb
  .p2align 4
.Lb:
  retq
g:
  retq
  leal __imp_g(%rip), %eax

  .data
  .long g@IMGREL_CALL
  .long g@REL32_CALL
  .byte g - .Lm
  .short g - .Lm
  .long g - .Lm
  .quad g - .Lm
  .uleb128 g - .Lm
  .sleb128 .Lm - g
.else
  .text
  .globl main
main:
.Lm:
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_g
  b.ne g
  adrp x8, __imp_g
  ldr x8, [x8, :fused_lo12:__imp_g]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_g
  blr x8
  movz w0, #:abs_g0:abs16+1
  movk w0, #:abs_g0_nc:abs32
  movz w0, #:abs_g1:abs32
  and x0, x1, #:abs_logimm:mask
  add x3, x3, #(g - .Lm + 4)
  ldr x4, [x3, #(g - .Lm + 8)]
  .p2align 4
g:
  b far
  ret

  .data
  .long g@IMGREL_CALL
  .long g@REL32_CALL
  .byte g - .Lm
  .short g - .Lm
  .long g - .Lm
  .quad g - .Lm
  .byte (g - .Lm) >> 2
  .hword (g - .Lm) >> 2

  .section .far,"bw"
  .zero 0x8000000
far:
  .zero 4
.endif
.endif

.ifndef ABS
  .section .rdata,"dr"
  .globl __imp_g
__imp_g:
  .quad g
.endif
