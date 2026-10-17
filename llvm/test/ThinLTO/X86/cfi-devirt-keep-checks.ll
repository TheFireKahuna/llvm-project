; REQUIRES: x86-registered-target

;; On Windows Itanium, a CFI check that outlives single-implementation
;; devirtualization reaches the backend with the type test's resolution, which
;; the thin link exports although every call site of the type was
;; devirtualized: the call is direct and the check still traps on a vtable
;; outside the type. On other targets the check goes.

; RUN: sed -e 's/x86_64-grtev4-linux-gnu/x86_64-unknown-windows-itanium/' \
; RUN:   -e 's/e-m:e-/e-m:w-/' %s | opt -thinlto-bc -thinlto-split-lto-unit -o %t.o
; RUN: opt -thinlto-bc -thinlto-split-lto-unit -o %t.elf.o %s
; RUN: llvm-lto2 run %t.o -save-temps -pass-remarks=. \
; RUN:   -whole-program-visibility \
; RUN:   -o %t2 \
; RUN:   -r=%t.o,test,px \
; RUN:   -r=%t.o,_ZN1A1nEi,p \
; RUN:   -r=%t.o,_ZN1B1fEi,p \
; RUN:   -r=%t.o,_ZN1C1fEi,p \
; RUN:   -r=%t.o,_ZTV1B, \
; RUN:   -r=%t.o,_ZTV1C, \
; RUN:   -r=%t.o,_ZN1A1nEi, \
; RUN:   -r=%t.o,_ZN1B1fEi, \
; RUN:   -r=%t.o,_ZN1C1fEi, \
; RUN:   -r=%t.o,_ZTV1B,px \
; RUN:   -r=%t.o,_ZTV1C,px 2>&1 | FileCheck %s --check-prefix=REMARK
; RUN: llvm-dis %t2.1.4.opt.bc -o - | FileCheck %s
; RUN: llvm-lto2 run %t.elf.o -save-temps -whole-program-visibility -o %t3 \
; RUN:   -r=%t.elf.o,test,px \
; RUN:   -r=%t.elf.o,_ZN1A1nEi,p \
; RUN:   -r=%t.elf.o,_ZN1B1fEi,p \
; RUN:   -r=%t.elf.o,_ZN1C1fEi,p \
; RUN:   -r=%t.elf.o,_ZTV1B, \
; RUN:   -r=%t.elf.o,_ZTV1C, \
; RUN:   -r=%t.elf.o,_ZN1A1nEi, \
; RUN:   -r=%t.elf.o,_ZN1B1fEi, \
; RUN:   -r=%t.elf.o,_ZN1C1fEi, \
; RUN:   -r=%t.elf.o,_ZTV1B,px \
; RUN:   -r=%t.elf.o,_ZTV1C,px
; RUN: llvm-dis %t3.1.4.opt.bc -o - | FileCheck %s --check-prefix=DROP

; REMARK: single-impl: devirtualized a call to _ZN1A1nEi

; CHECK-LABEL: define i32 @test
; CHECK:         br i1 {{%.*}}, label %{{trap|cont}}, label %{{cont|trap}}
; CHECK:       trap:
; CHECK-NEXT:    tail call void @llvm.trap()
; CHECK:       cont:
; CHECK-NEXT:    tail call i32 @_ZN1A1nEi

; DROP-LABEL: define i32 @test
; DROP-NOT:     @llvm.trap
; DROP:         tail call i32 @_ZN1A1nEi
; DROP-NOT:     @llvm.trap
; DROP:       }

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-grtev4-linux-gnu"

@_ZTV1B = constant { [4 x ptr] } { [4 x ptr] [ptr null, ptr undef, ptr @_ZN1B1fEi, ptr @_ZN1A1nEi] }, !type !0, !type !1
@_ZTV1C = constant { [4 x ptr] } { [4 x ptr] [ptr null, ptr undef, ptr @_ZN1C1fEi, ptr @_ZN1A1nEi] }, !type !0, !type !2

define i32 @test(ptr %obj, i32 %a) {
entry:
  %vtable = load ptr, ptr %obj
  %0 = tail call { ptr, i1 } @llvm.type.checked.load(ptr %vtable, i32 8, metadata !"_ZTS1A")
  %1 = extractvalue { ptr, i1 } %0, 1
  br i1 %1, label %cont, label %trap

trap:
  tail call void @llvm.trap()
  unreachable

cont:
  %2 = extractvalue { ptr, i1 } %0, 0
  %call = tail call i32 %2(ptr nonnull %obj, i32 %a)
  ret i32 %call
}

declare { ptr, i1 } @llvm.type.checked.load(ptr, i32, metadata)
declare void @llvm.trap()

declare i32 @_ZN1B1fEi(ptr %this, i32 %a)
declare i32 @_ZN1A1nEi(ptr %this, i32 %a)
declare i32 @_ZN1C1fEi(ptr %this, i32 %a)

!0 = !{i64 16, !"_ZTS1A"}
!1 = !{i64 16, !"_ZTS1B"}
!2 = !{i64 16, !"_ZTS1C"}
