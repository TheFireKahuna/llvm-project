// .linkbinding gives an external definition in a COMDAT a role, and
// .linkbindings says that the object lists every such definition: a group of
// the critical kind 5 that pairs each symbol's index with its role, empty if
// the object lists no definition.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj --defsym EMPTY=1 \
// RUN:   %s -o %t.empty.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.empty.o | \
// RUN:   FileCheck --check-prefix=EMPTY %s
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc --defsym BAD=1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=BAD %s

// "LLRC", version 1, no capabilities, then a group of kind 5 and 4 bytes:
// symbol 8, shared, and symbol 11, data, each with role 0.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01000504 08000b00

// ASM:      .linkbindings
// ASM:      .linkbinding shared, default
// ASM:      .linkbinding data, default

// EMPTY:      Contents of section .llvm_link_records:
// EMPTY-NEXT: 0000 4c4c5243 01000500{{ }}

// BAD: error: expected 'default'

.ifdef EMPTY
  .linkbindings
.else
  .linkbindings
  .section .text,"xr",discard,shared
  .globl shared
shared:
  .linkbinding shared, default
  retq

  .section .data,"dw",discard,data
  .globl data
data:
  .linkbinding data, default
  .long 1
.endif

.ifdef BAD
  .linkbinding shared, hidden
.endif
