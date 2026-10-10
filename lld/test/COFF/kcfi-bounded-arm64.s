# REQUIRES: aarch64

## On ARM64, under -guard:cf, in an image whose guard
## function table lists a function without a KCFI prefix, here plain, a closed
## type's check routine sends a target in X15 outside the image to the trap,
## and points X16 at an empty list and branches through X17 to the dynamic
## check scanner with any other. The static check scanner, which plain's type
## jumps to, is entered through a routine that keeps X16 and gives it only a
## target outside the image, and the dynamic scanner the rest.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc plain.s -o plain.obj
# RUN: lld-link main.obj plain.obj -machine:arm64 -guard:cf \
# RUN:   -entry:main -debug:symtab -opt:ref -out:main.exe
# RUN: llvm-objdump -d main.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata main.exe | FileCheck %s --check-prefix=DATA

# CHECK:      <__llvm_kcfi_trap>:
# CHECK-NEXT:   140001050:
# CHECK:      <static_scanner>:
# CHECK-NEXT:   140001058:
# CHECK:      <__llvm_kcfi_check_open_dynamic>:
# CHECK-NEXT:   14000105c:
# CHECK:      <__llvm_kcfi_check_mismatch_11111111>:
# CHECK-NEXT:   adrp x16, 0x140002000
# CHECK-NEXT:   add x16, x16, #0x10
# CHECK-NEXT:   adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0xb0
# CHECK-NEXT:   br x17
# CHECK:      <__llvm_kcfi_check_mismatch_22222222>:
# CHECK-NEXT:   adrp x17, 0x140000000
# CHECK-NEXT:   cmp x15, x17
# CHECK-NEXT:   b.lo 0x1400010a4
# CHECK-NEXT:   adrp x17, 0x140005000
# CHECK-NEXT:   cmp x15, x17
# CHECK-NEXT:   b.hs 0x1400010a4
# CHECK-NEXT:   adrp x16, 0x140002000
# CHECK-NEXT:   add x16, x16, #0x30
# CHECK-NEXT:   adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x5c
# CHECK-NEXT:   br x17
# CHECK-NEXT:   1400010a4: {{.*}} adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x50
# CHECK-NEXT:   br x17
# CHECK-EMPTY:
# CHECK-NEXT: <__llvm_kcfi_check_open>:
# CHECK-NEXT:   adrp x17, 0x140000000
# CHECK-NEXT:   cmp x15, x17
# CHECK-NEXT:   b.lo 0x1400010d4
# CHECK-NEXT:   adrp x17, 0x140005000
# CHECK-NEXT:   cmp x15, x17
# CHECK-NEXT:   b.hs 0x1400010d4
# CHECK-NEXT:   adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x5c
# CHECK-NEXT:   br x17
# CHECK-NEXT:   1400010d4: {{.*}} adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x58
# CHECK-NEXT:   br x17

## The empty list, after the guard function table: its head, then the odd word
## that ends it.
# DATA: 140002020 1c100000 60100000 00000000 00000000
# DATA-NEXT: 140002030 01000000 00000000

## Without plain, the table lists only functions with a prefix, and the closed
## type keeps the trap.
# RUN: lld-link main.obj -machine:arm64 -guard:cf -entry:main \
# RUN:   -debug:symtab -opt:ref -out:prefixed.exe
# RUN: llvm-objdump -d prefixed.exe | FileCheck %s --check-prefix=TRAP

# TRAP:      <__llvm_kcfi_check_22222222>:
# TRAP:        b.ne {{.*}} <__llvm_kcfi_trap>
# TRAP-NOT:  <__llvm_kcfi_check_open_dynamic>:

#--- plain.s
        .def plain; .scl 2; .type 32; .endef
        .text
        .globl plain
        .p2align 2
plain:
        ret

        .data
        .p2align 3
        .quad plain

#--- main.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
__cfi_main:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x33333333
        .globl main
main:
        bl __llvm_kcfi_check_11111111
        bl __llvm_kcfi_check_22222222
        ret

        .weak __kcfi_typeid_plain
__kcfi_typeid_plain = 0x11111111

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
static_scanner:
        mov w0, #3

        .section .text,"xr",discard,__llvm_kcfi_check_open_dynamic
        .globl __llvm_kcfi_check_open_dynamic
        .p2align 2
__llvm_kcfi_check_open_dynamic:
        mov w0, #4
