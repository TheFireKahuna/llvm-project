; RUN: split-file %s %t
; RUN: opt -module-summary %t/a.ll -o %t/a.bc
; RUN: opt -module-summary %t/b.ll -o %t/b.bc
; RUN: llvm-dis %t/a.bc -o - | FileCheck %s --check-prefix=SUMMARY
; RUN: llvm-dis %t/a.bc -o - | llvm-as -o - | llvm-dis -o - \
; RUN:   | FileCheck %s --check-prefix=SUMMARY

;; The summary records "thread-pointer-unused". The thin link clears it from a
;; function that calls one holding a thread pointer at a hot call site, and the
;; backend gives each declaration passed its thread pointer what the summaries
;; of its definitions say: that it holds none, or that it reads it.
; RUN: llvm-lto2 run %t/a.bc %t/b.bc -o %t1 -save-temps \
; RUN:   -r=%t/a.bc,calls_hot$tp,plx -r=%t/a.bc,calls_cold$tp,plx \
; RUN:   -r=%t/a.bc,reader$tp, -r=%t/a.bc,unused$tp, -r=%t/a.bc,foreign$tp, \
; RUN:   -r=%t/b.bc,reader$tp,p -r=%t/b.bc,unused$tp,p -r=%t/b.bc,x,p
; RUN: llvm-dis %t1.1.1.promote.bc -o - | FileCheck %s

; SUMMARY: ^{{[0-9]+}} = gv: (name: "calls_cold", {{.*}}funcFlags: ({{.*}}, threadPointerUnused: 1)

; CHECK: declare void @reader() [[READ:#[0-9]+]]
; CHECK: declare void @unused() [[UNUSED:#[0-9]+]]
; CHECK: declare void @foreign() [[HOLDS:#[0-9]+]]
; CHECK: define dso_local void @calls_hot() [[HOLDS]]
; CHECK: define dso_local void @calls_cold(i1 %c) [[UNUSED]]
; CHECK-DAG: attributes [[HOLDS]] = { "thread-pointer-argument" }
; CHECK-DAG: attributes [[UNUSED]] = { "thread-pointer-argument" "thread-pointer-unused" }
; CHECK-DAG: attributes [[READ]] = { "thread-pointer-argument"="read" }

;--- a.ll
target triple = "x86_64-pc-windows-ntposix"

declare void @reader() "thread-pointer-argument"
declare void @unused() "thread-pointer-argument"
declare void @foreign() "thread-pointer-argument"

define void @calls_hot() "thread-pointer-argument" "thread-pointer-unused" !prof !20 {
  call void @reader()
  call void @unused()
  call void @foreign()
  ret void
}

define void @calls_cold(i1 %c) "thread-pointer-argument" "thread-pointer-unused" !prof !20 {
  br i1 %c, label %cold, label %exit, !prof !41
cold:
  call void @reader()
  br label %exit
exit:
  ret void
}

!41 = !{!"branch_weights", i32 1, i32 1000}
!llvm.module.flags = !{!1}
!20 = !{!"function_entry_count", i64 1000}
!1 = !{i32 1, !"ProfileSummary", !2}
!2 = !{!3, !4, !5, !6, !7, !8, !9, !10}
!3 = !{!"ProfileFormat", !"InstrProf"}
!4 = !{!"TotalCount", i64 10000}
!5 = !{!"MaxCount", i64 10}
!6 = !{!"MaxInternalCount", i64 1}
!7 = !{!"MaxFunctionCount", i64 1000}
!8 = !{!"NumCounts", i64 3}
!9 = !{!"NumFunctions", i64 3}
!10 = !{!"DetailedSummary", !11}
!11 = !{!12, !13, !14}
!12 = !{i32 10000, i64 100, i32 1}
!13 = !{i32 999000, i64 100, i32 1}
!14 = !{i32 999999, i64 1, i32 2}

;--- b.ll
target triple = "x86_64-pc-windows-ntposix"

@x = thread_local global i32 0

define void @reader() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @x)
  store i32 1, ptr %p
  ret void
}

define void @unused() "thread-pointer-argument" "thread-pointer-unused" {
  ret void
}
