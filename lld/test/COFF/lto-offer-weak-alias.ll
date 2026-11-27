; REQUIRES: x86, aarch64

;; A weak alias in bitcode is a weak external, which LTO does not emit and the
;; link binds to its target. Where the code that LTO generates reaches it only
;; through its import pointer, as it reaches a type_info object that names a
;; class incomplete in its unit, the link still binds the alias. A DLL offers
;; the target, the placeholder, and not the alias, which names the complete
;; object, as an object file's.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{t} = x86_64-windows-itanium
; DEFINE: %{run} = \
; DEFINE:   llvm-mc -filetype=obj -triple=%{t} %S/Inputs/fill-entry.s -o fill.obj && \
; DEFINE:   llc -mtriple=%{t} -filetype=obj rt.ll -o rt.obj && \
; DEFINE:   llc -mtriple=%{t} -filetype=obj a.ll -o a.obj && \
; DEFINE:   opt -mtriple=%{t} a.ll -o a.bc && \
; DEFINE:   opt -mtriple=%{t} -module-summary a.ll -o thin.bc && \
; DEFINE:   lld-link -dll -noentry fill.obj rt.obj a.obj -out:obj.dll && \
; DEFINE:   lld-link -dll -noentry fill.obj rt.obj a.bc -out:full.dll && \
; DEFINE:   lld-link -dll -noentry fill.obj rt.obj thin.bc -out:thin.dll && \
; DEFINE:   llvm-readobj --coff-exports obj.dll | FileCheck %s && \
; DEFINE:   llvm-readobj --coff-exports full.dll | FileCheck %s && \
; DEFINE:   llvm-readobj --coff-exports thin.dll | FileCheck %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64-windows-itanium
; RUN: %{run}
; REDEFINE: %{t} = x86_64-pc-windows-ntposix
; RUN: %{run}

; CHECK-NOT: Name: _ZTI1S
; CHECK:     Name: __llvm_incomplete$_ZTI1S
; CHECK-NOT: Name: _ZTI1S

;--- rt.ll
@_ZTVN10__cxxabiv117__class_type_infoE = dllexport global [3 x ptr] zeroinitializer

;--- a.ll
$"__llvm_incomplete$_ZTI1S" = comdat any
$"__llvm_incomplete$_ZTS1S" = comdat any

@"__llvm_incomplete$_ZTI1S" = linkonce_odr dso_local constant { ptr, ptr } { ptr getelementptr inbounds (i8, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 16), ptr @"__llvm_incomplete$_ZTS1S" }, comdat, align 8 #0
@"__llvm_incomplete$_ZTS1S" = linkonce_odr dso_local constant [3 x i8] c"1S\00", comdat, align 1
@_ZTVN10__cxxabiv117__class_type_infoE = external global [0 x ptr]
@_ZTI1S = weak alias { ptr, ptr }, ptr @"__llvm_incomplete$_ZTI1S"

define dllexport ptr @f() {
  ret ptr @_ZTI1S
}

attributes #0 = { "type-info" }
