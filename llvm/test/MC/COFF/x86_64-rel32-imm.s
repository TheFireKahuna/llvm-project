// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -dr %t.o | FileCheck %s

// A RIP-relative field followed by an immediate is REL32_1, REL32_2 or
// REL32_4, which counts from the instruction's end, so its addend is the
// symbol offset alone. A field the assembler resolves is unchanged.

// CHECK:      c6 05 00 00 00 00 01                movb
// CHECK-NEXT:   IMAGE_REL_AMD64_REL32_1 x
// CHECK-NEXT: 66 c7 05 04 00 00 00 01 00          movw
// CHECK-NEXT:   IMAGE_REL_AMD64_REL32_2 x
// CHECK-NEXT: f7 05 00 00 00 00 01 00 00 00       testl
// CHECK-NEXT:   IMAGE_REL_AMD64_REL32_4 x
// CHECK-NEXT: 80 3d 00 00 00 00 01                cmpb
// CHECK-NOT:  IMAGE_REL

  movb $1, x(%rip)
  movw $1, x+4(%rip)
  testl $1, x(%rip)
  cmpb $1, y(%rip)
y:
  ret
