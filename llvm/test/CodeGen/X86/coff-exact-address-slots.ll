; RUN: opt -passes='coff-output-locality,verify' -S %s | FileCheck %s
; RUN: opt -passes='coff-output-locality,instcombine,verify' -S %s | FileCheck %s --check-prefix=FOLDED
; RUN: llc -filetype=obj %s -o %t.obj
; RUN: llvm-objdump -d %t.obj | FileCheck %s --check-prefix=ASM

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@data = constant [8 x i64] zeroinitializer, partition "pe:child"
@slot = dllexport constant ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 1), partition "pe:"
@mutable = dllexport global ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 2), partition "pe:"
@weak = weak dllexport constant ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 3), partition "pe:"
@unused = internal constant ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 4), partition "pe:"
@unaligned = dllexport constant ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 5), partition "pe:", align 1
@onepast = dllexport constant ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 8), partition "pe:"

; Nested/signed offsets and instruction GEPs resolve to the same exact address.
; The already exported field supplies one load; later folding must not undo it.
; CHECK: @slot = dso_local dllexport externally_initialized constant ptr
; CHECK-LABEL: define dso_local ptr @get(
; CHECK-NEXT: %[[ADDR:.*]] = load ptr, ptr @slot, align 8, !invariant.load
; CHECK-NEXT: ret ptr %[[ADDR]]
; FOLDED-LABEL: define dso_local ptr @get(
; FOLDED-NEXT: %[[ADDR:.*]] = load ptr, ptr @slot, align 8, !invariant.load
; FOLDED-NEXT: ret ptr %[[ADDR]]
; ASM-LABEL: <get>:
; ASM-NEXT: {{.*}}movq {{.*}}(%rip), %rax
; ASM-NEXT: {{.*}}retq
define ptr @get() partition "pe:" {
  %p = getelementptr i8, ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 2), i64 -8
  ret ptr %p
}

; An address field in another image is not a reason to indirect a local use.
; CHECK-LABEL: define dso_local ptr @local(
; CHECK-NEXT: ret ptr getelementptr
; ASM-LABEL: <local>:
; ASM-NEXT: {{.*}}leaq {{.*}}(%rip), %rax
; ASM-NEXT: {{.*}}retq
define ptr @local() partition "pe:child" {
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 1)
}

; PHI values are loaded on the incoming edge. Repeated uses in a dominated
; block reuse the same acquisition, without an additional load at the join.
; CHECK-LABEL: define dso_local ptr @choose(
; CHECK: left:
; CHECK-NEXT: %[[LEFT:.*]] = load ptr, ptr @slot
; CHECK-NEXT: br label %join
; CHECK: join:
; CHECK-NEXT: %p = phi ptr [ %[[LEFT]], %left ], [ null, %right ]
define ptr @choose(i1 %b) partition "pe:" {
  br i1 %b, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %p = phi ptr [ getelementptr ([8 x i64], ptr @data, i64 0, i64 1), %left ], [ null, %right ]
  ret ptr %p
}

; CHECK-LABEL: define dso_local ptr @dominated(
; CHECK: %[[DOM:.*]] = load ptr, ptr @slot
; CHECK: call void @escape(ptr %[[DOM]])
; CHECK: next:
; CHECK-NOT: load
; CHECK: ret ptr %[[DOM]]
define ptr @dominated() partition "pe:" {
  call void @escape(ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 1))
  br label %next
next:
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 1)
}
declare void @escape(ptr) partition "pe:"

; Ineligible fields retain the normal base acquisition plus offset. Neither
; mutable/replaceable contents, unaligned fields, dead storage nor one-past
; coordinates prove a reusable immutable native slot.
; CHECK-LABEL: define dso_local ptr @fallback(
; CHECK-NEXT: %[[BASE:.*]] = load ptr, ptr @"\01__imp_data"
; CHECK-NEXT: %[[OFFSET:.*]] = getelementptr [8 x i64], ptr %[[BASE]], i64 0, i64 2
; CHECK-NEXT: ret ptr %[[OFFSET]]
define ptr @fallback() partition "pe:" {
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 2)
}
; CHECK-LABEL: define dso_local ptr @weak_fallback(
; CHECK: load ptr, ptr @"\01__imp_data"
; CHECK: getelementptr [8 x i64], ptr {{.*}}, i64 0, i64 3
define ptr @weak_fallback() partition "pe:" {
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 3)
}
; CHECK-LABEL: define dso_local ptr @dead_fallback(
; CHECK: load ptr, ptr @"\01__imp_data"
; CHECK: getelementptr [8 x i64], ptr {{.*}}, i64 0, i64 4
define ptr @dead_fallback() partition "pe:" {
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 4)
}
; CHECK-LABEL: define dso_local ptr @unaligned_fallback(
; CHECK: load ptr, ptr @"\01__imp_data"
; CHECK: getelementptr [8 x i64], ptr {{.*}}, i64 0, i64 5
define ptr @unaligned_fallback() partition "pe:" {
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 5)
}
; CHECK-LABEL: define dso_local ptr @onepast_fallback(
; CHECK: load ptr, ptr @"\01__imp_data"
; CHECK: getelementptr [8 x i64], ptr {{.*}}, i64 0, i64 8
define ptr @onepast_fallback() partition "pe:" {
  ret ptr getelementptr ([8 x i64], ptr @data, i64 0, i64 8)
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"coff.output-set", i32 1}
!1 = !{i32 1, !"coff.import-slots", i32 1}
