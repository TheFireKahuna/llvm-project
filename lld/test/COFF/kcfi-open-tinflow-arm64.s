# REQUIRES: aarch64

## On ARM64, under -import-slots, a type that a __kcfi_tinflow_ fact names is
## opened dynamically when the called type is open dynamically: here
## 0x11111111, whose compiled check routine jumps to the dynamic scanner,
## opens node a1's 0x22222222, and 0x33333333, opened by
## __kcfi_inflow_0000000033333333_plain, opens 0x44444444. 0x55555555 is not open, and
## opens nothing.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc plain.s -o plain.obj
# RUN: lld-link main.obj plain.obj -machine:arm64 -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_check_11111111>:
# CHECK:        b.ne 0x{{[0-9a-f]+}} <__llvm_kcfi_check_mismatch_11111111>
# CHECK:      <__llvm_kcfi_check_22222222>:
# CHECK:        b.ne 0x{{[0-9a-f]+}} <__llvm_kcfi_check_mismatch_22222222>
# CHECK:      <__llvm_kcfi_check_33333333>:
# CHECK:        b.ne 0x{{[0-9a-f]+}} <__llvm_kcfi_check_mismatch_33333333>
# CHECK:      <__llvm_kcfi_check_44444444>:
# CHECK:        b.ne 0x{{[0-9a-f]+}} <__llvm_kcfi_check_mismatch_44444444>
# CHECK:      <__llvm_kcfi_check_55555555>:
# CHECK:        b.ne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# CHECK:      <__llvm_kcfi_check_66666666>:
# CHECK:        b.ne 0x{{[0-9a-f]+}} <__llvm_kcfi_trap>
# CHECK:      [[#%x,DYN:]] <__llvm_kcfi_check_open_dynamic>:
# CHECK:      <__llvm_kcfi_check_mismatch_11111111>:
# CHECK-NEXT:   adrp x16
# CHECK-NEXT:   add x16, x16
# CHECK-NEXT:   b 0x{{[0-9a-f]+}} <__llvm_kcfi_check_open_dynamic>
# CHECK-NEXT:   brk #0xf003
# CHECK:      <__llvm_kcfi_check_mismatch_22222222>:
# CHECK-NEXT:   adrp x16
# CHECK-NEXT:   add x16, x16
# CHECK-NEXT:   adrp x17, 0x[[#%x,PAGE:]]
# CHECK-NEXT:   add x17, x17, #[[#%#x,DYN - PAGE]]
# CHECK-NEXT:   br x17
# CHECK:      <__llvm_kcfi_check_mismatch_33333333>:
# CHECK-NEXT:   adrp x16
# CHECK-NEXT:   add x16, x16
# CHECK-NEXT:   adrp x17, 0x[[#%x,PAGE:]]
# CHECK-NEXT:   add x17, x17, #[[#%#x,DYN - PAGE]]
# CHECK-NEXT:   br x17
# CHECK:      <__llvm_kcfi_check_mismatch_44444444>:
# CHECK-NEXT:   adrp x16
# CHECK-NEXT:   add x16, x16
# CHECK-NEXT:   adrp x17, 0x[[#%x,PAGE:]]
# CHECK-NEXT:   add x17, x17, #[[#%#x,DYN - PAGE]]
# CHECK-NEXT:   br x17
# CHECK-NOT:  <__llvm_kcfi_check_mismatch_

## The compiler's head and trailer for 0x11111111, then the linker's for each
## type it opens.
# DATA:      140002000 11111111 00000000 23222222 00000000
# DATA-NEXT: 140002010 22222222 00000000 45444444 00000000
# DATA-NEXT: 140002020 33333333 00000000 67666666 00000000
# DATA-NEXT: 140002030 44444444 00000000 89888888 00000000
# DATA-NOT:  140002040

#--- plain.s
## Code without a KCFI prefix, through which a pointer of 0x33333333 can come
## back.
        .text
        .globl plain
        .p2align 2
plain:
        ret

#--- main.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 2
main:
        bl plain
        .irpc d, 123456
        bl __llvm_kcfi_check_\d\d\d\d\d\d\d\d
        .endr
        ret

## 0x11111111's own object publishes that it opens it dynamically.
        .weak __kcfi_popen_0000000011111111
__kcfi_popen_0000000011111111 = 0x11111111
        .weak __kcfi_inflow_0000000033333333_plain
__kcfi_inflow_0000000033333333_plain = 0x33333333
        .weak __kcfi_tinflow_n00000000000000a1_000000001111111111111111
__kcfi_tinflow_n00000000000000a1_000000001111111111111111 = 0
        .weak __kcfi_node_00000000000000a1_0000000022222222
__kcfi_node_00000000000000a1_0000000022222222 = 0x22222222
        .weak __kcfi_tinflow_0000000044444444_000000003333333333333333
__kcfi_tinflow_0000000044444444_000000003333333333333333 = 0x44444444
        .weak __kcfi_tinflow_0000000066666666_000000005555555555555555
__kcfi_tinflow_0000000066666666_000000005555555555555555 = 0x66666666

        .irpc d, 23456
        .weak __llvm_kcfi_check_mismatch_\d\d\d\d\d\d\d\d
__llvm_kcfi_check_mismatch_\d\d\d\d\d\d\d\d = __llvm_kcfi_trap
        .endr

        .irpc d, 123456
        .section .text,"xr",discard,__llvm_kcfi_check_\d\d\d\d\d\d\d\d
        .globl __llvm_kcfi_check_\d\d\d\d\d\d\d\d
        .p2align 2
__llvm_kcfi_check_\d\d\d\d\d\d\d\d:
        ldur w16, [x15, #-4]
        mov w17, #0x\d\d\d\d
        cmp w16, w17
        b.ne __llvm_kcfi_check_mismatch_\d\d\d\d\d\d\d\d
        ret
        .endr

        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 2
__llvm_kcfi_trap:
        mov w0, #64
        brk #0xf003

        .section .text,"xr",discard,__llvm_kcfi_check_open
        .globl __llvm_kcfi_check_open
        .p2align 2
__llvm_kcfi_check_open:
        mov w0, #3

        .section .text,"xr",discard,__llvm_kcfi_check_open_dynamic
        .globl __llvm_kcfi_check_open_dynamic
        .p2align 2
__llvm_kcfi_check_open_dynamic:
        mov w0, #4

        .section .text,"xr",largest,__llvm_kcfi_check_mismatch_11111111
        .globl __llvm_kcfi_check_mismatch_11111111
        .p2align 2
__llvm_kcfi_check_mismatch_11111111:
        adrp x16, __llvm_kcfi_list_11111111+8
        add x16, x16, :lo12:__llvm_kcfi_list_11111111+8
        b __llvm_kcfi_check_open_dynamic
        brk #0xf003
        .section .rdata$llvm_kcfi_11111111_a,"dr",discard,__llvm_kcfi_list_11111111
        .globl __llvm_kcfi_list_11111111
        .p2align 3
__llvm_kcfi_list_11111111:
        .quad 0x11111111
        .section .rdata$llvm_kcfi_11111111_z,"dr",associative,__llvm_kcfi_list_11111111
        .p2align 3
        .quad 0x22222223
