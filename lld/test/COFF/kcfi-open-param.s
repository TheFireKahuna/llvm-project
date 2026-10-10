# REQUIRES: x86

## When __kcfi_param_<type>_<g> names a KCFI type that
## g, one of our definitions, can receive, and an object that defines code
## without any KCFI prefix references g, the linker opens the type
## dynamically. A reference from an object with a prefix, or from one without
## code, opens nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foreign.s \
# RUN:   -o foreign.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc ours2.s -o ours2.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc data.s -o data.obj
# RUN: lld-link main.obj foreign.obj ours2.obj data.obj \
# RUN:   -entry:main -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_77777777>:
# CHECK:        jne 0x1400010f8 <__llvm_kcfi_mismatch_77777777>
# CHECK:      <__llvm_kcfi_dispatch_88888888>:
# CHECK:        jne 0x1400010c0 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_dispatch_99999999>:
# CHECK:        jne 0x1400010c0 <__llvm_kcfi_trap>
# CHECK:      <ours2>:
# CHECK:      <__llvm_kcfi_mismatch_77777777>:
# CHECK-NEXT:   4c 8d 15 09 0f 00 00 leaq 0xf09(%rip), %r10 # 0x140002008
# CHECK-NEXT:   e9 cc ff ff ff       jmp 0x1400010d0 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3

# DATA: 140002000 77777777 00000000 efeeeeee 00000000

#--- foreign.s
## Code without a KCFI prefix that hands g a callback.
        .text
        .globl foreign
foreign:
        jmp g

#--- ours2.s
## Code of ours that hands h a callback.
  .linktypeprefixes
        .def ours2; .scl 2; .type 32; .endef
        .text
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours2:
        nopl 0x71c5a06(%rax)
        movl $0x12345678, %eax
        .globl ours2
ours2:
        jmp h

#--- data.s
## Data without code that refers to k.
        .data
        .globl data
data:
        .quad k

#--- main.s
  .linktypeprefixes
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        callq foreign
        callq ours2
        movq data(%rip), %rax
        callq __llvm_kcfi_dispatch_77777777
        callq __llvm_kcfi_dispatch_88888888
        callq __llvm_kcfi_dispatch_99999999
        retq

## g, h and k are ours, each with a parameter of one of the types.
        .def g; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,g
        .p2align 4
        .fill 4, 1, 0x90
__cfi_g:
        nopl 0x71c5a06(%rax)
        movl $0x0000000a, %eax
        .globl g
g:
        retq

        .def h; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,h
        .p2align 4
        .fill 4, 1, 0x90
__cfi_h:
        nopl 0x71c5a06(%rax)
        movl $0x0000000b, %eax
        .globl h
h:
        retq

        .def k; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,k
        .p2align 4
        .fill 4, 1, 0x90
__cfi_k:
        nopl 0x71c5a06(%rax)
        movl $0x0000000c, %eax
        .globl k
k:
        retq

        .weak __kcfi_param_0000000077777777_g
__kcfi_param_0000000077777777_g = 0x77777777
        .weak __kcfi_param_0000000088888888_h
__kcfi_param_0000000088888888_h = 0x88888888
        .weak __kcfi_param_0000000099999999_k
__kcfi_param_0000000099999999_k = 0x99999999

        .irp t, 77777777, 88888888, 99999999
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
