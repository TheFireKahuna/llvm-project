; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | FileCheck %s --check-prefix=WIN64
; RUN: llc < %s -mtriple=i686-pc-windows-msvc | FileCheck %s --check-prefix=WIN32
; RUN: llc < %s -mtriple=i686-pc-windows-gnu | FileCheck %s --check-prefix=MINGW32
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | FileCheck %s --check-prefix=A64 %}

; On Windows the thread pointer is the thread's TLS array, which the TEB holds
; at 0x58 on 64-bit targets and 0x2c on 32-bit ones; AArch64 reserves
; TPIDR_EL0 and reaches the TEB in x18.

; WIN64:   movq %gs:88, %rax
; WIN32:   movl %fs:__tls_array, %eax
; MINGW32: movl %fs:44, %eax
; A64:     ldr x0, [x18, #88]
define ptr @thread_pointer() {
  %p = call ptr @llvm.thread.pointer()
  ret ptr %p
}
