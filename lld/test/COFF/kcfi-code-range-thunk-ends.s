# REQUIRES: x86

## The linker's KCFI thunk replaces clang's in its place, which may be the
## first or the last of the code section, so the bounds of the code range are
## placed once it has: they never name clang's thunk, which is no longer in the
## image.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj

## The thunk first: the range starts at it.
# RUN: echo __llvm_kcfi_dispatch_11111111 > %t.first
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -order:@%t.first -out:%t.first.exe
# RUN: llvm-objdump -t %t.first.exe | FileCheck %s --check-prefix=FIRST
# RUN: llvm-objdump -d %t.first.exe | FileCheck %s --check-prefix=FIRST-LEA
# FIRST-DAG:     0x00000000 __llvm_kcfi_dispatch_11111111
# FIRST-DAG:     (sec  1){{.*}} 0x00000000 __llvm_code_start
# FIRST-LEA:     <__llvm_kcfi_dispatch_11111111>:
# FIRST-LEA-NEXT:  leaq -0x7(%rip), %r10

## The thunk last: the range ends at the end of the linker's thunk, the end
## of .text.
# RUN: echo listed > %t.last
# RUN: echo main >> %t.last
# RUN: echo __llvm_kcfi_open >> %t.last
# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -order:@%t.last -out:%t.last.exe
# RUN: llvm-readobj --sections %t.last.exe > %t.last.txt
# RUN: llvm-objdump -t %t.last.exe >> %t.last.txt
# RUN: FileCheck %s --check-prefix=LAST < %t.last.txt
# LAST:      Name: .text
# LAST-NEXT: VirtualSize: 0x[[#%X,SIZE:]]
# LAST:      (sec  1){{.*}} 0x[[#%.8x,SIZE]] __llvm_code_end

  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def listed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,listed
        .p2align 4
        .fill 4, 1, 0x90
__cfi_listed:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl listed
listed:
        retq

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        movq fp(%rip), %rax
        callq __llvm_kcfi_dispatch_11111111
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .weak __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111 = __llvm_kcfi_open

        .section .text,"xr",discard,__llvm_kcfi_dispatch_11111111
        .globl __llvm_kcfi_dispatch_11111111
        .p2align 4
__llvm_kcfi_dispatch_11111111:
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_11111111
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        int3

        .data
fp:
        .quad listed

        .section .gfids$y,"dr"
        .symidx listed

        .section .rdata,"dr"
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0
