; RUN: llc -mtriple=x86_64-unknown-windows-itanium -function-sections \
; RUN:   -filetype=obj %s -o %t.x.o
; RUN: llvm-readobj -r --coff-link-records %t.x.o | \
; RUN:   FileCheck %s --check-prefixes=CHECK,X64
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -function-sections \
; RUN:   -filetype=obj %s -o %t.a.o %}
; RUN: %if aarch64-registered-target %{ llvm-readobj -r --coff-link-records %t.a.o | \
; RUN:   FileCheck %s --check-prefixes=CHECK,A64 %}

; Compiled code states what the linker may resize. On AArch64 eh's section,
; with a call through an import pointer, is listed and leaf's is not. eh's
; table of call sites is spans from a label at its start, which .pdata names;
; its unwind data is not packed, and its length and the start of its epilogue
; scope are SSPAN_WORDS18 from that label. On x86-64 the linker moves no code
; after such a call, so no section is listed and the table holds numbers.

; CHECK:      Section ({{[0-9]+}}) .xdata {
; A64-NEXT:     IMAGE_REL_LLVM_SSPAN_WORDS18 $L
; A64-NEXT:     IMAGE_REL_ARM64_PAIR [[EH:\$L[0-9a-f]+]]
; A64-NEXT:     IMAGE_REL_LLVM_SSPAN_WORDS18 $L
; A64-NEXT:     IMAGE_REL_ARM64_PAIR [[EH]]
; CHECK-NEXT:   IMAGE_REL_LLVM_ADDR32NB_CALL __gxx_personality_seh0
; A64-NEXT:     IMAGE_REL_LLVM_SSPAN_ULEB $L
; A64-NEXT:     IMAGE_REL_ARM64_PAIR [[EH]]
; X64-NEXT:   }
; CHECK:      Section ({{[0-9]+}}) .pdata {
; A64-NEXT:     IMAGE_REL_ARM64_ADDR32NB [[EH]]
; X64-NEXT:     IMAGE_REL_AMD64_ADDR32NB .text
; X64-NOT:      Kind: Sections
; A64:          Kind: Sections (0x2)
; A64-NEXT:     ResizableSection: .text
; A64-NEXT:   }

declare dllimport void @imp(i32)
declare void @may_throw()
declare i32 @__gxx_personality_seh0(...)

define i32 @eh(i32 %x) personality ptr @__gxx_personality_seh0 {
entry:
  call void @imp(i32 %x)
  invoke void @may_throw() to label %ok unwind label %lp
ok:
  call void @imp(i32 1)
  ret i32 0
lp:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  call void @imp(i32 2) [ "funclet"(token %cp) ]
  catchret from %cp to label %ok
}

define void @leaf() {
  ret void
}
