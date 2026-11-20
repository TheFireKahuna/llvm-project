// A Windows Itanium or NT-POSIX object states the size of a function with
// .size, as the TotalSize of a function definition auxiliary symbol. Only an
// external function symbol defined in a section takes one: a static symbol's
// auxiliary symbol is a section definition. A weak definition's is on its
// default, which is the definition, and its weak external keeps its own. On
// any other COFF triple the directive is accepted and has no effect.

// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-windows-itanium %s \
// RUN:   | llvm-readobj --symbols - | FileCheck %s
// RUN: llvm-mc -filetype=obj -triple aarch64-unknown-windows-ntposix %s \
// RUN:   | llvm-readobj --symbols - | FileCheck %s
// RUN: llvm-mc -filetype=obj -triple x86_64-windows-msvc %s \
// RUN:   | llvm-readobj --symbols - \
// RUN:   | FileCheck %s --check-prefix=MSVC --implicit-check-not=AuxFunctionDef
// RUN: llvm-mc -filetype=obj -triple x86_64-windows-gnu %s \
// RUN:   | llvm-readobj --symbols - \
// RUN:   | FileCheck %s --check-prefix=MSVC --implicit-check-not=AuxFunctionDef
// RUN: not llvm-mc -filetype=obj -triple x86_64-unknown-windows-itanium \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR

// MSVC: Name: ext

	.def ext; .scl 2; .type 32; .endef
	.def stat; .scl 3; .type 32; .endef
	.def weak; .scl 2; .type 32; .endef
	.globl ext
	.weak weak
	.globl data
	.text
ext:
	.long 0
	.long 0
.Lext_end:
	.size ext, .Lext_end-ext
stat:
	.long 0
	.size stat, .-stat
weak:
	.long 0
	.long 0
	.long 0
	.size weak, .-weak
	.data
data:
	.long 0
	.size data, 4
.ifdef ERR
// ERR: error: size expression must be absolute
	.size ext, undefined
.endif

// CHECK:      Name: ext
// CHECK:      StorageClass: External
// CHECK-NEXT: AuxSymbolCount: 1
// CHECK-NEXT: AuxFunctionDef {
// CHECK-NEXT:   TagIndex: 0
// CHECK-NEXT:   TotalSize: 8

// CHECK:      Name: stat
// CHECK:      StorageClass: Static
// CHECK-NEXT: AuxSymbolCount: 0

// CHECK:      Name: weak
// CHECK:      StorageClass: WeakExternal
// CHECK-NEXT: AuxSymbolCount: 1
// CHECK-NEXT: AuxWeakExternal {
// CHECK-NEXT:   Linked: .weak.weak.default.ext

// CHECK:      Name: .weak.weak.default.ext
// CHECK:      StorageClass: External
// CHECK-NEXT: AuxSymbolCount: 1
// CHECK-NEXT: AuxFunctionDef {
// CHECK-NEXT:   TagIndex: 0
// CHECK-NEXT:   TotalSize: 12

// CHECK:      Name: data
// CHECK:      StorageClass: External
// CHECK-NEXT: AuxSymbolCount: 0
