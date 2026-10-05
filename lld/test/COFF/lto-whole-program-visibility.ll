; REQUIRES: x86
;; -lto-whole-program-visibility upgrades the visibility of vtables to the
;; image's, as ELF's --lto-whole-program-visibility does, so that calls
;; through them devirtualise; a vtable the image exports stays public.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: opt --passes=assign-guid -o main.obj main.ll
; RUN: opt --passes=assign-guid --thinlto-bc -o main-thin.obj main.ll
; RUN: llc -filetype=obj -o native.obj native.ll
; RUN: llc -filetype=obj -o native-rtti.obj native-rtti.ll

;; Regular LTO and ThinLTO.
; RUN: lld-link -lto-whole-program-visibility main.obj -out:a.exe \
; RUN:   -entry:start -subsystem:console -mllvm:-pass-remarks=. 2>&1 \
; RUN:   | FileCheck %s --check-prefix=DEVIRT
; RUN: lld-link -lto-whole-program-visibility main-thin.obj -out:a.exe \
; RUN:   -entry:start -subsystem:console -mllvm:-pass-remarks=. 2>&1 \
; RUN:   | FileCheck %s --check-prefix=DEVIRT
; DEVIRT:     single-impl: devirtualized a call to _ZN1A1fEv
; DEVIRT-NOT: devirtualized a call to _ZN1E1fEv

;; Without it, or with it turned off, nothing devirtualises.
; RUN: lld-link main.obj -out:a.exe -entry:start -subsystem:console \
; RUN:   -mllvm:-pass-remarks=. 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NODEVIRT --allow-empty
; RUN: lld-link -lto-whole-program-visibility -lto-whole-program-visibility:no \
; RUN:   main-thin.obj -out:a.exe -entry:start -subsystem:console \
; RUN:   -mllvm:-pass-remarks=. 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NODEVIRT --allow-empty
; NODEVIRT-NOT: devirtualized

;; With validation, which reads the summary, a vtable defined in a native
;; object without a type info
;; turns the upgrade off, and a type info a native object refers to keeps its
;; class public.
; RUN: lld-link -lto-whole-program-visibility \
; RUN:   -lto-validate-all-vtables-have-type-infos main-thin.obj native.obj \
; RUN:   -out:a.exe -entry:start -subsystem:console -mllvm:-pass-remarks=. 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NORTTI
; NORTTI:     RTTI missing for vtable _ZTV1N, /lto-whole-program-visibility disabled
; NORTTI-NOT: devirtualized
; RUN: lld-link -lto-whole-program-visibility \
; RUN:   -lto-validate-all-vtables-have-type-infos -lto-known-safe-vtables:_ZTV1N \
; RUN:   main-thin.obj native.obj -out:a.exe -entry:start -subsystem:console \
; RUN:   -mllvm:-pass-remarks=. 2>&1 | FileCheck %s --check-prefix=DEVIRT
; RUN: lld-link -lto-whole-program-visibility \
; RUN:   -lto-validate-all-vtables-have-type-infos main-thin.obj native-rtti.obj \
; RUN:   -out:a.exe -entry:start -subsystem:console -mllvm:-pass-remarks=. 2>&1 \
; RUN:   | FileCheck %s --check-prefix=RTTI --allow-empty
; RTTI-NOT: devirtualized a call to _ZN1A1fEv
; RTTI-NOT: RTTI missing

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@_ZTI1A = constant [2 x ptr] zeroinitializer
@_ZTI1E = dllexport constant [2 x ptr] zeroinitializer
@_ZTV1A = constant { [3 x ptr] } { [3 x ptr] [ptr null, ptr @_ZTI1A, ptr @_ZN1A1fEv] }, !type !0, !vcall_visibility !2
;; Exported: another image may derive from E.
@_ZTV1E = dllexport constant { [3 x ptr] } { [3 x ptr] [ptr null, ptr @_ZTI1E, ptr @_ZN1E1fEv] }, !type !1, !vcall_visibility !2
@llvm.used = appending global [2 x ptr] [ptr @_ZTV1A, ptr @_ZTV1E]

define i32 @start(ptr %a, ptr %e) {
  %vt = load ptr, ptr %a
  %p = call i1 @llvm.public.type.test(ptr %vt, metadata !"_ZTS1A")
  call void @llvm.assume(i1 %p)
  %f = load ptr, ptr %vt
  %r = call i32 %f(ptr %a)
  %vte = load ptr, ptr %e
  %pe = call i1 @llvm.public.type.test(ptr %vte, metadata !"_ZTS1E")
  call void @llvm.assume(i1 %pe)
  %fe = load ptr, ptr %vte
  %re = call i32 %fe(ptr %e)
  %s = add i32 %r, %re
  ret i32 %s
}

define i32 @_ZN1A1fEv(ptr %this) noinline optnone { ret i32 1 }
define i32 @_ZN1E1fEv(ptr %this) noinline optnone { ret i32 2 }

declare i1 @llvm.public.type.test(ptr, metadata)
declare void @llvm.assume(i1)

!0 = !{i64 16, !"_ZTS1A"}
!1 = !{i64 16, !"_ZTS1E"}
!2 = !{i64 0}

;--- native.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
@_ZTV1N = constant [3 x ptr] zeroinitializer

;--- native-rtti.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
@_ZTI1A = external constant [2 x ptr]
@_ZTV1B = constant [3 x ptr] [ptr null, ptr @_ZTI1B, ptr null]
@_ZTI1B = constant [3 x ptr] [ptr null, ptr null, ptr @_ZTI1A]
