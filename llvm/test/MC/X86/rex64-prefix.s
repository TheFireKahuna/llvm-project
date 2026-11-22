# RUN: llvm-mc -triple x86_64 -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple x86_64 %s | llvm-mc -triple x86_64 -show-encoding | \
# RUN:   FileCheck %s
# RUN: not llvm-mc -triple x86_64 -defsym=ERR=1 %s -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=ERR

## As GNU as does, rex64 before an instruction sets REX.W in the instruction's
## own REX prefix, after any legacy prefix, rather than being a byte of its own
## that a REX prefix the instruction needs would override. A Win64 jump that
## ends an epilogue relies on it through r8 to r15.

.ifndef ERR
# CHECK: rex64 jmpq *%r11 # encoding: [0x49,0xff,0xe3]
rex64 jmpq *%r11
# CHECK: rex64 jmpq *%rax # encoding: [0x48,0xff,0xe0]
rex64 jmpq *%rax
# CHECK: rex64 callq *%r9 # encoding: [0x49,0xff,0xd1]
rex64 callq *%r9
# CHECK: rex64 jmpq *foo(%rip) # encoding: [0x48,0xff,0x25,A,A,A,A]
rex64 jmpq *foo(%rip)
# CHECK: rex64 movl %eax, %r10d # encoding: [0x49,0x89,0xc2]
rex64 movl %eax, %r10d
# CHECK: lock rex64 addl %eax, (%rcx) # encoding: [0xf0,0x48,0x01,0x01]
rex64 lock addl %eax, (%rcx)
# CHECK: rep rex64 movsb (%rsi), %es:(%rdi) # encoding: [0xf3,0x48,0xa4]
rex64 rep movsb

## On a line of its own, rex64 is a byte of its own.
# CHECK:      rex64 # encoding: [0x48]
# CHECK-NEXT: jmpq *%r11 # encoding: [0x41,0xff,0xe3]
rex64
jmpq *%r11

.intel_syntax noprefix
# CHECK: rex64 jmpq *%r10 # encoding: [0x49,0xff,0xe2]
rex64 jmp r10
.att_syntax
.else
# ERR: :[[#@LINE+1]]:1: error: 'rex64' prefix is not allowed with this instruction
rex64 vaddps %xmm0, %xmm1, %xmm2
# ERR: :[[#@LINE+1]]:1: error: can't encode 'ah' in an instruction requiring EVEX/REX2/REX prefix
rex64 movb %ah, %al
.endif
