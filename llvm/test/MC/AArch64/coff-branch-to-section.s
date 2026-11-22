// RUN: llvm-mc -triple aarch64-windows -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --symbols %t.o | FileCheck %s

// The field of an ARM64 branch holds no offset from a symbol, so a branch to a
// temporary label in another section names a label symbol at the target, not
// the target's section.

// CHECK:      Section (1) .text {
// CHECK-NEXT:   0x0 IMAGE_REL_ARM64_BRANCH26 .Ltarget
// CHECK:      Name: .Ltarget
// CHECK-NEXT: Value: 4
// CHECK-NEXT: Section: .text$split
// CHECK:      StorageClass: Label

        .text
        b .Ltarget
        .section .text$split,"xr"
        nop
.Ltarget:
        ret
