# REQUIRES: x86

## Under -import-slots, in an image it seals, the linker rewrites the range test
## at the start of clang's KCFI thunks: it becomes one comparison of the
## target's offset from the start of .text with its size, followed by a nop to
## clang's length. A type that no unsealed function in the image has jumps
## straight to the page test. Clang's type checks, page test and guard jump
## stay. Without -import-slots, clang's form stays.

# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: lld-link %t.obj -guard:cf -import-slots -entry:main -debug:symtab \
# RUN:   -out:%t.exe
# RUN: llvm-readobj --sections %t.exe > %t.txt
# RUN: llvm-objdump -d %t.exe >> %t.txt
# RUN: FileCheck %s < %t.txt

# CHECK:      Name: .text
# CHECK-NEXT: VirtualSize: 0x[[#%X,SIZE:]]

## listed is in the guard function table, so type 0x11111111 has an unsealed
## function and tests the range.
# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10 {{.*}}0x140001000
# CHECK-NEXT:   movq %rax, %r11
# CHECK-NEXT:   subq %r10, %r11
# CHECK-NEXT:   cmpq $0x[[#%x,SIZE]], %r11
# CHECK-NEXT:   jae {{.*}}<__llvm_kcfi_dispatch_11111111+0x2e>
# CHECK-NEXT:   nop
# CHECK-NEXT:   movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne
# CHECK-NEXT:   jmpq *%rax
# CHECK-NEXT:   testl $0xff0, %eax
# CHECK-NEXT:   je
# CHECK-NEXT:   movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne
# CHECK-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>

## sealed is only called directly, so type 0x22222222 has no unsealed function
## in the image and goes straight to the page test.
# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK-NEXT:   jmp {{.*}}<__llvm_kcfi_dispatch_22222222+0x2e>
# CHECK-COUNT-22: int3
# CHECK-NEXT:   movabsq $0x22222222b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne
# CHECK-NEXT:   jmpq *%rax
# CHECK-NEXT:   testl $0xff0, %eax
# CHECK-NEXT:   je
# CHECK-NEXT:   movabsq $0x22222222b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   jne
# CHECK-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>

## The check thunk takes the target in RCX and returns in range.
# CHECK:      <__llvm_kcfi_check_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10 {{.*}}0x140001000
# CHECK-NEXT:   movq %rcx, %r11
# CHECK-NEXT:   subq %r10, %r11
# CHECK-NEXT:   cmpq $0x[[#%x,SIZE]], %r11
# CHECK-NEXT:   jae {{.*}}<__llvm_kcfi_check_11111111+0x2d>
# CHECK-NEXT:   nop
# CHECK-NEXT:   movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rcx)
# CHECK-NEXT:   jne
# CHECK-NEXT:   retq
# CHECK-NEXT:   testl $0xff0, %ecx
# CHECK-NEXT:   je
# CHECK-NEXT:   movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rcx)
# CHECK-NEXT:   jne
# CHECK-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_check_icall_fptr>

## A thunk that differs from clang's current form in one byte or in one
## relocation's target, one of an older form, and a local thunk, which fails
## fast outside the range, are left as they are. In the sealed image the local
## thunk's bounds are .text's; in one that is not sealed both are clang's weak
## default, so its out-of-range path sees an empty range.
# CHECK:      <__llvm_kcfi_dispatch_33333333>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   cmpq %r10, %rax
# CHECK-NEXT:   jb
# CHECK:      <__llvm_kcfi_dispatch_44444444>:
# CHECK-NEXT:   cmpl $0x44444444, -0x4(%rax)
# CHECK-NEXT:   jne
# CHECK-NEXT:   jmpq *{{.*}}(%rip)
# CHECK:      <__llvm_kcfi_dispatch_55555555>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   cmpq %r10, %rax
# CHECK-NEXT:   jb
# CHECK:      <__llvm_kcfi_local_dispatch_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10 {{.*}}0x140001000
# CHECK-NEXT:   leaq {{.*}}(%rip), %r11 {{.*}}0x[[#%x,5368713216+SIZE]]
# CHECK-NEXT:   cmpq %r10, %rax

# RUN: lld-link %t.obj -guard:cf -entry:main -debug:symtab -out:%t.clang.exe
# RUN: llvm-objdump -d %t.clang.exe | FileCheck %s --check-prefix=CLANG

