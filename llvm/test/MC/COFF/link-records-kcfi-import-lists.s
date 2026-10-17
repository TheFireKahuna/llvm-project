// .linkkcfilists says that the object's KCFI lists name each import by its
// import address table entry alone. It is a group of the critical kind 3 with
// an empty payload, which a linker that does not support it reports.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s -o %t.arm64.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.arm64.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// "LLRC", version 1, no capabilities, then a group of kind 3 and 0 bytes.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01000300
// ASM:        .linkkcfilists

  .linkkcfilists
