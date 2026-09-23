; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llc -filetype=obj -function-sections -data-sections %t/input.ll -o %t/input.obj
; RUN: llc -filetype=obj -function-sections -data-sections %t/runtime.ll -o %t/runtime.obj
; RUN: llvm-as %t/input.ll -o %t/input.bc
; RUN: llvm-as %t/runtime.ll -o %t/runtime.bc
; RUN: opt -module-summary %t/input.ll -o %t/input.thin.bc
; RUN: opt -module-summary %t/runtime.ll -o %t/runtime.thin.bc
; RUN: lld-link /dll /entry:_DllMainCRTStartup /import-slots /out:%t/native.dll %t/input.obj %t/runtime.obj
; RUN: lld-link /dll /entry:_DllMainCRTStartup /import-slots /out:%t/full.dll %t/input.bc %t/runtime.bc
; RUN: lld-link /dll /entry:_DllMainCRTStartup /import-slots /out:%t/thin.dll %t/input.thin.bc %t/runtime.thin.bc
; RUN: %python %t/check.py %t

; Exercise the runtime's per-image contribution contract with a minimal CRT
; fixture. Each image runs its own constructor list and owns its own runtime
; state. Only the primary image invokes the application's DllMain callback.

;--- input.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
@initialized = internal global i32 0, partition "child"
@callbacks = internal global i32 0
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @initialize, ptr null }]
declare ptr @runtime_address()
define internal void @initialize() partition "child" {
  store i32 23, ptr @initialized
  ret void
}
define i32 @feature() noinline partition "child" {
  %value = load volatile i32, ptr @initialized
  ret i32 %value
}
define ptr @feature_address() noinline partition "child" {
  %address = call ptr @runtime_address()
  ret ptr %address
}
define dllexport ptr @main_address() {
  %address = call ptr @runtime_address()
  ret ptr %address
}
define dllexport i32 @entry() {
  %value = call i32 @feature()
  %count = load volatile i32, ptr @callbacks
  %result = add i32 %value, %count
  ret i32 %result
}
define i32 @DllMain(ptr %image, i32 %reason, ptr %reserved) {
  %old = load i32, ptr @callbacks
  %next = add i32 %old, 1
  store i32 %next, ptr @callbacks
  ret i32 1
}

;--- runtime.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
@__xc_a = constant [1 x ptr] zeroinitializer, section ".CRT$XCA"
@__xc_z = constant [1 x ptr] zeroinitializer, section ".CRT$XCZ"
@state = internal global i32 0
declare i32 @DllMain(ptr, i32, ptr)
define ptr @runtime_address() noinline {
  ret ptr @state
}
define i32 @__wincrt_DefaultDllMain(ptr %image, i32 %reason, ptr %reserved) {
  ret i32 1
}
define i32 @_DllMainCRTStartup(ptr %image, i32 %reason, ptr %reserved) {
entry:
  %attach = icmp eq i32 %reason, 1
  br i1 %attach, label %initialize, label %done
initialize:
  store i32 1, ptr @state
  br label %loop
loop:
  %at = phi ptr [ getelementptr ([1 x ptr], ptr @__xc_a, i64 0, i64 1), %initialize ], [ %next, %call ]
  %end = icmp eq ptr %at, @__xc_z
  br i1 %end, label %callback, label %call
call:
  %function = load ptr, ptr %at
  call void %function()
  %next = getelementptr ptr, ptr %at, i64 1
  br label %loop
callback:
  %result = call i32 @DllMain(ptr %image, i32 %reason, ptr %reserved)
  ret i32 %result
done:
  ret i32 1
}
!llvm.linker.options = !{!0}
!0 = !{!"/lldimagelocal"}

;--- check.py
import ctypes
import pathlib
import sys

if sys.platform == 'win32':
    root = pathlib.Path(sys.argv[1]).resolve()
    policy = ctypes.c_uint32(1)
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.SetProcessMitigationPolicy.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    assert kernel32.SetProcessMitigationPolicy(2, ctypes.byref(policy), ctypes.sizeof(policy))
    for mode in ['native', 'full', 'thin']:
        image = ctypes.CDLL(str(root / (mode + '.dll')))
        assert image.entry() == 24, mode
        image.main_address.restype = ctypes.c_void_p
        image.feature_address.restype = ctypes.c_void_p
        assert image.main_address() != image.feature_address(), mode
