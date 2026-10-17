// Check that ".lto_discard" makes ".weak" ignore the specified symbols on
// COFF, as it does on ELF.
// RUN: llvm-mc -triple x86_64-pc-windows-msvc < %s | FileCheck %s
// RUN: llvm-mc -filetype=obj -triple x86_64-pc-windows-msvc %s -o %t.o
// RUN: llvm-readobj --symbols %t.o | FileCheck %s --check-prefix=OBJ

// CHECK-NOT:   .weak foo
// CHECK:       .weak bar

.lto_discard foo
.weak foo

.lto_discard
.weak bar
bar:
    .byte 2

// What the LLVM LTO library emits for the module that does not keep a weak
// definition that two bitcode inputs each carry through file-scope inline asm:
// the symbol takes neither its value nor a weak external, which on COFF would
// default to zero and meet the kept definition as a second one.
// OBJ-NOT:  Name: qux
// OBJ-NOT:  Name: .weak.qux
.lto_discard qux
.weak qux
.set qux, 0xCAFEC0DE

// "Preserve first, discard later": the second pair is ignored and the first
// definition keeps its value.
// OBJ:      Name: .weak.baz.default
// OBJ-NEXT: Value: 3405693150
.weak baz
.set baz, 0xCAFEC0DE
.lto_discard baz
.weak baz
.set baz, 0xCAFEC0DE
