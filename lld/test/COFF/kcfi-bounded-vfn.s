# REQUIRES: x86

## Under -import-slots and -guard:cf, in an image whose guard function table
## lists a function without a KCFI prefix that a foreign object lists, here
## plain, a check through a member function pointer, whose thunk compares the
## second type before the marker, reaches the bounded check routine of its
## closed type: it sends a target in RCX outside the image to the trap, and
## points R10 at an empty list and jumps to the dynamic check scanner with any
## other.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
# RUN: lld-link main.obj plain.obj -import-slots -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s

# CHECK:      <__llvm_kcfi_vfn_check_22222222>:
# CHECK:        jne 0x140001041 <__llvm_kcfi_check_mismatch_22222222>
# CHECK:      <__llvm_kcfi_check_mismatch_22222222>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r11 # 0x140000000
# CHECK-NEXT:   cmpq %r11, %rcx
# CHECK-NEXT:   jb 0x140001030 <__llvm_kcfi_trap>
# CHECK-NEXT:   leaq {{.*}}(%rip), %r11
# CHECK-NEXT:   cmpq %r11, %rcx
# CHECK-NEXT:   jae 0x140001030 <__llvm_kcfi_trap>
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp {{.*}} <__llvm_kcfi_check_open_dynamic>

## Without plain, the check keeps the trap.
# RUN: lld-link main.obj -import-slots -guard:cf -entry:main -debug:symtab \
# RUN:   -opt:ref -out:prefixed.exe
# RUN: llvm-objdump -d prefixed.exe | FileCheck %s --check-prefix=TRAP

# TRAP:      <__llvm_kcfi_vfn_check_22222222>:
# TRAP:        jne {{.*}} <__llvm_kcfi_trap>
# TRAP-NOT:  <__llvm_kcfi_check_open_dynamic>:

#--- plain.s
        .def plain; .scl 2; .type 32; .endef
        .text
        .globl plain
plain:
        retq

        .data
        .quad plain

#--- main.s
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .fill 4, 1, 0x90
__cfi_main:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl main
main:
        callq __llvm_kcfi_vfn_check_22222222
        retq

        .weak __llvm_kcfi_check_mismatch_22222222
__llvm_kcfi_check_mismatch_22222222 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_vfn_check_22222222
        .globl __llvm_kcfi_vfn_check_22222222
        .p2align 4
__llvm_kcfi_vfn_check_22222222:
        cmpl $0x22222222, -16(%rcx)
        jne __llvm_kcfi_check_mismatch_22222222
        retq

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .section .text,"xr",discard,__llvm_kcfi_check_open
        .globl __llvm_kcfi_check_open
        .p2align 4
__llvm_kcfi_check_open:
        int3

        .section .text,"xr",discard,__llvm_kcfi_check_open_dynamic
        .globl __llvm_kcfi_check_open_dynamic
        .p2align 4
__llvm_kcfi_check_open_dynamic:
        ud2
