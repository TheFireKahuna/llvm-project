// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-readobj --symbols %t.o | FileCheck %s

// ".lto_discard" makes ".weak" ignore the symbol on COFF too. Otherwise the
// module whose weak definition from inline asm LTO does not keep would declare
// a weak external, which defaults to zero and meets the kept definition.

// CHECK-NOT: Name: foo
// CHECK:     Name: bar
// CHECK-NOT: Name: foo

.lto_discard foo
.weak foo, bar
.set foo, 1
