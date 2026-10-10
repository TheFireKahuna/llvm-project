# REQUIRES: aarch64

## On ARM64, in an image it seals, the linker replaces
## each KCFI check thunk that its object's records describe with its own form:
## the range test becomes one comparison of the target's offset from the start
## of .text with its size, followed by clang's type check and the return for a
## target inside it; a target outside it takes clang's page test, the type
## check and the guard function. The mismatch routine is reached at any
## distance through X17. A type that no unsealed function in the image has goes
## straight to the page test. Without an object
## that says it has KCFI prefixes, clang's form stays.

# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj -o %t.obj
# RUN: llvm-mc -triple aarch64-windows-msvc %s -filetype=obj --defsym NOREC=1 \
# RUN:   -o %t.norec.obj
# RUN: lld-link %t.obj -machine:arm64 -guard:cf -entry:main \
# RUN:   -debug:symtab -out:%t.exe
# RUN: llvm-readobj --sections %t.exe > %t.txt
# RUN: llvm-objdump -d %t.exe >> %t.txt
# RUN: FileCheck %s < %t.txt

# CHECK:      Name: .text
# CHECK-NEXT: VirtualSize: 0x[[#%X,SIZE:]]

## listed is in the guard function table, so type 0x11111111 has an unsealed
## function and tests the range, all of .text.
# CHECK:      <__llvm_kcfi_check_11111111>:
# CHECK-NEXT:   adrp x16, 0x140001000
# CHECK-NEXT:   add x16, x16, #0x0
# CHECK-NEXT:   sub x16, x15, x16
# CHECK-NEXT:   mov x17, #0x[[#%x,SIZE]]
# CHECK-NEXT:   movk x17, #0x0, lsl #16
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.hs {{.*}}<__llvm_kcfi_check_11111111+0x3c>
# CHECK-NEXT:   ldur x16, [x15, #-0x8]
# CHECK-NEXT:   mov x17, #0x1c5a
# CHECK-NEXT:   movk x17, #0xb807, lsl #16
# CHECK-NEXT:   movk x17, #0x1111, lsl #32
# CHECK-NEXT:   movk x17, #0x1111, lsl #48
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.ne {{.*}}<__llvm_kcfi_check_11111111+0x6c>
# CHECK-NEXT:   ret
# CHECK-NEXT:   tst x15, #0xff0
# CHECK-NEXT:   b.eq {{.*}}<__llvm_kcfi_check_11111111+0x6c>
# CHECK-NEXT:   ldur x16, [x15, #-0x8]
# CHECK-NEXT:   mov x17, #0x1c5a
# CHECK-NEXT:   movk x17, #0xb807, lsl #16
# CHECK-NEXT:   movk x17, #0x1111, lsl #32
# CHECK-NEXT:   movk x17, #0x1111, lsl #48
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.ne {{.*}}<__llvm_kcfi_check_11111111+0x6c>
# CHECK-NEXT:   adrp x16, {{.*}}<__guard_check_icall_fptr>
# CHECK-NEXT:   ldr x16, [x16]
# CHECK-NEXT:   br x16
# CHECK-NEXT:   adrp x17, [[MISMATCH:0x[0-9a-f]+]]
# CHECK-NEXT:   add x17, x17, #0x[[#%x,MISMATCH_LO:]]
# CHECK-NEXT:   br x17

## sealed is only called directly, so type 0x22222222 has no unsealed function
## in the image and goes straight to the page test.
# CHECK:      <__llvm_kcfi_check_22222222>:
# CHECK-NEXT:   tst x15, #0xff0
# CHECK-NEXT:   b.eq {{.*}}<__llvm_kcfi_check_22222222+0x30>
# CHECK-NEXT:   ldur x16, [x15, #-0x8]
# CHECK-NEXT:   mov x17, #0x1c5a
# CHECK-NEXT:   movk x17, #0xb807, lsl #16
# CHECK-NEXT:   movk x17, #0x2222, lsl #32
# CHECK-NEXT:   movk x17, #0x2222, lsl #48
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.ne {{.*}}<__llvm_kcfi_check_22222222+0x30>
# CHECK-NEXT:   adrp x16, {{.*}}<__guard_check_icall_fptr>
# CHECK-NEXT:   ldr x16, [x16]
# CHECK-NEXT:   br x16
# CHECK-NEXT:   adrp x17,
# CHECK-NEXT:   add x17, x17,
# CHECK-NEXT:   br x17

