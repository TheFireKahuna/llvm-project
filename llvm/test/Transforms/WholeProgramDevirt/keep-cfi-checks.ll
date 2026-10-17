; RUN: sed 's/x86_64-unknown-linux-gnu/x86_64-unknown-windows-itanium/' %s \
; RUN:   | opt -S -passes=wholeprogramdevirt -whole-program-visibility \
; RUN:   -pass-remarks=wholeprogramdevirt 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=REMARK,KEEP
; RUN: sed 's/x86_64-unknown-linux-gnu/x86_64-pc-windows-ntposix/' %s \
; RUN:   | opt -S -passes=wholeprogramdevirt -whole-program-visibility \
; RUN:   | FileCheck %s --check-prefix=KEEP
; RUN: opt -S -passes=wholeprogramdevirt -whole-program-visibility %s \
; RUN:   | FileCheck %s --check-prefix=DROP

;; On Windows Itanium and NT-POSIX, the CFI check of a virtual call outlives
;; single-implementation devirtualization, which still runs a method on the
;; object, and virtual constant propagation, which still reads at an offset
;; from its vtable pointer. Uniform and unique return value optimization,
;; which read nothing through the vtable pointer, drop it as they do on other
;; targets.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

; REMARK-DAG: single-impl: devirtualized a call to sf
; REMARK-DAG: uniform-ret-val: devirtualized a call to ug1
; REMARK-DAG: unique-ret-val: devirtualized a call to qx
; REMARK-DAG: virtual-const-prop: devirtualized a call to vh1

@vts = constant [1 x ptr] [ptr @sf], !type !0
@vtu1 = constant [1 x ptr] [ptr @ug1], !type !1
@vtu2 = constant [1 x ptr] [ptr @ug2], !type !1
@vtq1 = constant [1 x ptr] [ptr @qx], !type !2
@vtq2 = constant [1 x ptr] [ptr @qy], !type !2
@vtv1 = constant [1 x ptr] [ptr @vh1], !type !3
@vtv2 = constant [1 x ptr] [ptr @vh2], !type !3

define i32 @sf(ptr %this, i32 %a) { ret i32 %a }
define i32 @ug1(ptr %this) readnone { ret i32 7 }
define i32 @ug2(ptr %this) readnone { ret i32 7 }
define i1 @qx(ptr %this) readnone { ret i1 true }
define i1 @qy(ptr %this) readnone { ret i1 false }
define i32 @vh1(ptr %this, i32 %a) readnone {
  %r = add i32 %a, 1
  ret i32 %r
}
define i32 @vh2(ptr %this, i32 %a) readnone {
  %r = add i32 %a, 2
  ret i32 %r
}

; KEEP-LABEL: define i32 @single(
; KEEP:         [[T:%.*]] = call i1 @llvm.type.test(ptr %vtable, metadata !"s")
; KEEP:         br i1 [[T]], label %cont, label %trap
; KEEP:         call i32 @sf(
; DROP-LABEL: define i32 @single(
; DROP-NOT:     @llvm.type.test
; DROP:         br i1 true, label %cont, label %trap
define i32 @single(ptr %obj, i32 %a) {
  %vtable = load ptr, ptr %obj
  %p = call { ptr, i1 } @llvm.type.checked.load(ptr %vtable, i32 0, metadata !"s")
  %ok = extractvalue { ptr, i1 } %p, 1
  br i1 %ok, label %cont, label %trap
trap:
  call void @llvm.trap()
  unreachable
cont:
  %f = extractvalue { ptr, i1 } %p, 0
  %r = call i32 %f(ptr %obj, i32 %a)
  ret i32 %r
}

; KEEP-LABEL: define i32 @uniform(
; KEEP-NOT:     @llvm.type.test
; KEEP:         br i1 true, label %cont, label %trap
; DROP-LABEL: define i32 @uniform(
; DROP-NOT:     @llvm.type.test
; DROP:         br i1 true, label %cont, label %trap
define i32 @uniform(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call { ptr, i1 } @llvm.type.checked.load(ptr %vtable, i32 0, metadata !"u")
  %ok = extractvalue { ptr, i1 } %p, 1
  br i1 %ok, label %cont, label %trap
trap:
  call void @llvm.trap()
  unreachable
cont:
  %f = extractvalue { ptr, i1 } %p, 0
  %r = call i32 %f(ptr %obj)
  ret i32 %r
}

; KEEP-LABEL: define i1 @unique(
; KEEP-NOT:     @llvm.type.test
; KEEP:         br i1 true, label %cont, label %trap
; DROP-LABEL: define i1 @unique(
; DROP-NOT:     @llvm.type.test
; DROP:         br i1 true, label %cont, label %trap
define i1 @unique(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call { ptr, i1 } @llvm.type.checked.load(ptr %vtable, i32 0, metadata !"q")
  %ok = extractvalue { ptr, i1 } %p, 1
  br i1 %ok, label %cont, label %trap
trap:
  call void @llvm.trap()
  unreachable
cont:
  %f = extractvalue { ptr, i1 } %p, 0
  %r = call i1 %f(ptr %obj)
  ret i1 %r
}

; KEEP-LABEL: define i32 @vcp(
; KEEP:         [[T:%.*]] = call i1 @llvm.type.test(ptr %vtable, metadata !"v")
; KEEP:         br i1 [[T]], label %cont, label %trap
; KEEP:         load i32, ptr
; DROP-LABEL: define i32 @vcp(
; DROP-NOT:     @llvm.type.test
; DROP:         br i1 true, label %cont, label %trap
define i32 @vcp(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call { ptr, i1 } @llvm.type.checked.load(ptr %vtable, i32 0, metadata !"v")
  %ok = extractvalue { ptr, i1 } %p, 1
  br i1 %ok, label %cont, label %trap
trap:
  call void @llvm.trap()
  unreachable
cont:
  %f = extractvalue { ptr, i1 } %p, 0
  %r = call i32 %f(ptr %obj, i32 5)
  ret i32 %r
}

declare { ptr, i1 } @llvm.type.checked.load(ptr, i32, metadata)
declare void @llvm.trap()

!0 = !{i32 0, !"s"}
!1 = !{i32 0, !"u"}
!2 = !{i32 0, !"q"}
!3 = !{i32 0, !"v"}
