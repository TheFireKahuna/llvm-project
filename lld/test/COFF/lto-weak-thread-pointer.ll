; REQUIRES: x86, aarch64

;; A weak function that bitcode calls passing the thread pointer is null under
;; both its names where nothing defines it: the reference to X$tp binds the
;; null of the weak reference to X, so the test reads zero and skips the call.
;; Where a regular object defines it, LTO makes the reference dso_local, and
;; the call binds it directly, on AArch64 through the import pointer, since
;; the reference may still be null.

; RUN: rm -rf %t && split-file %s %t && cd %t
; DEFINE: %{t} = x86_64
; DEFINE: %{summary} =
; DEFINE: %{run} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium \
; DEFINE:     %S/Inputs/fill-entry.s -o fill.obj && \
; DEFINE:   opt %{summary} -mtriple=%{t}-windows-itanium use.ll -o use.bc && \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj def.ll -o def.obj && \
; DEFINE:   lld-link -dll -noentry fill.obj use.bc -out:absent.dll -export:test && \
; DEFINE:   lld-link -dll -noentry fill.obj use.bc def.obj -out:defined.dll \
; DEFINE:     -export:test && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn absent.dll defined.dll | \
; DEFINE:     FileCheck %s --check-prefix=%{t}

; RUN: %{run}
; REDEFINE: %{summary} = -module-summary
; RUN: %{run}
; REDEFINE: %{t} = aarch64
; REDEFINE: %{summary} =
; RUN: %{run}
; REDEFINE: %{summary} = -module-summary
; RUN: %{run}

; x86_64-LABEL: <test$tp>:
; x86_64-NEXT:    jmp 0x[[#%x,RET:]]
; x86_64-NEXT:    jmpq *
; x86_64-NEXT:    [[#RET]]: retq
; x86_64-LABEL: <test$tp>:
; x86_64-NEXT:    leaq {{.*}} <.text>
; x86_64-NEXT:    testq %rax, %rax
; x86_64-NEXT:    jne {{.*}} <.text>
; x86_64-NEXT:    retq

; aarch64-LABEL: <test$tp>:
; aarch64-NEXT:    mov x0, #0x0
; aarch64-NEXT:    b 0x[[#%x,RET:]]
; aarch64-NEXT:    br x0
; aarch64-NEXT:    [[#RET]]: ret
; aarch64-LABEL: <test$tp>:
; aarch64-NEXT:    b {{.*}} <.text>
; aarch64-NEXT:    ret

;--- use.ll
declare extern_weak void @weak() "thread-pointer-argument"

define void @test() "thread-pointer-argument" "thread-pointer-unused" {
  %c = icmp ne ptr @weak, null
  br i1 %c, label %call, label %exit
call:
  call void @weak()
  br label %exit
exit:
  ret void
}

;--- def.ll
define void @weak() "thread-pointer-argument" "thread-pointer-unused" {
  ret void
}
