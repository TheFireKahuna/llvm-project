# REQUIRES: x86

## Under -import-slots, __kcfi_tinflow_<type>_<called> or
## __kcfi_tinflow_n<node>_<called> says that a pointer of the type, or of each
## type of the node, can come back from a call through a pointer of the called
## type. When the called type is open dynamically, the linker opens the type
## dynamically too, and so on until nothing more opens. Here 0x11111111 is
## open dynamically by the routine of opener.obj, which wins over main.obj's
## static one, and opens 0x22222222; 0x33333333 is open dynamically by
## __kcfi_inflow_33333333_getter and opens 0x44444444, which opens
## 0x55555555, and node a1's 0x66666666; 0x77777777 is not open, and opens
## nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc opener.s \
# RUN:   -o opener.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foreign.s \
# RUN:   -o foreign.obj
# RUN: lld-link main.obj opener.obj foreign.obj -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_11111111>
# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_22222222>
# CHECK:      <__llvm_kcfi_dispatch_33333333>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_33333333>
# CHECK:      <__llvm_kcfi_dispatch_44444444>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_44444444>
# CHECK:      <__llvm_kcfi_dispatch_55555555>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_55555555>
# CHECK:      <__llvm_kcfi_dispatch_66666666>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_mismatch_66666666>
# CHECK:      <__llvm_kcfi_dispatch_77777777>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_dispatch_88888888>:
# CHECK:        jne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_mismatch_11111111>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   int3
# CHECK:      <__llvm_kcfi_mismatch_22222222>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_33333333>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_44444444>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_55555555>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_66666666>:
# CHECK-NEXT:   leaq {{.*}}(%rip), %r10
# CHECK-NEXT:   jmp 0x{{[0-9a-f]+}} <__llvm_kcfi_open_dynamic>
# CHECK-NOT:  <__llvm_kcfi_mismatch_

## The compiler's head and trailer for 0x11111111, then the linker's for each
## type it opens.
# DATA:      140002000 11111111 00000000 23222222 00000000
# DATA-NEXT: 140002010 22222222 00000000 45444444 00000000
# DATA-NEXT: 140002020 33333333 00000000 67666666 00000000
# DATA-NEXT: 140002030 44444444 00000000 89888888 00000000
# DATA-NEXT: 140002040 55555555 00000000 abaaaaaa 00000000
# DATA-NEXT: 140002050 66666666 00000000 cdcccccc 00000000
# DATA-NOT:  140002060

#--- foreign.s
## Code without a KCFI prefix, through which a pointer of 0x33333333 can come
## back.
        .text
        .globl getter
getter:
        retq

#--- opener.s
## An object that opens 0x11111111 dynamically.
        .section .text,"xr",largest,__llvm_kcfi_mismatch_11111111
        .globl __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111:
        leaq __llvm_kcfi_list_11111111+8(%rip), %r10
        jmp __llvm_kcfi_open_dynamic
        int3
        .section .rdata$llvm_kcfi_11111111_a,"dr",discard,__llvm_kcfi_list_11111111
        .globl __llvm_kcfi_list_11111111
        .p2align 3
__llvm_kcfi_list_11111111:
        .quad 0x11111111
        .section .rdata$llvm_kcfi_11111111_z,"dr",associative,__llvm_kcfi_list_11111111
        .p2align 3
        .quad 0x22222223

#--- main.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        callq getter
        .irpc d, 12345678
        callq __llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d
        .endr
        retq

        .weak __kcfi_inflow_33333333_getter
__kcfi_inflow_33333333_getter = 0x33333333
        .weak __kcfi_tinflow_22222222_11111111
__kcfi_tinflow_22222222_11111111 = 0x22222222
        .weak __kcfi_tinflow_44444444_33333333
__kcfi_tinflow_44444444_33333333 = 0x44444444
        .weak __kcfi_tinflow_55555555_44444444
__kcfi_tinflow_55555555_44444444 = 0x55555555
        .weak __kcfi_tinflow_n00000000000000a1_44444444
__kcfi_tinflow_n00000000000000a1_44444444 = 0
        .weak __kcfi_node_00000000000000a1_66666666
__kcfi_node_00000000000000a1_66666666 = 0x66666666
        .weak __kcfi_tinflow_88888888_77777777
__kcfi_tinflow_88888888_77777777 = 0x88888888

## A static routine for 0x11111111, which opener.obj's dynamic one replaces.
        .section .text,"xr",largest,__llvm_kcfi_mismatch_11111111
        .globl __llvm_kcfi_mismatch_11111111
__llvm_kcfi_mismatch_11111111:
        leaq __llvm_kcfi_list_11111111+8(%rip), %r10
        jmp __llvm_kcfi_open
        .section .rdata$llvm_kcfi_11111111_a,"dr",discard,__llvm_kcfi_list_11111111
        .globl __llvm_kcfi_list_11111111
        .p2align 3
__llvm_kcfi_list_11111111:
        .quad 0x11111111
        .section .rdata$llvm_kcfi_11111111_z,"dr",associative,__llvm_kcfi_list_11111111
        .p2align 3
        .quad 0x22222223

        .irpc d, 2345678
        .weak __llvm_kcfi_mismatch_\d\d\d\d\d\d\d\d
__llvm_kcfi_mismatch_\d\d\d\d\d\d\d\d = __llvm_kcfi_trap
        .endr

        .irpc d, 12345678
        .section .text,"xr",discard,__llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d
        .globl __llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d
        .p2align 4
__llvm_kcfi_dispatch_\d\d\d\d\d\d\d\d:
        cmpl $0x\d\d\d\d\d\d\d\d, -4(%rax)
        jne __llvm_kcfi_mismatch_\d\d\d\d\d\d\d\d
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
