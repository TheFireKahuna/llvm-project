; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,GLOBAL
; RUN: sed 's/^;TLS //' %s | llc -mtriple=aarch64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,TLS
; RUN: sed 's/^;TLS //' %s | llc -mtriple=aarch64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,TLS

; On Windows Itanium and NT-POSIX a protected function reads its image's own
; guard, __stack_chk_guard, in one literal load, and stores it subtracted from
; the stack pointer, reading no thread pointer and setting up no frame
; pointer; a function whose stack an alloca moves subtracts it from the frame
; pointer. Under -mstack-protector-guard=tls, and in a function whose thread
; pointer is pinned, it reads the thread's guard at 0x28 from the thread
; pointer instead. A mismatch calls __stack_chk_fail.

; CHECK-LABEL:  f$tp:
; CHECK-NOT:      x29
; GLOBAL:         adrp [[A:x[0-9]+]], __stack_chk_guard
; GLOBAL:         ldr [[G:x[0-9]+]], [[[A]], :lo12:__stack_chk_guard]
; GLOBAL-NEXT:    sub [[G]], sp, [[G]]
; GLOBAL-NEXT:    str [[G]], [sp, #[[SLOT:[0-9]+]]]
; GLOBAL:         bl use
; GLOBAL-NEXT:    ldr [[S:x[0-9]+]], [sp, #[[SLOT]]]
; GLOBAL-NEXT:    adrp [[A2:x[0-9]+]], __stack_chk_guard
; GLOBAL-NEXT:    ldr [[G2:x[0-9]+]], [[[A2]], :lo12:__stack_chk_guard]
; GLOBAL-NEXT:    sub [[S]], sp, [[S]]
; GLOBAL-NEXT:    cmp [[S]], [[G2]]
; GLOBAL:         __imp___stack_chk_fail
; TLS:            ldr [[T:x[0-9]+]], [x28, #40]
; TLS:            str [[T]], [sp, #[[SLOT:[0-9]+]]]
; TLS:            bl use
; TLS-NEXT:       ldr [[T2:x[0-9]+]], [x28, #40]
; TLS-NEXT:       ldr [[S:x[0-9]+]], [sp, #[[SLOT]]]
; TLS-NEXT:       cmp [[T2]], [[S]]
; TLS:            bl __stack_chk_fail
define void @f() sspstrong "thread-pointer-argument" {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-LABEL:  pinned$tp:
; CHECK:          ldr [[T:x[0-9]+]], [x28, #40]
; CHECK:          str [[T]], [sp, #[[SLOT:[0-9]+]]]
; CHECK:          bl use
; CHECK:          ldr [[T2:x[0-9]+]], [x28, #40]
; CHECK:          bl __stack_chk_fail
define void @pinned() sspstrong "pin-thread-pointer" "thread-pointer-argument" {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}

; CHECK-LABEL:  alloca$tp:
; CHECK:          mov x29, sp
; GLOBAL:         ldr [[G:x[0-9]+]], [{{x[0-9]+}}, :lo12:__stack_chk_guard]
; GLOBAL-NEXT:    sub [[G]], x29, [[G]]
; GLOBAL-NEXT:    stur [[G]], [x29, #-8]
; GLOBAL:         bl use
; GLOBAL:         ldur [[S:x[0-9]+]], [x29, #-8]
; GLOBAL:         sub [[S]], x29, [[S]]
; TLS:            ldr [[T:x[0-9]+]], [x28, #40]
; TLS:            stur [[T]], [x29, #-8]
define void @alloca(i64 %n) sspstrong "thread-pointer-argument" {
  %a = alloca i8, i64 %n
  call void @use(ptr %a)
  ret void
}

; CHECK-NOT: __security_cookie

declare void @use(ptr)

;TLS !llvm.module.flags = !{!0}
;TLS !0 = !{i32 1, !"stack-protector-guard", !"tls"}
