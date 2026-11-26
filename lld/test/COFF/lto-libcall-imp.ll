; REQUIRES: x86, aarch64

;; A libcall LTO makes is resolved after input resolution: a fork object's
;; partners are loaded again, repeatedly (__udivti3, then __udivmodti4 through
;; its __imp_ pointer).

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{a} = x64
; DEFINE: %{t} = x86_64
; DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
; DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console
; DEFINE: %{run} = \
; DEFINE:   opt -mtriple=%{t}-unknown-windows-itanium main.ll -o full.bc && \
; DEFINE:   opt -mtriple=%{t}-unknown-windows-itanium -module-summary main.ll \
; DEFINE:     -o thin.bc && %{mc} %{a}-udivti3.s -o udivti3.obj && \
; DEFINE:   %{mc} udivmodti4.s -o udivmodti4.obj && \
; DEFINE:   llvm-lib -machine:%{a} udivti3.obj udivmodti4.obj \
; DEFINE:     -out:builtins.lib && \
; DEFINE:   %{link} -out:full.exe full.bc builtins.lib -debug:symtab \
; DEFINE:     -verbose 2>&1 | FileCheck %s && \
; DEFINE:   %{link} -out:thin.exe thin.bc builtins.lib -verbose 2>&1 | \
; DEFINE:     FileCheck %s && \
; DEFINE:   llvm-objdump -d full.exe | FileCheck --check-prefix=IMAGE-%{a} %s

; RUN: %{run}
; REDEFINE: %{a} = arm64
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; CHECK:     Loaded builtins.lib(udivti3.obj) for __udivti3
; CHECK:     Loading lazy __udivmodti4 from builtins.lib for __imp___udivmodti4
; CHECK-NOT: undefined symbol

;; __udivti3's jump through the pointer reaches __udivmodti4 directly.
; IMAGE-x64:      <__udivti3>:
; IMAGE-x64-NEXT: jmp 0x[[#%x,MOD:]] <__udivmodti4>
; IMAGE-x64:      [[#%.16x,MOD]] <__udivmodti4>:

; IMAGE-arm64:      <__udivti3>:
; IMAGE-arm64-NEXT: adr x16, 0x[[#%x,MOD:]]
; IMAGE-arm64:      [[#%.16x,MOD]] <__udivmodti4>:

;--- main.ll
define i64 @main(i128 %a, i128 %b) {
  %q = udiv i128 %a, %b
  %r = trunc i128 %q to i64
  ret i64 %r
}

;--- x64-udivti3.s
.globl __udivti3
__udivti3:
  jmp *__imp___udivmodti4(%rip)

;--- arm64-udivti3.s
.globl __udivti3
__udivti3:
  adrp x16, __imp___udivmodti4
  ldr x16, [x16, :lo12:__imp___udivmodti4]
  br x16

;--- udivmodti4.s
.globl __udivmodti4
__udivmodti4:
  ret
