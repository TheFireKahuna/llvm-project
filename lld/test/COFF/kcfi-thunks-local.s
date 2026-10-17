# REQUIRES: x86

## A local KCFI thunk serves a type whose every target is in the image. In an
## image the linker seals, it replaces the local thunks that the records
## describe with its own form. Where the image has a code range, a target
## outside it fails fast; a target inside it takes the type check only if a
## function of the type is in the range, and otherwise goes straight to the
## mismatch routine, since every other function there has a sealed prefix.

## Each thunk's mismatch routine, where it is the type's own, is placed right
## after the thunk, whose branches to it then take an 8-bit displacement.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple x86_64-windows-itanium main.s -filetype=obj -o main.obj
# RUN: llvm-mc -triple x86_64-windows-itanium main.s -filetype=obj \
# RUN:   --defsym NOREC=1 -o main.norec.obj
# RUN: llvm-mc -triple x86_64-windows-itanium other.s -filetype=obj -o other.obj
# RUN: lld-link main.obj -guard:cf -entry:main -debug:symtab -out:main.exe
# RUN: llvm-readobj --sections main.exe > main.txt
# RUN: llvm-objdump -d main.exe >> main.txt
# RUN: FileCheck %s < main.txt

# CHECK:      Name: .text
# CHECK-NEXT: VirtualSize: 0x[[#%X,SIZE:]]

## listed is in the guard function table, so type 0x11111111 has a function in
## the range: a target inside it takes the type check and the jump.
# CHECK:      <__llvm_kcfi_local_dispatch_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10 {{.*}}0x[[#%x,5368713215+SIZE]]
# CHECK-NEXT:   subq %rax, %r10
# CHECK-NEXT:   cmpq $0x[[#%x,SIZE]], %r10
# CHECK-NEXT:   jae 0x[[FAIL:[0-9a-f]+]]
# CHECK-NEXT:   movabsq $0x11111111b8071c5a, %r11
# CHECK-NEXT:   cmpq %r11, -0x8(%rax)
# CHECK-NEXT:   75 {{[0-9a-f]+}} jne 0x[[ROUTINE:[0-9a-f]+]] <__llvm_kcfi_mismatch_11111111>
# CHECK-NEXT:   jmpq *%rax
# CHECK-NEXT:   [[FAIL]]: {{.*}}movl $0x40, %ecx
# CHECK-NEXT:   int $0x29
# CHECK-EMPTY:
# CHECK-NEXT: [[ROUTINE]] <__llvm_kcfi_mismatch_11111111>:

## sealed is only called directly, so a target of type 0x22222222 inside the
## range goes to the mismatch routine, here the trap, which stays where it is.
# CHECK:      <__llvm_kcfi_local_dispatch_22222222>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   subq %rax, %r10
# CHECK-NEXT:   cmpq $0x[[#%x,SIZE]], %r10
# CHECK-NEXT:   0f 82 {{.*}} jb {{.*}} <__llvm_kcfi_trap>
# CHECK-NEXT:   movl $0x40, %ecx
# CHECK-NEXT:   int $0x29

## Prefixes in two output sections leave the image without a range, so a local
## thunk takes the page test, the type check and the guard function.
# RUN: lld-link main.obj other.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:two.exe
# RUN: llvm-objdump -d two.exe | FileCheck %s --check-prefix=NORANGE
# NORANGE:      <__llvm_kcfi_local_dispatch_11111111>:
# NORANGE-NEXT:   testl $0xff0, %eax
# NORANGE-NEXT:   74 {{[0-9a-f]+}} je {{.*}} <__llvm_kcfi_mismatch_11111111>
# NORANGE-NEXT:   movabsq $0x11111111b8071c5a, %r11
# NORANGE-NEXT:   cmpq %r11, -0x8(%rax)
# NORANGE-NEXT:   jne {{.*}} <__llvm_kcfi_mismatch_11111111>
# NORANGE-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>
# NORANGE-EMPTY:
# NORANGE-NEXT: <__llvm_kcfi_mismatch_11111111>:

