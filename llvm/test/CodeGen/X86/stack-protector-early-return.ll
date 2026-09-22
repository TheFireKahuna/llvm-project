; RUN: llc -mtriple=x86_64-linux-gnu -start-before=stack-protector -stop-after=stack-protector -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=IR
; RUN: llc -mtriple=x86_64-linux-gnu -enable-selectiondag-sp=false -start-before=stack-protector -stop-after=stack-protector %s -o - | FileCheck %s --check-prefix=IR
; RUN: llc -mtriple=x86_64-windows-msvc -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=WIN
; RUN: llc -mtriple=x86_64-windows-itanium -verify-machineinstrs %s -o - | FileCheck %s --check-prefix=WIN

declare i32 @consume(ptr)
declare void @side_effect()
declare i32 @__gxx_personality_v0(...)

; Protection stays on the path exposing the local's address. Frame lowering
; remains free to reserve the frame at entry, as Windows unwind rules require.
define i32 @tail_exit(i1 %slow, i64 %value, ptr %fn) sspstrong {
; IR-LABEL: define i32 @tail_exit(
; IR: entry:
; IR-NOT: @llvm.stackprotector
; IR: br i1 %slow
; IR: fast:
; IR-NEXT: %result = tail call i32 %fn(i64 %value)
; IR-NEXT: ret i32 %result
; IR: protected:
; IR: call void @llvm.stackprotector
; IR: call i32 @consume
; IR: call void @__stack_chk_fail
; WIN-LABEL: tail_exit:
; WIN-NOT: __security_cookie
; WIN: testb
; WIN-NEXT: je [[FAST:.LBB[0-9_]+]]
; WIN: __security_cookie
; WIN: callq consume
; WIN: __security_cookie
; WIN: [[FAST]]:
; WIN-NOT: __security_cookie
; WIN: jmpq
; WIN: callq __security_check_cookie
entry:
  %local = alloca i64, align 8
  br i1 %slow, label %protected, label %fast
fast:
  %result = tail call i32 %fn(i64 %value)
  ret i32 %result
protected:
  store i64 %value, ptr %local, align 8
  %other = call i32 @consume(ptr %local)
  ret i32 %other
}

; A plain early return also needs no guard. Test the opposite successor order.
define i32 @constant_exit(i1 %fast) sspstrong {
; IR-LABEL: define i32 @constant_exit(
; IR: entry:
; IR-NOT: @llvm.stackprotector
; IR: br i1 %fast
; IR: early:
; IR-NEXT: ret i32 0
; IR: protected:
; IR: call void @llvm.stackprotector
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}

; The stronger, unconditional policy is preserved.
define i32 @required(i1 %fast) sspreq {
; IR-LABEL: define i32 @required(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}

; Even pure address formation before the branch prevents sinking the guard.
define i32 @entry_address(i1 %fast) sspstrong {
; IR-LABEL: define i32 @entry_address(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca [2 x i64], align 8
  %address = getelementptr [2 x i64], ptr %local, i64 0, i64 1
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %address)
  ret i32 %r
}

; A call on the prospective early path may access the local too.
define i32 @both_expose(i1 %fast) sspstrong {
; IR-LABEL: define i32 @both_expose(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  %a = tail call i32 @consume(ptr %local)
  ret i32 %a
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}

; Do not bypass the check when the protected path reaches the same return.
define i32 @merged_exit(i1 %fast) sspstrong {
; IR-LABEL: define i32 @merged_exit(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %exit, label %protected
protected:
  %r = call i32 @consume(ptr %local)
  br label %exit
exit:
  ret i32 0
}

; Re-entering the proposed prologue must not reset a corrupted guard.
define i32 @backedge(i1 %fast, i1 %again) sspstrong {
; IR-LABEL: define i32 @backedge(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %local)
  br i1 %again, label %protected, label %exit
exit:
  ret i32 %r
}

; Incoming ABI stack copies are not covered by the alloca-use proof.
define i32 @byval_argument(i1 %fast, ptr byval([8 x i64]) %argument) sspstrong {
; IR-LABEL: define i32 @byval_argument(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  %a = tail call i32 @consume(ptr %argument)
  ret i32 %a
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}

; Side effects before the branch retain entry instrumentation.
define i32 @entry_call(i1 %fast) sspstrong {
; IR-LABEL: define i32 @entry_call(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: call void @side_effect
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  call void @side_effect()
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}

; Do not change EH instrumentation or optnone behavior.
define i32 @personality(i1 %fast) sspstrong personality ptr @__gxx_personality_v0 {
; IR-LABEL: define i32 @personality(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}

define i32 @no_opt(i1 %fast) noinline optnone sspstrong {
; IR-LABEL: define i32 @no_opt(
; IR: entry:
; IR: call void @llvm.stackprotector
; IR: br i1 %fast
entry:
  %local = alloca i64, align 8
  br i1 %fast, label %early, label %protected
early:
  ret i32 0
protected:
  %r = call i32 @consume(ptr %local)
  ret i32 %r
}
