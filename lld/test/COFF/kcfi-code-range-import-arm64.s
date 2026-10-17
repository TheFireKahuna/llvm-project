# REQUIRES: aarch64

## On ARM64, the linker's KCFI check thunk tests the code range of a DLL the
## image imports statically after its own, loading the DLL's two bounds from
## the import address table with one ldp, and returns for a target inside it.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple aarch64-windows-msvc dll.s -filetype=obj -o dll.obj
# RUN: llvm-mc -triple aarch64-windows-msvc exe.s -filetype=obj -o exe.obj
# RUN: lld-link dll.obj -machine:arm64 -dll -noentry -export:d1 -guard:cf \
# RUN:   -out:d.dll -implib:d.lib
# RUN: lld-link exe.obj d.lib -machine:arm64 -guard:cf \
# RUN:   -entry:main -debug:symtab -out:exe.exe
# RUN: llvm-readobj --coff-imports exe.exe > exe.txt
# RUN: llvm-objdump -d exe.exe >> exe.txt
# RUN: FileCheck %s < exe.txt

# CHECK:      Name: d.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x{{[0-9A-F]*}}0{{$}}
# CHECK-NEXT: Symbol: __llvm_code_start (1)
# CHECK-NEXT: Symbol: __llvm_code_end (0)
# CHECK-NEXT: Symbol: d1 (2)

## The image has no function of the type, so only the DLL's range is tested.
# CHECK:      <__llvm_kcfi_check_11111111>:
# CHECK-NEXT:   adrp x16, 0x140002000
# CHECK-NEXT:   add x16, x16, #0x{{[0-9a-f]+}}
# CHECK-NEXT:   ldp x16, x17, [x16]
# CHECK-NEXT:   cmp x15, x16
# CHECK-NEXT:   b.lo
# CHECK-NEXT:   cmp x15, x17
# CHECK-NEXT:   b.lo 0x[[HIT:[0-9a-f]+]]
# CHECK-NEXT:   tst x15, #0xff0
# CHECK-NEXT:   b.eq 0x[[MISMATCH:[0-9a-f]+]]
# CHECK-NEXT:   ldur x16, [x15, #-0x8]
# CHECK-NEXT:   mov x17, #0x1c5a
# CHECK-NEXT:   movk x17, #0xb807, lsl #16
# CHECK-NEXT:   movk x17, #0x1111, lsl #32
# CHECK-NEXT:   movk x17, #0x1111, lsl #48
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.ne 0x[[MISMATCH]]
# CHECK-NEXT:   adrp x16, {{.*}}<__guard_check_icall_fptr>
# CHECK-NEXT:   ldr x16, [x16]
# CHECK-NEXT:   br x16
# CHECK-NEXT:   [[HIT]]: {{.*}}ldur x16, [x15, #-0x8]
# CHECK-NEXT:   mov x17, #0x1c5a
# CHECK-NEXT:   movk x17, #0xb807, lsl #16
# CHECK-NEXT:   movk x17, #0x1111, lsl #32
# CHECK-NEXT:   movk x17, #0x1111, lsl #48
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.ne 0x[[MISMATCH]]
# CHECK-NEXT:   ret
# CHECK-NEXT:   [[MISMATCH]]: {{.*}}adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0xa0
# CHECK-NEXT:   br x17
# CHECK:      <__llvm_kcfi_check_open>:
# CHECK-NEXT:   1400010a0:

#--- dll.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def d1; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,d1
        .p2align 4
__cfi_d1:
        .byte 0x0f, 0x1f, 0x80
        .long 0x071c5a06
        .byte 0xb8
        .long 0x11111111
        .globl d1
d1:
        ret

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .section .gfids$y,"dr"
        .symidx d1

        .section .rdata,"dr"
        .globl _load_config_used
_load_config_used:
        .long 320
        .fill 108, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 196, 1, 0

#--- exe.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        adrp x16, __imp_d1
        ldr x16, [x16, :lo12:__imp_d1]
        blr x16
        mov x15, x16
        bl __llvm_kcfi_check_11111111
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

        .section .text,"xr",discard,__llvm_kcfi_check_11111111
        .globl __llvm_kcfi_check_11111111
        .p2align 4
__llvm_kcfi_check_11111111:
        .linkkcfithunk __llvm_kcfi_check_11111111, check, 0x11111111, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_11111111
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
        movk x17, #0x1111, lsl #32
        movk x17, #0x1111, lsl #48
        cmp x16, x17
        b.ne __llvm_kcfi_check_mismatch_11111111
        ret
1:      tst x15, #0xff0
        b.eq __llvm_kcfi_check_mismatch_11111111
        ldur x16, [x15, #-8]
        movz x17, #0x1c5a
        movk x17, #0xb807, lsl #16
        movk x17, #0x1111, lsl #32
        movk x17, #0x1111, lsl #48
        cmp x16, x17
        b.ne __llvm_kcfi_check_mismatch_11111111
        adrp x16, __guard_check_icall_fptr
        ldr x16, [x16, :lo12:__guard_check_icall_fptr]
        br x16

        .section .text,"xr",discard,__llvm_kcfi_check_open
        .globl __llvm_kcfi_check_open
        .p2align 4
__llvm_kcfi_check_open:
        brk #0x1

        .section .rdata,"dr"
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad 0
