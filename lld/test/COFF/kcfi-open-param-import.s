# REQUIRES: x86

## Under -import-slots, __kcfi_param_<type>_<g> may name a variable of ours
## that can hold a pointer of the type, and an object that defines code
## without any KCFI prefix may reach g, a variable or a function, through
## __imp_g as well as directly. Either reference opens the type dynamically.
## An __imp_g reference from an object with a prefix opens nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foreign.s \
# RUN:   -o foreign.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ours2.s -o ours2.obj
# RUN: lld-link main.obj foreign.obj ours2.obj -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_aaaaaaaa>:
# CHECK:        jne 0x1400010d8 <__llvm_kcfi_mismatch_aaaaaaaa>
# CHECK:      <__llvm_kcfi_dispatch_bbbbbbbb>:
# CHECK:        jne 0x1400010e5 <__llvm_kcfi_mismatch_bbbbbbbb>
# CHECK:      <__llvm_kcfi_dispatch_cccccccc>:
# CHECK:        jne 0x1400010f2 <__llvm_kcfi_mismatch_cccccccc>
# CHECK:      <__llvm_kcfi_dispatch_dddddddd>:
# CHECK:        jne 0x140001090 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_mismatch_aaaaaaaa>:
# CHECK-NEXT:   4c 8d 15 29 0f 00 00 leaq 0xf29(%rip), %r10 # 0x140002008
# CHECK-NEXT:   e9 bc ff ff ff       jmp 0x1400010a0 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3
# CHECK:      <__llvm_kcfi_mismatch_bbbbbbbb>:
# CHECK-NEXT:   4c 8d 15 2c 0f 00 00 leaq 0xf2c(%rip), %r10 # 0x140002018
# CHECK-NEXT:   e9 af ff ff ff       jmp 0x1400010a0 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3
# CHECK:      <__llvm_kcfi_mismatch_cccccccc>:
# CHECK-NEXT:   4c 8d 15 2f 0f 00 00 leaq 0xf2f(%rip), %r10 # 0x140002028
# CHECK-NEXT:   e9 a2 ff ff ff       jmp 0x1400010a0 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3

## The linker's heads and trailers, then the local import pointers.
# DATA:      140002000 aaaaaaaa 00000000 55555555 01000000
# DATA-NEXT: 140002010 bbbbbbbb 00000000 77777777 01000000
# DATA-NEXT: 140002020 cccccccc 00000000 99999999 01000000

#--- foreign.s
## Code without a KCFI prefix that refers to vg directly, and to vh and fn
## as if they were imports.
        .text
        .globl foreign
foreign:
        movq vg(%rip), %rax
        movq __imp_vh(%rip), %rax
        jmpq *__imp_fn(%rip)

#--- ours2.s
## Code of ours that refers to vk as if it were an import.
        .def ours2; .scl 2; .type 32; .endef
        .text
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours2:
        nopl 0x71c5a06(%rax)
        movl $0x12345678, %eax
        .globl ours2
ours2:
        movq __imp_vk(%rip), %rax
        retq

#--- main.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
        .fill 4, 1, 0x90
__cfi_main:
        nopl 0x71c5a06(%rax)
        movl $0x12345678, %eax
main:
        callq foreign
        callq ours2
        callq __llvm_kcfi_dispatch_aaaaaaaa
        callq __llvm_kcfi_dispatch_bbbbbbbb
        callq __llvm_kcfi_dispatch_cccccccc
        callq __llvm_kcfi_dispatch_dddddddd
        retq

        .def fn; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,fn
        .p2align 4
        .fill 4, 1, 0x90
__cfi_fn:
        nopl 0x71c5a06(%rax)
        movl $0x0000000a, %eax
        .globl fn
fn:
        retq

## vg, vh and vk are ours, each able to hold a pointer of one of the types,
## and fn is ours, with a parameter of another.
        .data
        .globl vg
vg:
        .quad 0
        .globl vh
vh:
        .quad 0
        .globl vk
vk:
        .quad 0

        .weak __kcfi_param_aaaaaaaa_vg
__kcfi_param_aaaaaaaa_vg = 0xaaaaaaaa
        .weak __kcfi_param_bbbbbbbb_vh
__kcfi_param_bbbbbbbb_vh = 0xbbbbbbbb
        .weak __kcfi_param_cccccccc_fn
__kcfi_param_cccccccc_fn = 0xcccccccc
        .weak __kcfi_param_dddddddd_vk
__kcfi_param_dddddddd_vk = 0xdddddddd

        .irp t, aaaaaaaa, bbbbbbbb, cccccccc, dddddddd
        .weak __llvm_kcfi_mismatch_\t
__llvm_kcfi_mismatch_\t = __llvm_kcfi_trap
        .section .text,"xr",discard,__llvm_kcfi_dispatch_\t
        .globl __llvm_kcfi_dispatch_\t
        .p2align 4
__llvm_kcfi_dispatch_\t:
        cmpl $0x\t, -4(%rax)
        jne __llvm_kcfi_mismatch_\t
        jmpq *%rax
        .endr

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        movl $64, %ecx
        int $0x29

        .section .text,"xr",discard,__llvm_kcfi_open
        .globl __llvm_kcfi_open
        .p2align 4
__llvm_kcfi_open:
        movl $1, %eax

        .section .text,"xr",discard,__llvm_kcfi_open_dynamic
        .globl __llvm_kcfi_open_dynamic
        .p2align 4
__llvm_kcfi_open_dynamic:
        movl $2, %eax
