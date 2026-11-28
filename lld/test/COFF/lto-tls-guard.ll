; REQUIRES: x86, aarch64

;; A unit that only declares a thread-local variable tests its guard through a
;; declaration that names the variable, a word of the variable's record with
;; no symbol of its own, so bitcode lists none. The user links with the unit
;; that defines the variable, either in bitcode or in an object, under full and
;; thin LTO, and code that LTO generates reads the guard that the definition's
;; record states, directly where it compiles the definition, else through the
;; record.

; RUN: rm -rf %t && split-file %s %t && cd %t
; DEFINE: %{t} = x86_64
; DEFINE: %{m} = x64
; DEFINE: %{build} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium \
; DEFINE:     %S/Inputs/fill-entry.s -o fill.obj && \
; DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium sup.s -o sup.obj && \
; DEFINE:   lld-link -machine:%{m} -def:rt.def -out:rt.lib && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium def.ll -o def.bc && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium use.ll -o use.bc && \
; DEFINE:   opt -module-summary -mtriple=%{t}-windows-itanium def.ll -o def.thin.bc && \
; DEFINE:   opt -module-summary -mtriple=%{t}-windows-itanium use.ll -o use.thin.bc && \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj def.ll -o def.obj && \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj use.ll -o use.obj
; DEFINE: %{in} =
; DEFINE: %{link} = lld-link -dll -noentry fill.obj sup.obj rt.lib %{in} -out:out.dll \
; DEFINE:   -export:get && llvm-objdump -d --no-show-raw-insn out.dll | \
; DEFINE:   FileCheck %s --implicit-check-not='$guard'

; RUN: %{build}
; REDEFINE: %{in} = def.bc use.bc
; RUN: %{link}
; REDEFINE: %{in} = def.thin.bc use.thin.bc
; RUN: %{link}
; REDEFINE: %{in} = def.obj use.bc
; RUN: %{link}
; REDEFINE: %{in} = def.obj use.thin.bc
; RUN: %{link}
; REDEFINE: %{in} = def.bc use.obj
; RUN: %{link}
; REDEFINE: %{in} = def.thin.bc use.obj
; RUN: %{link}
; REDEFINE: %{t} = aarch64
; REDEFINE: %{m} = arm64
; RUN: %{build}
; REDEFINE: %{in} = def.bc use.bc
; RUN: %{link}
; REDEFINE: %{in} = def.thin.bc use.thin.bc
; RUN: %{link}
; REDEFINE: %{in} = def.obj use.bc
; RUN: %{link}

; CHECK: <get>:

;--- def.ll
$__tls_set = comdat any

@count = dso_local thread_local global i32 0, !tls_record !0 #0
@fixed = dso_local thread_local global i32 7, !tls_record !1
@__tls_guard = internal thread_local global i8 0
@__tls_set = linkonce_odr hidden thread_local constant i8 1, comdat
@llvm.compiler.used = appending global [1 x ptr] [ptr @__tls_set], section "llvm.metadata"

define void @_ZTH5count() {
  %g = load i8, ptr @__tls_guard
  %new = icmp eq i8 %g, 0
  br i1 %new, label %init, label %done
init:
  store i8 1, ptr @__tls_guard
  call ptr @llvm.invariant.start.p0(i64 1, ptr @__tls_guard)
  store i32 40, ptr @count
  br label %done
done:
  ret void
}

declare ptr @llvm.invariant.start.p0(i64, ptr)

attributes #0 = { "dynamic-init"="thread-local" }

!0 = !{ptr @__tls_guard}
!1 = !{ptr @__tls_set}

;--- use.ll
@count = external thread_local global i32
@"count$guard" = external thread_local global i8, !tls_record !0
@fixed = external thread_local global i32
@"fixed$guard" = external thread_local global i8, !tls_record !1
declare extern_weak void @_ZTH5count()
declare extern_weak void @_ZTH5fixed()

define i32 @get() "thread-pointer-argument" {
  %cg = call ptr @llvm.threadlocal.address.p0(ptr @"count$guard")
  %c0 = load i8, ptr %cg
  %cnew = icmp eq i8 %c0, 0
  br i1 %cnew, label %cinit, label %f
cinit:
  call void @_ZTH5count()
  br label %f
f:
  %fg = call ptr @llvm.threadlocal.address.p0(ptr @"fixed$guard")
  %f0 = load i8, ptr %fg
  %fnew = icmp eq i8 %f0, 0
  br i1 %fnew, label %finit, label %read
finit:
  call void @_ZTH5fixed()
  br label %read
read:
  %cp = call ptr @llvm.threadlocal.address.p0(ptr @count)
  %c = load i32, ptr %cp
  %fp = call ptr @llvm.threadlocal.address.p0(ptr @fixed)
  %fv = load i32, ptr %fp
  %r = add i32 %c, %fv
  ret i32 %r
}

declare ptr @llvm.threadlocal.address.p0(ptr)

!0 = !{ptr @count}
!1 = !{ptr @fixed}

;--- sup.s
  .data
  .globl _tls_index
_tls_index:
  .long 0
  .section .rdata,"dr"
  .globl _tls_used
_tls_used:
  .quad _tls_start
  .quad _tls_end
  .quad _tls_index
  .quad 0
  .long 0
  .long 0

;--- rt.def
LIBRARY clang_rt.wincrt_dynamic.dll
EXPORTS
__tp_register
__tp_map DATA
__tp_base DATA
__tp_key DATA
__tp_slow
__tp_personality
