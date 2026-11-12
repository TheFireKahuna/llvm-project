# REQUIRES: asserts
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o %t
# RUN: llvm-jitlink -noexec -debug-only=jitlink %t 2>&1 | FileCheck %s
#
# Check that each symbol table index in .gfids$y becomes an edge to the
# symbol it names.
#
# CHECK:      section .gfids$y:
# CHECK:      edges:
# CHECK-NEXT:   (block + 0x00000000), {{.*}}, kind = Keep-Alive, target = f
# CHECK-NEXT:   (block + 0x00000004), {{.*}}, kind = Keep-Alive, target = g

	.text
	.globl	main
main:
	retq
f:
	retq
	.globl	g
g:
	retq

	.section	.gfids$y,"dr"
	.symidx	f
	.symidx	g
