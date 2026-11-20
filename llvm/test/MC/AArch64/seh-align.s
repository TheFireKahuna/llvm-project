// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-readobj --unwind %t.o | FileCheck %s
// RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=MSVC-RELOC
// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -o %t.i.o
// RUN: llvm-readobj --unwind %t.i.o | FileCheck %s
// RUN: llvm-readobj -r %t.i.o | FileCheck %s --check-prefix=RELOC

// Alignment inside a function leaves its length, and the offset of each
// epilogue after it, to layout: the unwind record states them, unpacked since
// .pdata's form needs the length when written, and an epilogue that ends the
// function is still found to end it.

// CHECK:      Function: one
// CHECK-NEXT: ExceptionRecord:
// CHECK-NEXT: ExceptionData {
// CHECK-NEXT:   FunctionLength: 44
// CHECK:        EpiloguePacked: Yes
// CHECK-NEXT:   EpilogueOffset: 0

// CHECK:      Function: two
// CHECK-NEXT: ExceptionRecord:
// CHECK-NEXT: ExceptionData {
// CHECK-NEXT:   FunctionLength: 76
// CHECK:        EpiloguePacked: No
// CHECK-NEXT:   EpilogueScopes: 2
// CHECK:          StartOffset: 7
// CHECK-NEXT:     EpilogueStartIndex: 0
// CHECK:          StartOffset: 17
// CHECK-NEXT:     EpilogueStartIndex: 3

// A function that may be longer than one record's reach is divided where no
// epilogue can lie across the boundary wherever layout puts it: before both
// of the two epilogues that the alignment's padding may move across the end
// of the first record's reach.

// CHECK:      Function: big (0xC0)
// CHECK-NEXT: ExceptionRecord:
// CHECK-NEXT: ExceptionData {
// CHECK-NEXT:   FunctionLength: 1048504
// CHECK:        EpilogueScopes: 0
// CHECK:      Function: {{.*}} (0x100078)
// CHECK-NEXT: ExceptionRecord:
// CHECK-NEXT: ExceptionData {
// CHECK-NEXT:   FunctionLength: 40084
// CHECK:        EpilogueScopes: 3
// CHECK:          StartOffset: 15
// CHECK:          StartOffset: 17
// CHECK:          StartOffset: 10019

// A function with an instruction the linker may resize has relocations for
// the same words, which hold the distances as assembled.

// CHECK:      Function: resized
// CHECK-NEXT: ExceptionRecord:
// CHECK-NEXT: ExceptionData {
// CHECK-NEXT:   FunctionLength: 56
// CHECK:        EpiloguePacked: No
// CHECK-NEXT:   EpilogueScopes: 2
// CHECK:          StartOffset: 9
// CHECK-NEXT:     EpilogueStartIndex: 3
// CHECK:          StartOffset: 12
// CHECK-NEXT:     EpilogueStartIndex: 3

// MSVC-RELOC-NOT: IMAGE_REL_LLVM
// MSVC-RELOC-NOT: IMAGE_REL_ARM64_PAIR

// RELOC:      Section ({{[0-9]+}}) .text$r {
// RELOC:        0xC IMAGE_REL_LLVM_ALIGN - (132)
// RELOC:      Section ({{[0-9]+}}) .xdata {
// RELOC-NEXT:   0x0 IMAGE_REL_LLVM_SSPAN_WORDS18 [[END:\$L[0-9a-f]+]]
// RELOC-NEXT:   0x0 IMAGE_REL_ARM64_PAIR [[BEGIN:\$L[0-9a-f]+]]
// RELOC-NEXT:   0x4 IMAGE_REL_LLVM_SSPAN_WORDS18 [[FIRST:\$L[0-9a-f]+]]
// RELOC-NEXT:   0x4 IMAGE_REL_ARM64_PAIR [[BEGIN]]
// RELOC-NEXT:   0x8 IMAGE_REL_LLVM_SSPAN_WORDS18 [[SECOND:\$L[0-9a-f]+]]
// RELOC-NEXT:   0x8 IMAGE_REL_ARM64_PAIR [[BEGIN]]
// RELOC-NEXT: }

	.text
	.globl one
	.seh_proc one
one:
	str x30, [sp, #-16]!
	.seh_save_reg_x x30, 16
	.seh_endprologue
	.p2align 4
1:
	nop
	cbnz x0, 1b
	.p2align 5
	nop
	.seh_startepilogue
	ldr x30, [sp], #16
	.seh_save_reg_x x30, 16
	.seh_endepilogue
	ret
	.seh_endproc

	.p2align 6
	.globl two
	.seh_proc two
two:
	str x30, [sp, #-16]!
	.seh_save_reg_x x30, 16
	.seh_endprologue
	cbz x0, 2f
	.p2align 4
1:
	nop
	cbnz x0, 1b
	nop
	.seh_startepilogue
	ldr x30, [sp], #16
	.seh_save_reg_x x30, 16
	.seh_endepilogue
	ret
2:
	.p2align 6
	nop
	.seh_startepilogue
	add sp, sp, #16
	.seh_stackalloc 16
	.seh_endepilogue
	b one
	.seh_endproc

	.p2align 6
	.globl big
	.seh_proc big
big:
	str x30, [sp, #-16]!
	.seh_save_reg_x x30, 16
	.seh_endprologue
	.p2align 6
	.rept 262125
	nop
	.endr
	.seh_startepilogue
	ldr x30, [sp], #16
	.seh_save_reg_x x30, 16
	.seh_endepilogue
	ret
	.seh_startepilogue
	ldr x30, [sp], #16
	.seh_save_reg_x x30, 16
	.seh_endepilogue
	ret
	.rept 10000
	nop
	.endr
	.seh_startepilogue
	ldr x30, [sp], #16
	.seh_save_reg_x x30, 16
	.seh_endepilogue
	ret
	.seh_endproc

	.section .text$r,"xr"
	.globl resized
	.seh_proc resized
resized:
	str x30, [sp, #-16]!
	.seh_save_reg_x x30, 16
	.seh_endprologue
	adrp x8, var
	add x8, x8, :lo12:var
	.p2align 4
1:
	nop
	cbnz x0, 1b
	.p2align 4
	nop
	.seh_startepilogue
	add sp, sp, #16
	.seh_stackalloc 16
	.seh_endepilogue
	ret
	nop
	.seh_startepilogue
	add sp, sp, #16
	.seh_stackalloc 16
	.seh_endepilogue
	ret
	.seh_endproc
