// RUN: llvm-mc -triple=aarch64 -filetype=obj -o %t.o %s
// RUN: llvm-readobj -S --sd %t.o | FileCheck %s
// RUN: llvm-mc -triple=aarch64-windows -filetype=obj -o %t.obj %s
// RUN: llvm-readobj -S %t.obj | FileCheck %s --check-prefix=COFF
.section sec00, "ax"
.byte 1
.section sec01, "ax"
nop
nop
.section sec02, "ax"
.balign 4
nop
nop
.section sec03, "ax"
.balign 8
nop
nop
.section sec04, "ax"
.byte 0
.section sec05, "aw"
nop
nop
// CHECK: Name: sec00
// CHECK: AddressAlignment: 4
// CHECK: Name: sec01
// CHECK: AddressAlignment: 4
// CHECK: Name: sec02
// CHECK: AddressAlignment: 4
// CHECK: Name: sec03
// CHECK: AddressAlignment: 8
// CHECK: Name: sec04
// CHECK: AddressAlignment: 4
// CHECK: Name: sec05
// CHECK: AddressAlignment: 1
// A COFF section is aligned as an instruction where it holds one.
// COFF: Name: sec00
// COFF: IMAGE_SCN_ALIGN_1BYTES
// COFF: Name: sec01
// COFF: IMAGE_SCN_ALIGN_4BYTES
// COFF: Name: sec02
// COFF: IMAGE_SCN_ALIGN_4BYTES
// COFF: Name: sec03
// COFF: IMAGE_SCN_ALIGN_8BYTES
// COFF: Name: sec04
// COFF: IMAGE_SCN_ALIGN_1BYTES
// COFF: Name: sec05
// COFF: IMAGE_SCN_ALIGN_4BYTES
