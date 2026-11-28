// RUN: llvm-mc -triple x86_64-pc-win32 -filetype=obj %s | llvm-readobj -u - | FileCheck %s

// An XMM register saved at an offset that is 8 off a multiple of 16 is stated
// in bytes, in the far form.

// CHECK:      UnwindCodes [
// CHECK-NEXT:   0x0E: SAVE_XMM128 reg=XMM7, offset=0x20
// CHECK-NEXT:   0x09: SAVE_XMM128_FAR reg=XMM6, offset=0x8
// CHECK-NEXT:   0x04: ALLOC_SMALL size=40
// CHECK-NEXT: ]

    .text
    .seh_proc func
func:
    subq $40, %rsp
    .seh_stackalloc 40
    movups %xmm6, 8(%rsp)
    .seh_savexmm %xmm6, 8
    movups %xmm7, 32(%rsp)
    .seh_savexmm %xmm7, 32
    .seh_endprologue
    addq $40, %rsp
    ret
    .seh_endproc
