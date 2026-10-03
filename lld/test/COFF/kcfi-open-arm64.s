# REQUIRES: aarch64

## On ARM64, under -import-slots, a KCFI type the linker opens gets a check
## mismatch routine that points X16 at the type's list and branches through
## X17 to the check scanner, which it reaches at any distance: the static
## scanner for a type opened statically, here by __kcfi_typeid_plain, and the
## dynamic one for a type opened dynamically, here by
## __kcfi_inflow_0000000022222222_plain2.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc plain.s -o plain.obj
# RUN: lld-link main.obj plain.obj -machine:arm64 -import-slots -entry:main \
# RUN:   -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_check_11111111>:
# CHECK:        b.ne 0x140001048 <__llvm_kcfi_check_mismatch_11111111>
# CHECK:      <__llvm_kcfi_check_22222222>:
# CHECK:        b.ne 0x14000105c <__llvm_kcfi_check_mismatch_22222222>
# CHECK:      <__llvm_kcfi_check_mismatch_11111111>:
# CHECK-NEXT:   adrp x16, 0x140002000
# CHECK-NEXT:   add x16, x16, #0x10
# CHECK-NEXT:   adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x38
# CHECK-NEXT:   br x17
# CHECK:      <__llvm_kcfi_check_mismatch_22222222>:
# CHECK-NEXT:   adrp x16, 0x140002000
# CHECK-NEXT:   add x16, x16, #0x28
# CHECK-NEXT:   adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x3c
# CHECK-NEXT:   br x17

## plain's cell; the head, plain's entry and the trailer of 0x11111111; and
## the head and the trailer of 0x22222222.
# DATA:      140002000 40100040 01000000 11111111 00000000
# DATA-NEXT: 140002010 00200040 01000000 23222222 00000000
# DATA-NEXT: 140002020 22222222 00000000 45444444 00000000

#--- plain.s
        .text
        .globl plain
        .p2align 2
plain:
        ret
        .globl plain2
plain2:
        ret

#--- main.s
        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .globl main
        .p2align 2
main:
        bl plain2
        bl __llvm_kcfi_check_11111111
        bl __llvm_kcfi_check_22222222
        ret

        .data
        .p2align 3
        .quad plain

        .weak __kcfi_typeid_plain
__kcfi_typeid_plain = 0x11111111
        .weak __kcfi_inflow_0000000022222222_plain2
__kcfi_inflow_0000000022222222_plain2 = 0x22222222

        .weak __llvm_kcfi_check_mismatch_11111111
__llvm_kcfi_check_mismatch_11111111 = __llvm_kcfi_trap
        .weak __llvm_kcfi_check_mismatch_22222222
__llvm_kcfi_check_mismatch_22222222 = __llvm_kcfi_trap

        .section .text,"xr",discard,__llvm_kcfi_check_11111111
        .globl __llvm_kcfi_check_11111111
        .p2align 2
__llvm_kcfi_check_11111111:
        ldur w16, [x15, #-4]
        mov w17, #0x1111
        cmp w16, w17
        b.ne __llvm_kcfi_check_mismatch_11111111
        ret

        .section .text,"xr",discard,__llvm_kcfi_check_22222222
        .globl __llvm_kcfi_check_22222222
        .p2align 2
__llvm_kcfi_check_22222222:
        ldur w16, [x15, #-4]
        mov w17, #0x2222
        cmp w16, w17
        b.ne __llvm_kcfi_check_mismatch_22222222
        ret

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