## In an image the linker does not seal, clang's thunk stays. Its mismatch
## routine, placed after it, takes the place of the jump that ends the thunk.
# RUN: lld-link main.norec.obj -guard:cf -entry:main -debug:symtab \
# RUN:   -out:norec.exe
# RUN: llvm-objdump -d norec.exe | FileCheck %s --check-prefix=CLANG
# CLANG:      <__llvm_kcfi_local_dispatch_11111111>:
# CLANG:        jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>
# CLANG-NEXT:   movl $0x40, %ecx
# CLANG-NEXT:   int $0x29
# CLANG-EMPTY:
# CLANG-NEXT: <__llvm_kcfi_mismatch_11111111>:

#--- main.s
.ifndef NOREC
  .linktypeprefixes
.endif
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

## The routine of type 0x11111111 comes before the code that calls its thunk.
        .def __llvm_kcfi_mismatch_11111111; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_mismatch_11111111
        .globl __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111:
        leaq list(%rip), %r10
        jmp scan

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        callq sealed
        movq fp(%rip), %rax
        callq __llvm_kcfi_local_dispatch_11111111
        callq __llvm_kcfi_local_dispatch_22222222
        xorl %eax, %eax
        retq

        .def scan; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,scan
        .p2align 4
        .globl scan
scan:
        int3

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .weak __llvm_kcfi_mismatch_22222222
__llvm_kcfi_mismatch_22222222 = __llvm_kcfi_trap

        .def __llvm_kcfi_local_dispatch_11111111; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_local_dispatch_11111111
        .globl __llvm_kcfi_local_dispatch_11111111
        .p2align 4
__llvm_kcfi_local_dispatch_11111111:
        .linkkcfithunk __llvm_kcfi_local_dispatch_11111111, local_dispatch, 0x11111111, 0x071c5a06, 0, __llvm_kcfi_mismatch_11111111
        leaq __llvm_code_start(%rip), %r10
        leaq __llvm_code_end(%rip), %r11
        cmpq %r10, %rax
        jb 1f
        cmpq %r11, %rax
        jae 1f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne 3f
        jmpq *%rax
1:      cmpq %r11, %r10
        jne 2f
        testl $0xff0, %eax
        je 3f
        movabsq $0x11111111b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne 3f
        jmpq *__guard_dispatch_icall_fptr(%rip)
2:      movl $64, %ecx
        int $0x29
3:      .linktailjump
        jmp __llvm_kcfi_mismatch_11111111

        .def __llvm_kcfi_local_dispatch_22222222; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_local_dispatch_22222222
        .globl __llvm_kcfi_local_dispatch_22222222
        .p2align 4
__llvm_kcfi_local_dispatch_22222222:
        .linkkcfithunk __llvm_kcfi_local_dispatch_22222222, local_dispatch, 0x22222222, 0x071c5a06, 0, __llvm_kcfi_mismatch_22222222
        leaq __llvm_code_start(%rip), %r10
        leaq __llvm_code_end(%rip), %r11
        cmpq %r10, %rax
        jb 1f
        cmpq %r11, %rax
        jae 1f
        movabsq $0x22222222b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne 3f
        jmpq *%rax
1:      cmpq %r11, %r10
        jne 2f
        testl $0xff0, %eax
        je 3f
        movabsq $0x22222222b8071c5a, %r11
        cmpq %r11, -8(%rax)
        jne 3f
        jmpq *__guard_dispatch_icall_fptr(%rip)
2:      movl $64, %ecx
        int $0x29
3:      .linktailjump
        jmp __llvm_kcfi_mismatch_22222222

        .def __llvm_kcfi_trap; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .data
fp:
        .quad listed

        .section .rdata,"dr"
list:
        .quad 0

        .section .gfids$y,"dr"
        .symidx listed

        .section .rdata,"dr"
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0

#--- other.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def g; .scl 2; .type 32; .endef
        .section .xtext,"xr",one_only,g
        .p2align 4
        .fill 4, 1, 0x90
__cfi_g:
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl g
g:
        retq

        .section .xtext,"xr"
        .globl g_user
g_user:
        jmp g
