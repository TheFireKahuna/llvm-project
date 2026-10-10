// .linkprotecteddelayiat says that the object's delay-load helper writes a
// delay-load import address table only while the table is writable. It is a
// group of kind 16 with an empty payload, which a linker that does not use it
// skips.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s -o %t.arm64.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.arm64.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// "LLRC", version 1, no capabilities, then one group of kind 16 and 0 bytes,
// however often the directive appears.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01001000 LLRC....{{$}}
// ASM:        .linkprotecteddelayiat
// ASM-NEXT:   .linkprotecteddelayiat

  .linkprotecteddelayiat
  .linkprotecteddelayiat
