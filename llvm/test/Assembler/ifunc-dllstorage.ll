; RUN: llvm-as < %s | llvm-dis | FileCheck %s

;; An ifunc keeps its DLL storage class through bitcode and assembly, as an
;; alias does: a COFF image exports an ifunc that has it.

@exported = weak_odr dllexport ifunc i32 (i32), ptr @resolver
; CHECK: @exported = weak_odr dllexport ifunc i32 (i32), ptr @resolver

@plain = ifunc i32 (i32), ptr @resolver
; CHECK: @plain = ifunc i32 (i32), ptr @resolver

define internal ptr @resolver() {
  ret ptr null
}
