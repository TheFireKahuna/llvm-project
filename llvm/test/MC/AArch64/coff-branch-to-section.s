// RUN: llvm-mc -triple aarch64-windows -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --symbols %t.o | FileCheck %s --check-prefixes=CHECK,MSVC
// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s -o %t.wi.o
// RUN: llvm-readobj -r --symbols %t.wi.o | FileCheck %s --check-prefixes=CHECK,WI
// RUN: not llvm-mc -triple aarch64-windows -filetype=obj --defsym ERR=1 %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR

// The field of an ARM64 branch holds no offset from a symbol, so a branch of
// any type to a temporary label in another section names a label symbol at
// the target, not the target's section. A b outside an unwind frame is a
// BRANCH_TAIL on Windows Itanium.

// CHECK:      Section ({{[0-9]+}}) .text {
// MSVC-NEXT:    0x0 IMAGE_REL_ARM64_BRANCH26 .Ltarget
// WI-NEXT:      0x0 IMAGE_REL_LLVM_BRANCH_TAIL .Ltarget
// CHECK-NEXT:   0x4 IMAGE_REL_ARM64_BRANCH19 .Ltarget
// CHECK-NEXT:   0x8 IMAGE_REL_ARM64_BRANCH14 .Ltarget
// CHECK-NEXT: }
// CHECK:      Name: .Ltarget
// CHECK-NEXT: Value: 4
// CHECK-NEXT: Section: .text$split
// CHECK:      StorageClass: Label

        .text
        b .Ltarget
        cbz x0, .Ltarget
        tbz w0, #1, .Ltarget
        .section .text$split,"xr"
other:
        nop
.Ltarget:
        ret

.ifdef ERR
        .text
        cbz x0, other+4
// ERR: [[#@LINE-1]]:22: error: cannot perform a PC-relative fixup with a non-zero symbol offset
        tbz w0, #1, other+4
// ERR: [[#@LINE-1]]:26: error: cannot perform a PC-relative fixup with a non-zero symbol offset
.endif
