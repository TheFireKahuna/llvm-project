# RUN: llvm-mc -triple=x86_64-pc-windows-ntposix %s | FileCheck %s
# RUN: llvm-mc -triple=x86_64-pc-windows-ntposix -filetype=obj %s -o - \
# RUN:   | llvm-objdump -dr - | FileCheck %s --check-prefix=OBJ
# RUN: llvm-mc -triple=x86_64-unknown-windows-itanium %s | FileCheck %s --check-prefix=ITANIUM
# RUN: not llvm-mc -triple=x86_64-pc-windows-ntposix --defsym WRITE=1 %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=WRITE --implicit-check-not=error:

# On NT-POSIX Linux's stack guard, %fs:0x28, is the image's __stack_chk_guard:
# a read of it, movabs's among them, is one RIP-relative load of that word,
# and a write is an error. Any other %fs: operand, and Windows Itanium, are
# untouched.

# CHECK:      movq __stack_chk_guard(%rip), %rax
# CHECK:      subq __stack_chk_guard(%rip), %rdx
# CHECK:      cmpq __stack_chk_guard(%rip), %rcx
# CHECK:      movl __stack_chk_guard(%rip), %eax
# CHECK:      movq __stack_chk_guard(%rip), %rax
# CHECK:      movq %fs:48, %rax
# CHECK:      movq %fs:40(%rbx), %rax
# CHECK:      leaq %fs:40, %rax

# OBJ-COUNT-5:  IMAGE_REL_LLVM_{{.*}} __stack_chk_guard
# OBJ-NOT:      __stack_chk_guard

# ITANIUM-NOT:  __stack_chk_guard

.ifndef WRITE
movq %fs:0x28, %rax
subq %fs:40, %rdx
cmpq %fs:0x28, %rcx
movabsl %fs:0x28, %eax
.intel_syntax noprefix
mov rax, qword ptr fs:[0x28]
.att_syntax
movq %fs:0x30, %rax
movq %fs:0x28(%rbx), %rax
leaq %fs:0x28, %rax
.else
# WRITE: error: the stack guard, %fs:0x28, is read-only on NT-POSIX
movq %rax, %fs:0x28
# WRITE: error: the stack guard, %fs:0x28, is read-only on NT-POSIX
addq $1, %fs:0x28
# WRITE: error: the stack guard, %fs:0x28, is read-only on NT-POSIX
movabsq %rax, %fs:0x28
.endif