## A thunk that its object does not describe is left as it is.
# CHECK:      <__llvm_kcfi_check_33333333>:
# CHECK-NEXT:   adrp x16,
# CHECK-NEXT:   add x16, x16,
# CHECK-NEXT:   cmp x15, x16
# CHECK-NEXT:   b.lo

# RUN: lld-link %t.norec.obj -machine:arm64 -guard:cf -entry:main -debug:symtab \
# RUN:   -out:%t.clang.exe
# RUN: llvm-objdump -d %t.clang.exe | FileCheck %s --check-prefix=CLANG

# CLANG:      <__llvm_kcfi_check_22222222>:
# CLANG-NEXT:   adrp x16,
# CLANG-NEXT:   add x16, x16,
# CLANG-NEXT:   cmp x15, x16
# CLANG-NEXT:   b.lo
# CLANG-NEXT:   adrp x16,
# CLANG-NEXT:   add x16, x16,
# CLANG-NEXT:   cmp x15, x16
# CLANG-NEXT:   b.hs

.ifndef NOREC
  .linktypeprefixes
.endif
        .globl @feat.00
@feat.00 = 0x800

        .def listed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,listed
        .p2align 4
__cfi_listed:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x11111111
        .globl listed
listed:
        ret

        .def sealed; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,sealed
        .p2align 4
__cfi_sealed:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x22222222
        .globl sealed
sealed:
        ret

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        bl sealed
        adrp x15, listed
        add x15, x15, :lo12:listed
        bl __llvm_kcfi_check_11111111
        bl __llvm_kcfi_check_22222222
        bl __llvm_kcfi_check_33333333
        ret

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .weak __llvm_kcfi_check_mismatch_11111111
__llvm_kcfi_check_mismatch_11111111 = __llvm_kcfi_check_open
        .weak __llvm_kcfi_check_mismatch_22222222
__llvm_kcfi_check_mismatch_22222222 = __llvm_kcfi_check_open

        .weak __llvm_kcfi_check_mismatch_33333333
__llvm_kcfi_check_mismatch_33333333 = __llvm_kcfi_check_open

.macro check_thunk type, hi, record=1
        .section .text,"xr",discard,__llvm_kcfi_check_\type
        .globl __llvm_kcfi_check_\type
        .p2align 4
__llvm_kcfi_check_\type:
.if \record
        .linkkcfithunk __llvm_kcfi_check_\type, check, 0x\type, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_\type
.endif
        adrp x16, __llvm_code_start
        add x16, x16, :lo12:__llvm_code_start
        cmp x15, x16
        b.lo 1f
        adrp x16, __llvm_code_end
        add x16, x16, :lo12:__llvm_code_end
        cmp x15, x16
        b.hs 1f
        ldur x16, [x15, #-8]
        movz x17, #0x1c5a
        movk x17, #0xb807, lsl #16
        movk x17, #\hi, lsl #32
        movk x17, #\hi, lsl #48
        cmp x16, x17
        b.ne __llvm_kcfi_check_mismatch_\type
        ret
1:      tst x15, #0xff0
        b.eq __llvm_kcfi_check_mismatch_\type
        ldur x16, [x15, #-8]
        movz x17, #0x1c5a
        movk x17, #0xb807, lsl #16
        movk x17, #\hi, lsl #32
        movk x17, #\hi, lsl #48
        cmp x16, x17
        b.ne __llvm_kcfi_check_mismatch_\type
        adrp x16, __guard_check_icall_fptr
        ldr x16, [x16, :lo12:__guard_check_icall_fptr]
        br x16
.endm
        check_thunk 11111111, 0x1111
        check_thunk 22222222, 0x2222
        check_thunk 33333333, 0x3333, 0

        .section .text,"xr",discard,__llvm_kcfi_check_open
        .globl __llvm_kcfi_check_open
        .p2align 4
__llvm_kcfi_check_open:
        brk #0xf003

        .section .gfids$y,"dr"
        .symidx listed

        .section .rdata,"dr"
        .globl __guard_check_icall_fptr
        .p2align 3
__guard_check_icall_fptr:
        .xword 0
