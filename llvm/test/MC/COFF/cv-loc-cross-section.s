# RUN: llvm-mc -triple=x86_64-pc-win32 -filetype=obj < %s -o %t.obj
# RUN: llvm-readobj --codeview %t.obj | FileCheck %s

# A function's code may lie in two sections. Each .cv_linetable lists its
# begin label's section, an inline site's annotations count the nth table's
# code from its begin label after changing the base to n, and a .cv_def_range
# gives a record for each section's ranges.

	.text
	.global baz
baz:
.Lfunc_begin0:
	.cv_file 1 "t.cpp"
	.cv_func_id 0
	.cv_inline_site_id 1 within 0 inlined_at 1 2 1
	.cv_loc 0 1 1 1
	pushq %rbp
	.cv_loc 1 1 7 1
	movq %rsp, %rbp
.Lfunc_end0:

	.section .text$split,"xr"
baz.cold:
	.cv_loc 1 1 8 1
	incl x(%rip)
	.cv_loc 0 1 3 1
	popq %rbp
	retq
.Lcold_end0:

	.section	.debug$S,"dr"
	.long 4
	.long 241 # Symbol subsection
	.long .Lsym_end-.Lsym_begin
.Lsym_begin:
	.short .Linline_end-.Linline_begin
.Linline_begin:
	.short 4429 # S_INLINESITE
	.long 0
	.long 0
	.long 0
	.cv_inline_linetable 1 1 7 .Lfunc_begin0 .Lfunc_end0
.Linline_end:
	.short 2
	.short 4430 # S_INLINESITE_END
	.cv_def_range .Lfunc_begin0 .Lfunc_end0 baz.cold .Lcold_end0, reg, 17
.Lsym_end:
	.p2align 2
	.cv_linetable 0, baz, .Lfunc_end0
	.cv_linetable 0, baz.cold, .Lcold_end0
	.cv_filechecksums
	.cv_stringtable

# CHECK:      InlineSiteSym {
# CHECK:        BinaryAnnotations [
# CHECK-NEXT:     ChangeCodeOffsetAndLineOffset: {CodeOffset: 0x1, LineOffset: 0}
# CHECK-NEXT:     ChangeCodeLength: 0x3
# CHECK-NEXT:     ChangeCodeOffsetBase: 1
# CHECK-NEXT:     ChangeCodeOffsetAndLineOffset: {CodeOffset: 0x0, LineOffset: 1}
# CHECK-NEXT:     ChangeCodeLength: 0x6
# CHECK-NEXT:   ]
# CHECK:      DefRangeRegisterSym {
# CHECK:          OffsetStart: .text+0x0
# CHECK:          Range: 0x4
# CHECK:      DefRangeRegisterSym {
# CHECK:          OffsetStart: baz.cold+0x0
# CHECK:          Range: 0x8
# CHECK:      LinkageName: baz
# CHECK:        +0x0 [
# CHECK-NEXT:     LineNumberStart: 1
# CHECK:        +0x1 [
# CHECK-NEXT:     LineNumberStart: 2
# CHECK-NOT:    +0x
# CHECK:      LinkageName: baz.cold
# CHECK:        +0x0 [
# CHECK-NEXT:     LineNumberStart: 2
# CHECK:        +0x6 [
# CHECK-NEXT:     LineNumberStart: 3
# CHECK-NOT:    +0x