# CLANG:      <__llvm_kcfi_dispatch_22222222>:
# CLANG-NEXT:   leaq {{.*}}(%rip), %r10
# CLANG-NEXT:   cmpq %r10, %rax
# CLANG-NEXT:   jb
# CLANG-NEXT:   leaq {{.*}}(%rip), %r10
# CLANG-NEXT:   cmpq %r10, %rax
# CLANG-NEXT:   jae
# CLANG:      <__llvm_kcfi_local_dispatch_11111111>:
# CLANG-NEXT:   leaq {{.*}}(%rip), %r10 {{.*}}0x[[ADDR:[0-9a-f]+]]
# CLANG-NEXT:   leaq {{.*}}(%rip), %r11 {{.*}}0x[[ADDR]]

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

        .def sealed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,sealed
        .p2align 4
        .fill 4, 1, 0x90
__cfi_sealed:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl sealed
sealed:
        retq

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq sealed
        movq fp(%rip), %rax
        callq __llvm_kcfi_dispatch_11111111
        callq __llvm_kcfi_dispatch_22222222
        movq fp(%rip), %rcx
        callq __llvm_kcfi_check_11111111
        movq fp(%rip), %rax
        callq __llvm_kcfi_dispatch_33333333
        callq __llvm_kcfi_dispatch_44444444
        callq __llvm_kcfi_dispatch_55555555
        callq __llvm_kcfi_local_dispatch_11111111
        xorl %eax, %eax
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
        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_open
        .weak __llvm_kcfi_mismatch_33333333
__llvm_kcfi_mismatch_33333333 = __llvm_kcfi_open
        .weak __llvm_kcfi_mismatch_44444444
__llvm_kcfi_mismatch_44444444 = __llvm_kcfi_open
        .weak __llvm_kcfi_mismatch_55555555
__llvm_kcfi_mismatch_55555555 = __llvm_kcfi_open
        .weak __llvm_kcfi_check_mismatch_11111111
__llvm_kcfi_check_mismatch_11111111 = __llvm_kcfi_open

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

        .section .text,"xr",discard,__llvm_kcfi_dispatch_22222222
        .globl __llvm_kcfi_dispatch_22222222
        .p2align 4
__llvm_kcfi_dispatch_22222222:
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x22222222b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_22222222
        movabsq $0x22222222b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_22222222
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_check_11111111
        .globl __llvm_kcfi_check_11111111
        .p2align 4
__llvm_kcfi_check_11111111:
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rcx
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rcx
        jae 1f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rcx)
        jne __llvm_kcfi_check_mismatch_11111111
        retq
1:      testl $0xff0, %ecx
        je __llvm_kcfi_check_mismatch_11111111
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rcx)
        jne __llvm_kcfi_check_mismatch_11111111
        jmpq *__guard_check_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_dispatch_33333333
        .globl __llvm_kcfi_dispatch_33333333
        .p2align 4
__llvm_kcfi_dispatch_33333333:
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x33333333b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_33333333
        jmpq *%rcx
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_33333333
        movabsq $0x33333333b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_33333333
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_dispatch_44444444
        .globl __llvm_kcfi_dispatch_44444444
        .p2align 4
__llvm_kcfi_dispatch_44444444:
        cmpl $0x44444444, -4(%rax)
        jne __llvm_kcfi_mismatch_44444444
        jmpq *__guard_dispatch_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_dispatch_55555555
        .globl __llvm_kcfi_dispatch_55555555
        .p2align 4
__llvm_kcfi_dispatch_55555555:
        leaq __llvm_code_start(%rip), %r10
        cmpq %r10, %rax
        jb 1f
        leaq __llvm_code_end(%rip), %r10
        cmpq %r10, %rax
        jae 1f
        movabsq $0x55555555b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_55555555
        jmpq *%rax
1:      testl $0xff0, %eax
        je __llvm_kcfi_mismatch_55555555
        movabsq $0x55555555b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_55555555
        jmpq *__guard_check_icall_fptr(%rip)

        .section .text,"xr",discard,__llvm_kcfi_local_dispatch_11111111
        .globl __llvm_kcfi_local_dispatch_11111111
        .p2align 4
__llvm_kcfi_local_dispatch_11111111:
        leaq __llvm_code_start(%rip), %r10
        leaq __llvm_code_end(%rip), %r11
        cmpq %r10, %rax
        jb 1f
        cmpq %r11, %rax
        jae 1f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *%rax
1:      cmpq %r11, %r10
        jne 2f
        testl $0xff0, %eax
        je __llvm_kcfi_mismatch_11111111
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne __llvm_kcfi_mismatch_11111111
        jmpq *__guard_dispatch_icall_fptr(%rip)
2:      movl $64, %ecx
        int $0x29

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
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad 0
