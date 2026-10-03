; On Windows Itanium and NT-POSIX a catch-type entry is a 32-bit offset to a
; pointer to the descriptor: the import pointer of a descriptor the linker can
; see, and a pointer private to the object for a local one.
;
; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s -o %t.obj
; RUN: llvm-readobj -r %t.obj | FileCheck %s --check-prefix=RELOC

@_ZTI8Imported = external constant ptr
@_ZTIN12_GLOBAL__N_14AnonE = internal constant { ptr, ptr } { ptr null, ptr null }

declare void @may_throw()
declare i32 @__gxx_personality_seh0(...)

define i32 @f() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw()
          to label %ret unwind label %lpad

lpad:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTI8Imported
          catch ptr @_ZTIN12_GLOBAL__N_14AnonE
          catch ptr null
  %sel = extractvalue { ptr, i32 } %lp, 1
  ret i32 %sel

ret:
  ret i32 0
}

; CHECK:      .byte 155 // @TType Encoding = indirect pcrel sdata4
; CHECK:      [[T3:.Ltmp[0-9]+]]: // TypeInfo 3
; CHECK-NEXT: .word __imp__ZTI8Imported-[[T3]]
; CHECK-NEXT: [[T2:.Ltmp[0-9]+]]: // TypeInfo 2
; CHECK-NEXT: .word .L_ZTIN12_GLOBAL__N_14AnonE.DW.stub-[[T2]]
; CHECK-NEXT: .word 0 // TypeInfo 1
; CHECK:      .section .rdata,"dr"
; CHECK-NOT:  .section
; CHECK:      .L_ZTIN12_GLOBAL__N_14AnonE.DW.stub:
; CHECK-NEXT: .xword _ZTIN12_GLOBAL__N_14AnonE

; RELOC:      Section ({{[0-9]+}}) .xdata {
; RELOC:        IMAGE_REL_ARM64_REL32 __imp__ZTI8Imported
; RELOC-NEXT:   IMAGE_REL_ARM64_REL32 .rdata
; RELOC-NEXT: }
