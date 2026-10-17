# REQUIRES: x86

## When __kcfi_inflow_<type>_<g> names a KCFI type and g
## resolves to an import or to a definition without a KCFI prefix, the linker
## opens the type dynamically: each of its mismatch routines that is still the
## trap, of either kind, becomes one that points R10 at the type's list and
## jumps to the dynamic scanner of its kind, followed by int3, with a head and
## a trailer when the type has no list. A compiled routine of a statically
## open type is replaced the same way, with the compiled list. A g with a
## prefix opens nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc getter2.s \
# RUN:   -o getter2.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link main.obj getter2.obj lib.lib -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -d main.exe | FileCheck %s --check-prefix=GC
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_44444444>:
# CHECK:        jne 0x1400010b9 <__llvm_kcfi_mismatch_44444444>
# CHECK:      <__llvm_kcfi_check_44444444>:
# CHECK:        jne 0x1400010c6 <__llvm_kcfi_check_mismatch_44444444>
# CHECK:      <__llvm_kcfi_dispatch_55555555>:
# CHECK:        jne 0x1400010d3 <__llvm_kcfi_mismatch_55555555>
# CHECK:      <__llvm_kcfi_dispatch_66666666>:
# CHECK:        jne 0x140001090 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_open_dynamic>:
# CHECK:      <__llvm_kcfi_check_open_dynamic>:
# CHECK:      <getter2>:
# CHECK:      <__llvm_kcfi_mismatch_44444444>:
# CHECK-NEXT:   4c 8d 15 48 0f 00 00 leaq 0xf48(%rip), %r10 # 0x140002008
# CHECK-NEXT:   e9 db ff ff ff       jmp 0x1400010a0 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3
# CHECK:      <__llvm_kcfi_check_mismatch_44444444>:
# CHECK-NEXT:   4c 8d 15 3b 0f 00 00 leaq 0xf3b(%rip), %r10 # 0x140002008
# CHECK-NEXT:   e9 de ff ff ff jmp 0x1400010b0 <__llvm_kcfi_check_open_dynamic>
# CHECK-NEXT:   cc                   int3
# CHECK:      <__llvm_kcfi_mismatch_55555555>:
# CHECK-NEXT:   4c 8d 15 3e 0f 00 00 leaq 0xf3e(%rip), %r10 # 0x140002018
# CHECK-NEXT:   e9 c1 ff ff ff       jmp 0x1400010a0 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3

## Once the compiled routine is replaced, nothing keeps the static scanners.
# GC-NOT: <__llvm_kcfi_open>:
# GC-NOT: <__llvm_kcfi_check_open>:

## The linker's head and trailer for 0x44444444, and the compiler's for
## 0x55555555.
# DATA:      140002000 44444444 00000000 89888888 00000000
# DATA-NEXT: 140002010 55555555 00000000 abaaaaaa 00000000

#--- lib.def
LIBRARY lib.dll
EXPORTS
getter

#--- getter2.s
        .text
        .globl getter2
getter2:
        retq

#--- main.s
  .linktypeprefixes
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
main:
        callq getter
        callq getter2
        callq ours
        callq __llvm_kcfi_dispatch_44444444
        callq __llvm_kcfi_check_44444444
        callq __llvm_kcfi_dispatch_55555555
        callq __llvm_kcfi_dispatch_66666666
        retq

        .def ours; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,ours
        .p2align 4
        .fill 4, 1, 0x90
__cfi_ours:
        nopl 0x71c5a06(%rax)
        movl $0x77777777, %eax
        .globl ours
ours:
        retq

        .weak __kcfi_inflow_0000000044444444_getter
__kcfi_inflow_0000000044444444_getter = 0x44444444
        .weak __kcfi_inflow_0000000055555555_getter2
__kcfi_inflow_0000000055555555_getter2 = 0x55555555
        .weak __kcfi_inflow_0000000066666666_ours
__kcfi_inflow_0000000066666666_ours = 0x66666666

        .weak __llvm_kcfi_mismatch_44444444
__llvm_kcfi_mismatch_44444444 = __llvm_kcfi_trap
        .weak __llvm_kcfi_check_mismatch_44444444
__llvm_kcfi_check_mismatch_44444444 = __llvm_kcfi_trap
        .weak __llvm_kcfi_mismatch_66666666
__llvm_kcfi_mismatch_66666666 = __llvm_kcfi_trap

        .section .text,"xr",largest,__llvm_kcfi_mismatch_55555555
        .globl __llvm_kcfi_mismatch_55555555
__llvm_kcfi_mismatch_55555555:
        leaq __llvm_kcfi_list_55555555+8(%rip), %r10
        jmp __llvm_kcfi_open
        .section .rdata$llvm_kcfi_55555555_a,"dr",discard,__llvm_kcfi_list_55555555
        .globl __llvm_kcfi_list_55555555
        .p2align 3
__llvm_kcfi_list_55555555:
        .quad 0x55555555
        .section .rdata$llvm_kcfi_55555555_z,"dr",associative,__llvm_kcfi_list_55555555
        .p2align 3
        .quad 0xaaaaaaab

        .section .text,"xr",discard,__llvm_kcfi_dispatch_44444444
        .globl __llvm_kcfi_dispatch_44444444
        .p2align 4
__llvm_kcfi_dispatch_44444444:
        cmpl $0x44444444, -4(%rax)
        jne __llvm_kcfi_mismatch_44444444
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_check_44444444
        .globl __llvm_kcfi_check_44444444
        .p2align 4
__llvm_kcfi_check_44444444:
        cmpl $0x44444444, -4(%rcx)
        jne __llvm_kcfi_check_mismatch_44444444
        retq

        .section .text,"xr",discard,__llvm_kcfi_dispatch_55555555
        .globl __llvm_kcfi_dispatch_55555555
        .p2align 4
__llvm_kcfi_dispatch_55555555:
        cmpl $0x55555555, -4(%rax)
        jne __llvm_kcfi_mismatch_55555555
        jmpq *%rax

        .section .text,"xr",discard,__llvm_kcfi_dispatch_66666666
        .globl __llvm_kcfi_dispatch_66666666
        .p2align 4
__llvm_kcfi_dispatch_66666666:
        cmpl $0x66666666, -4(%rax)
        jne __llvm_kcfi_mismatch_66666666
        jmpq *%rax

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

        .section .text,"xr",discard,__llvm_kcfi_check_open
        .globl __llvm_kcfi_check_open
        .p2align 4
__llvm_kcfi_check_open:
        movl $3, %eax

        .section .text,"xr",discard,__llvm_kcfi_check_open_dynamic
        .globl __llvm_kcfi_check_open_dynamic
        .p2align 4
__llvm_kcfi_check_open_dynamic:
        movl $4, %eax
