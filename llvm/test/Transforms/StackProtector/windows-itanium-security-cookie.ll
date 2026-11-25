; REQUIRES: x86-registered-target
; DEFINE: %{sp} = opt -S -passes='require<libcall-lowering-info>,stack-protector'
; RUN: %{sp} -mtriple=x86_64-unknown-windows-itanium %s | FileCheck %s --check-prefix=LOCAL
; RUN: %if aarch64-registered-target %{ sed 's/^;DECLARED //' %s | %{sp} -mtriple=aarch64-unknown-windows-itanium | FileCheck %s --check-prefix=LOCAL %}
; RUN: sed 's/^;IMPORTED //' %s | %{sp} -mtriple=x86_64-unknown-windows-itanium | FileCheck %s --check-prefix=IMPORTED
; RUN: %{sp} -mtriple=x86_64-pc-windows-msvc %s | FileCheck %s --check-prefix=MSVC

; Every Windows Itanium image defines its own security cookie, so a protected
; function reads it directly, whether the pass or the module declares it,
; unless the module declares it imported.

; LOCAL:    @__security_cookie = external dso_local global ptr
; IMPORTED: @__security_cookie = external dllimport global ptr
; MSVC:     @__security_cookie = external global ptr

;DECLARED @__security_cookie = external global ptr
;IMPORTED @__security_cookie = external dllimport global ptr

declare void @use(ptr)

define void @f() sspstrong {
  %a = alloca [16 x i8]
  call void @use(ptr %a)
  ret void
}
