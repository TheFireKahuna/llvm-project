; REQUIRES: x86-registered-target, aarch64-registered-target
; DEFINE: %{sp} = opt -S -passes='require<libcall-lowering-info>,stack-protector'
; RUN: %{sp} -mtriple=x86_64-unknown-windows-itanium %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: %{sp} -mtriple=aarch64-unknown-windows-itanium %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: %{sp} -mtriple=x86_64-pc-windows-ntposix %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: %{sp} -mtriple=aarch64-pc-windows-ntposix %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: sed 's/^;TLS //' %s | %{sp} -mtriple=x86_64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,TLS
; RUN: sed 's/^;TLS //' %s | %{sp} -mtriple=aarch64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,TLS
; RUN: sed 's/^;DECLARED //' %s | %{sp} -mtriple=x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: sed 's/^;IMPORTED //' %s | %{sp} -mtriple=aarch64-pc-windows-ntposix | FileCheck %s --check-prefix=IMPORTED
; RUN: %{sp} -mtriple=x86_64-pc-windows-msvc %s | FileCheck %s --check-prefix=MSVC

; On Windows Itanium and NT-POSIX the stack guard is __stack_chk_guard, a word
; of the image's own, read directly whether the pass or the module declares it,
; unless the module declares it imported; the pass leaves it to instruction
; selection. Under -mstack-protector-guard=tls, and in a function
; whose thread pointer is pinned, it is the thread's guard at 0x28 from the
; thread pointer, which the pass reads and checks itself, calling
; __stack_chk_fail on a mismatch. MSVC keeps its security cookie.

; GLOBAL:   @__stack_chk_guard = external dso_local global ptr
; IMPORTED: @__stack_chk_guard = external dllimport global ptr
; MSVC:     @__security_cookie = external global ptr

;DECLARED @__stack_chk_guard = external global ptr
;IMPORTED @__stack_chk_guard = external dllimport global ptr

; CHECK-LABEL: define void @f()
; GLOBAL:        call ptr @llvm.stackguard()
; TLS:           %[[TP:.+]] = call ptr @llvm.thread.pointer.p0()
; TLS-NEXT:      %[[G:.+]] = getelementptr i8, ptr %[[TP]], i32 40
; TLS-NEXT:      load volatile ptr, ptr %[[G]]
define void @f() sspstrong {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-LABEL: define void @pinned()
; CHECK:         %[[TP:.+]] = call ptr @llvm.thread.pointer.p0()
; CHECK-NEXT:    %[[G:.+]] = getelementptr i8, ptr %[[TP]], i32 40
; CHECK-NEXT:    %[[V:.+]] = load volatile ptr, ptr %[[G]]
; CHECK-NEXT:    call void @llvm.stackprotector(ptr %[[V]], ptr %StackGuardSlot)
; CHECK:         call void @__stack_chk_fail()
define void @pinned() sspstrong "pin-thread-pointer" "thread-pointer-argument" {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-NOT: __security_cookie

declare void @use(ptr)

;TLS !llvm.module.flags = !{!0}
;TLS !0 = !{i32 1, !"stack-protector-guard", !"tls"}
