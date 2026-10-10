# REQUIRES: x86

## An inflow or parameter fact may name a node in place
## of a type, __kcfi_inflow_n<node>_<g> or __kcfi_param_n<node>_<g>, and
## __kcfi_node_<node>_<type> puts a type in the node. Such a fact stands for
## the facts of each type of the node, which objects may give more than once.
## A node with no type names nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc other.s -o other.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foreign.s \
# RUN:   -o foreign.obj
# RUN: lld-link main.obj other.obj foreign.obj -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_11111111>:
# CHECK:        jne 0x140001109 <__llvm_kcfi_mismatch_11111111>
# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK:        jne 0x140001116 <__llvm_kcfi_mismatch_22222222>
# CHECK:      <__llvm_kcfi_dispatch_33333333>:
# CHECK:        jne 0x1400010d0 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_dispatch_55555555>:
# CHECK:        jne 0x140001123 <__llvm_kcfi_mismatch_55555555>
# CHECK:      <__llvm_kcfi_dispatch_66666666>:
# CHECK:        jne 0x140001130 <__llvm_kcfi_mismatch_66666666>
# CHECK:      <__llvm_kcfi_mismatch_11111111>:
# CHECK-NEXT:   4c 8d 15 f8 0e 00 00 leaq 0xef8(%rip), %r10 # 0x140002008
# CHECK-NEXT:   e9 cb ff ff ff       jmp 0x1400010e0 <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_22222222>:
# CHECK-NEXT:   4c 8d 15 fb 0e 00 00 leaq 0xefb(%rip), %r10 # 0x140002018
# CHECK-NEXT:   e9 be ff ff ff       jmp 0x1400010e0 <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_55555555>:
# CHECK-NEXT:   4c 8d 15 fe 0e 00 00 leaq 0xefe(%rip), %r10 # 0x140002028
# CHECK-NEXT:   e9 b1 ff ff ff       jmp 0x1400010e0 <__llvm_kcfi_open_dynamic>
# CHECK:      <__llvm_kcfi_mismatch_66666666>:
# CHECK-NEXT:   4c 8d 15 01 0f 00 00 leaq 0xf01(%rip), %r10 # 0x140002038
# CHECK-NEXT:   e9 a4 ff ff ff       jmp 0x1400010e0 <__llvm_kcfi_open_dynamic>

# DATA:      140002000 11111111 00000000 23222222 00000000
# DATA-NEXT: 140002010 22222222 00000000 45444444 00000000
# DATA-NEXT: 140002020 55555555 00000000 abaaaaaa 00000000
# DATA-NEXT: 140002030 66666666 00000000 cdcccccc 00000000

#--- foreign.s
## Code without a KCFI prefix, which g can hand a callback.
        .text
        .globl getter
getter:
        jmp g

#--- other.s
## Code of ours that gives node b1 and part of node a1 again.
  .linktypeprefixes
        .def other; .scl 2; .type 32; .endef
        .text
        .p2align 4
        .fill 4, 1, 0x90
__cfi_other:
        nopl 0x71c5a06(%rax)
        movl $0x12345678, %eax
        .globl other
other:
        retq

        .weak __kcfi_node_00000000000000a1_0000000011111111
__kcfi_node_00000000000000a1_0000000011111111 = 0x11111111
        .weak __kcfi_node_00000000000000b1_0000000055555555
__kcfi_node_00000000000000b1_0000000055555555 = 0x55555555
        .weak __kcfi_node_00000000000000b1_0000000066666666
__kcfi_node_00000000000000b1_0000000066666666 = 0x66666666

#--- main.s
  .linktypeprefixes
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
        .fill 4, 1, 0x90
__cfi_main:
        nopl 0x71c5a06(%rax)
        movl $0x12345678, %eax
main:
        callq getter
        callq other
        callq ours
        .irp t, 11111111, 22222222, 33333333, 55555555, 66666666
        callq __llvm_kcfi_dispatch_\t
        .endr
        retq

        .def ours; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,ours
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours:
        nopl 0x71c5a06(%rax)
        movl $0x0000000a, %eax
        .globl ours
ours:
        retq

        .def g; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,g
        .p2align 4
        .fill 4, 1, 0x90
__cfi_g:
        nopl 0x71c5a06(%rax)
        movl $0x0000000b, %eax
        .globl g
g:
        retq

## getter is foreign and ours is not; b1 is given only by other.obj, and c1
## by no object.
        .weak __kcfi_inflow_n00000000000000a1_getter
__kcfi_inflow_n00000000000000a1_getter = 0
        .weak __kcfi_inflow_n00000000000000a2_ours
__kcfi_inflow_n00000000000000a2_ours = 0
        .weak __kcfi_inflow_n00000000000000c1_getter
__kcfi_inflow_n00000000000000c1_getter = 0
        .weak __kcfi_param_n00000000000000b1_g
__kcfi_param_n00000000000000b1_g = 0

        .weak __kcfi_node_00000000000000a1_0000000011111111
__kcfi_node_00000000000000a1_0000000011111111 = 0x11111111
        .weak __kcfi_node_00000000000000a1_0000000022222222
__kcfi_node_00000000000000a1_0000000022222222 = 0x22222222
        .weak __kcfi_node_00000000000000a2_0000000033333333
__kcfi_node_00000000000000a2_0000000033333333 = 0x33333333

        .irp t, 11111111, 22222222, 33333333, 55555555, 66666666
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
