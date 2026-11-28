; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-pc-windows-ntposix -function-sections -filetype=obj < %s -o %t.o
; RUN: llvm-readobj -r --sections --section-data %t.o | FileCheck %s --check-prefix=OBJ
; RUN: sed 's/^!llvm.module.flags = !{!0, !1}/!llvm.module.flags = !{!0, !1, !4}/' %s \
; RUN:   | llc -mtriple=aarch64-pc-windows-ntposix | FileCheck %s --check-prefix=CFG

;; The word is the entry's distance from the function, a REL32 of its symbol
;; from a field 16 bytes before the function, and 0F 1F 82 or, after a second
;; type, 83 states it.
; OBJ:      0000: F4FFFFFF 0F1F8210 000000B8 4E61BC80
; OBJ:      0010: F4FFFFFF 0F1F8310 000000B8 4E61BC80
; OBJ:      0x0 IMAGE_REL_ARM64_REL32 reader$tp
; OBJ:      0x10 IMAGE_REL_ARM64_REL32 vreader$tp

declare ptr @llvm.thread.pointer.p0()

define void @reader(ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  ret void
}

define void @vreader(ptr %p) "thread-pointer-argument" !kcfi_type !2 !kcfi_vfn_type !3 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  ret void
}

;; A function that holds the thread pointer passes it on a checked call
;; through a pointer. The check accepts the type with or without its tag,
;; bit 31, which the compare of the difference shifted left by one leaves out,
;; and goes to the target plus its offset where the target is tagged and its
;; prefix holds one: in place where the call clobbers the target's register,
;; through X9 where it keeps it. Under Control Flow Guard checks, which accept
;; no thread-pointer entry, the call passes none.

; CHECK-LABEL: holder$tp:
; CHECK:         ldur w16, [x20, #-4]
; CHECK-NEXT:    movk w17, #24910
; CHECK-NEXT:    movk w17, #188, lsl #16
; CHECK-NEXT:    eor w16, w16, w17
; CHECK-NEXT:    cmp wzr, w16, lsl #1
; CHECK-NEXT:    b.eq [[PASS:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[TRAP:.Ltmp[0-9]+]]:
; CHECK-NEXT:    brk #0x8234
; CHECK-NEXT:  [[PASS]]:
; CHECK-NEXT:    mov x9, x20
; CHECK-NEXT:    tbz w16, #31, [[CALL:.Ltmp[0-9]+]]
; CHECK-NEXT:    ldurb w17, [x20, #-10]
; CHECK-NEXT:    tbz w17, #1, [[TRAP]]
; CHECK-NEXT:    ldursw x17, [x20, #-16]
; CHECK-NEXT:    add x9, x9, x17
; CHECK-NEXT:  [[CALL]]:
; CHECK-NEXT:    blr x9
; CFG-LABEL:   holder$tp:
; CFG-NOT:       eor w16, w16, w17
; CFG:         .Lfunc_end
define void @holder(ptr %f, ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  call void %f(ptr %p) [ "kcfi"(i32 12345678) ]
  call void %f(ptr %p) [ "kcfi"(i32 12345678) ]
  ret void
}

; CHECK-LABEL: tail$tp:
; CHECK:         cmp wzr, w16, lsl #1
; CHECK-NEXT:    b.eq [[PASS:.Ltmp[0-9]+]]
; CHECK-NEXT:  [[TRAP:.Ltmp[0-9]+]]:
; CHECK-NEXT:    brk #0x8222
; CHECK-NEXT:  [[PASS]]:
; CHECK-NEXT:    tbz w16, #31, [[CALL:.Ltmp[0-9]+]]
; CHECK-NEXT:    ldurb w17, [x2, #-10]
; CHECK-NEXT:    tbz w17, #1, [[TRAP]]
; CHECK-NEXT:    ldursw x17, [x2, #-16]
; CHECK-NEXT:    add x2, x2, x17
; CHECK-NEXT:  [[CALL]]:
; CHECK-NEXT:    br x2
define void @tail(ptr %f, ptr %p) "thread-pointer-argument" !kcfi_type !2 {
  %tp = call ptr @llvm.thread.pointer.p0()
  store ptr %tp, ptr %p
  tail call void %f(ptr %p) [ "kcfi"(i32 12345678) ]
  ret void
}

;; The type prefix of a plain entry with a body of its own, emitted after the
;; bodies, holds the offset of its thread-pointer entry before the pattern and
;; sets bit 31 of the type.
; CHECK:      {{^}}__cfi_reader:
; CHECK-NEXT: [[W:.Ltmp[0-9]+]]:
; CHECK-NEXT:   .word reader$tp-([[W]]+16)
; CHECK-NEXT:   .xword -5188146770453848305
; CHECK-NEXT:   .word 2159829326
; CHECK-NEXT: {{^}}reader:
; CHECK:      {{^}}__cfi_vreader:
; CHECK-NEXT:   .word 7
; CHECK-NEXT: [[W:.Ltmp[0-9]+]]:
; CHECK-NEXT:   .word vreader$tp-([[W]]+16)
; CHECK-NEXT:   .xword -5188146770453782769
; CHECK-NEXT:   .word 2159829326
; CHECK-NEXT: {{^}}vreader:

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 7}
!4 = !{i32 2, !"cfguard", i32 2}
