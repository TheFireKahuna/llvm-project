# REQUIRES: x86

## Under -import-slots, __kcfi_inflow_<type>_<g> may name a variable through
## which a pointer of the type can come back. The linker opens the type
## dynamically when the variable is defined in an object that defines code
## without any KCFI prefix, or, with automatic import, when a DLL provides it.
## A variable defined in an object with a prefix, or in one without code,
## opens nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc foreign.s \
# RUN:   -o foreign.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc data.s -o data.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc kcfi.s -o kcfi.obj
# RUN: lld-link main.obj foreign.obj data.obj kcfi.obj -import-slots \
# RUN:   -entry:main -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_dispatch_22222222>:
# CHECK:        jne 0x140001085 <__llvm_kcfi_mismatch_22222222>
# CHECK:      <__llvm_kcfi_dispatch_33333333>:
# CHECK:        jne 0x140001070 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_dispatch_44444444>:
# CHECK:        jne 0x140001070 <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_mismatch_22222222>:
# CHECK-NEXT:   4c 8d 15 7c 0f 00 00 leaq 0xf7c(%rip), %r10 # 0x140002008
# CHECK-NEXT:   e9 ef ff ff ff       jmp 0x140001080 <__llvm_kcfi_open_dynamic>
# CHECK-NEXT:   cc                   int3

# DATA: 140002000 22222222 00000000 45444444 00000000

## A variable that a DLL provides, reached by automatic import.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu imp.s -o imp.obj
# RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
# RUN: lld-link -lldmingw imp.obj kcfi.obj lib.lib -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:imp.exe
# RUN: llvm-objdump -d imp.exe | FileCheck %s --check-prefix=IMP

# IMP:      <main>:
# IMP-NEXT:   movq 0x1051(%rip), %rax # 0x140002068 <.refptr.vimp>
# IMP:      <__llvm_kcfi_dispatch_11111111>:
# IMP:        jne 0x140001035 <__llvm_kcfi_mismatch_11111111>
# IMP:      <__llvm_kcfi_mismatch_11111111>:
# IMP-NEXT:   4c 8d 15 cc 0f 00 00 leaq 0xfcc(%rip), %r10 # 0x140002008
# IMP-NEXT:   e9 ef ff ff ff       jmp 0x140001030 <__llvm_kcfi_open_dynamic>
# IMP-NEXT:   cc                   int3

#--- lib.def
LIBRARY lib.dll
EXPORTS
vimp DATA

#--- foreign.s
## Code without a KCFI prefix, and a variable it defines.
        .text
        .globl foreign
foreign:
        retq
        .data
        .globl vforeign
vforeign:
        .quad 0

#--- data.s
## A variable in an object without code.
        .data
        .globl vdata
vdata:
        .quad 0

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
        movq vforeign(%rip), %rax
        movq vours(%rip), %rax
        movq vdata(%rip), %rax
        callq __llvm_kcfi_dispatch_22222222
        callq __llvm_kcfi_dispatch_33333333
        callq __llvm_kcfi_dispatch_44444444
        retq

        .data
        .globl vours
vours:
        .quad 0

        .weak __kcfi_inflow_0000000022222222_vforeign
__kcfi_inflow_0000000022222222_vforeign = 0x22222222
        .weak __kcfi_inflow_0000000033333333_vours
__kcfi_inflow_0000000033333333_vours = 0x33333333
        .weak __kcfi_inflow_0000000044444444_vdata
__kcfi_inflow_0000000044444444_vdata = 0x44444444

#--- imp.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 4
        .fill 4, 1, 0x90
__cfi_main:
        nopl 0x71c5a06(%rax)
        movl $0x12345678, %eax
main:
        movq .refptr.vimp(%rip), %rax
        callq __llvm_kcfi_dispatch_11111111
        retq

        .section .rdata$.refptr.vimp,"dr",discard,.refptr.vimp
        .globl .refptr.vimp
.refptr.vimp:
        .quad vimp

        .weak __kcfi_inflow_0000000011111111_vimp
__kcfi_inflow_0000000011111111_vimp = 0x11111111

#--- kcfi.s
## The thunks of each type, whose mismatch routines are the trap, and the
## routines that the linker's open routines use.
        .irp t, 11111111, 22222222, 33333333, 44444444
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
