# REQUIRES: aarch64

## On ARM64, the linker's form of a local KCFI check thunk, with the target in
## X15: where the image has a code range, a target outside it fails fast; a
## target inside it takes the type check and the return if a function of the
## type is in the range, and goes to the mismatch routine otherwise. A mismatch
## routine placed right after the thunk is reached by the thunk's branches
## alone; another one through a jump at the thunk's end.

# RUN: split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -triple aarch64-windows-msvc main.s -filetype=obj -o main.obj
# RUN: lld-link main.obj -machine:arm64 -guard:cf -entry:main -debug:symtab \
# RUN:   -out:main.exe
# RUN: llvm-readobj --sections main.exe > main.txt
# RUN: llvm-objdump -d main.exe >> main.txt
# RUN: FileCheck %s < main.txt

# CHECK:      Name: .text
# CHECK-NEXT: VirtualSize: 0x[[#%X,SIZE:]]

# CHECK:      <__llvm_kcfi_local_check_11111111>:
# CHECK-NEXT:   adrp x16, 0x140001000
# CHECK-NEXT:   add x16, x16, #0x0
# CHECK-NEXT:   sub x16, x15, x16
# CHECK-NEXT:   mov x17, #0x[[#%x,SIZE]]
# CHECK-NEXT:   movk x17, #0x0, lsl #16
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.hs 0x[[FAIL:[0-9a-f]+]]
# CHECK-NEXT:   ldur x16, [x15, #-0x8]
# CHECK-NEXT:   mov x17, #0x1c5a
# CHECK-NEXT:   movk x17, #0xb807, lsl #16
# CHECK-NEXT:   movk x17, #0x1111, lsl #32
# CHECK-NEXT:   movk x17, #0x1111, lsl #48
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.ne 0x[[ROUTINE:[0-9a-f]+]] <__llvm_kcfi_check_mismatch_11111111>
# CHECK-NEXT:   ret
# CHECK-NEXT:   [[FAIL]]: {{.*}}mov w0, #0x40
# CHECK-NEXT:   brk #0xf003
# CHECK-EMPTY:
# CHECK-NEXT: [[ROUTINE]] <__llvm_kcfi_check_mismatch_11111111>:

# CHECK:      <__llvm_kcfi_local_check_22222222>:
# CHECK-NEXT:   adrp x16, 0x140001000
# CHECK-NEXT:   add x16, x16, #0x0
# CHECK-NEXT:   sub x16, x15, x16
# CHECK-NEXT:   mov x17, #0x[[#%x,SIZE]]
# CHECK-NEXT:   movk x17, #0x0, lsl #16
# CHECK-NEXT:   cmp x16, x17
# CHECK-NEXT:   b.lo 0x[[MISMATCH:[0-9a-f]+]]
# CHECK-NEXT:   mov w0, #0x40
# CHECK-NEXT:   brk #0xf003
# CHECK-NEXT:   [[MISMATCH]]: {{.*}}adrp x17, 0x140001000
# CHECK-NEXT:   add x17, x17, #0x{{[0-9a-f]+}}
# CHECK-NEXT:   br x17

#--- main.s
  .linktypeprefixes
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

        .def __llvm_kcfi_check_mismatch_11111111; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_check_mismatch_11111111
        .globl __llvm_kcfi_check_mismatch_11111111
        .p2align 2
__llvm_kcfi_check_mismatch_11111111:
        brk #0xf003

        .def main; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,main
        .p2align 4
        .globl main
main:
        bl sealed
        adrp x15, listed
        add x15, x15, :lo12:listed
        bl __llvm_kcfi_local_check_11111111
        bl __llvm_kcfi_local_check_22222222
        ret

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .weak __llvm_kcfi_check_mismatch_22222222
__llvm_kcfi_check_mismatch_22222222 = __llvm_kcfi_trap

        .def __llvm_kcfi_local_check_11111111; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_local_check_11111111
        .globl __llvm_kcfi_local_check_11111111
        .p2align 4
__llvm_kcfi_local_check_11111111:
        .linkkcfithunk __llvm_kcfi_local_check_11111111, local_check, 0x11111111, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_11111111
        b __llvm_kcfi_check_mismatch_11111111

        .def __llvm_kcfi_local_check_22222222; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_local_check_22222222
        .globl __llvm_kcfi_local_check_22222222
        .p2align 4
__llvm_kcfi_local_check_22222222:
        .linkkcfithunk __llvm_kcfi_local_check_22222222, local_check, 0x22222222, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_22222222
        b __llvm_kcfi_check_mismatch_22222222

        .def __llvm_kcfi_trap; .scl 2; .type 32; .endef
        .section .text,"xr",discard,__llvm_kcfi_trap
        .globl __llvm_kcfi_trap
        .p2align 4
__llvm_kcfi_trap:
        mov w0, #64
        brk #0xf003

        .section .gfids$y,"dr"
        .symidx listed

        .section .rdata,"dr"
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad 0
        .globl _load_config_used
_load_config_used:
        .long 320
        .fill 108, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 196, 1, 0
