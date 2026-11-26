;; A unit that only declares a thread-local variable tests its guard through a
;; declaration that names the variable. The declaration is a word of the
;; variable's record and has no symbol, and a module that links the unit with
;; the definition reads the guard that the definition's record states.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: opt -mtriple=x86_64-unknown-windows-itanium use.ll -o use.bc
; RUN: opt -mtriple=x86_64-unknown-windows-itanium def.ll -o def.bc
; RUN: llvm-lto2 dump-symtab use.bc | FileCheck %s --check-prefix=SYMTAB
; RUN: llvm-lto2 run use.bc def.bc -o out -save-temps \
; RUN:   -r=use.bc,get,px -r=use.bc,count, -r=def.bc,count,px
; RUN: llvm-dis out.0.0.preopt.bc -o - | FileCheck %s

; SYMTAB:     DU----T- count
; SYMTAB-NOT: count$guard

; CHECK-NOT: count$guard
; CHECK:     define {{.*}} @get()
; CHECK-NEXT:  load i8, ptr @guard

;--- use.ll
target triple = "x86_64-unknown-windows-itanium"

@count = external thread_local global i32
@"count$guard" = external thread_local global i8, !tls_record !0

define i32 @get() {
  %g = load i8, ptr @"count$guard"
  %c = load i32, ptr @count
  %w = zext i8 %g to i32
  %r = add i32 %c, %w
  ret i32 %r
}

!0 = !{ptr @count}

;--- def.ll
target triple = "x86_64-unknown-windows-itanium"

@count = thread_local global i32 0, !tls_record !0
@guard = internal thread_local global i8 0

!0 = !{ptr @guard}
