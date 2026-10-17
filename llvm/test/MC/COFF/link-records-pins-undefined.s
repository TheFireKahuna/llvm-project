// Only a symbol the object defines can be pinned. A required pin of any other
// symbol is an error; an advisory one is dropped with a warning.

// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -defsym=ADVISORY=1 -o %t.o 2>&1 | FileCheck %s --check-prefix=WARN
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | \
// RUN:   FileCheck %s --check-prefix=OBJ

// CHECK: error: pin of 'undef', which is not defined
// CHECK: error: pin of 'unused', which is not defined

// WARN: warning: pin of 'undef', which is not defined; the pin is dropped
// WARN: warning: pin of 'unused', which is not defined; the pin is dropped

// The records hold only the pin of a (symbol 9), at 8 modulo 64, and none of
// the dropped pins.
// OBJ:      Contents of section .llvm_link_records:
// OBJ-NEXT: 0000 4c4c5243 01030103 090c08

  .data
a:
  .quad undef

.ifdef ADVISORY
.linkpin undef, 6, 8
.linkpin unused, 6, 8
.else
.linkpin undef, 6, 8, required
.linkpin unused, 6, 8, required
.endif
.linkpin a, 6, 8
