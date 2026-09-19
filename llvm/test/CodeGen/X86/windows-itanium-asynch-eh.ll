; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s

; Asynchronous exceptions with a landing-pad personality: the instructions
; between the llvm.seh.try.* and llvm.seh.scope.* markers become call-site
; ranges of the landing pad in effect, so a hardware exception raised by a
; plain store or by a call the compiler took as not throwing has an entry
; in the table.
;
;   int f(int *p, int *q) {
;     int r = 0;
;     try { T t; *p = 1; g(); } catch (...) { r = 1; }
;     *q = 2;
;     return r;
;   }

%struct.T = type { i8 }

define i32 @f(ptr %p, ptr %q) personality ptr @__gxx_personality_seh0 {
entry:
  %t = alloca %struct.T, align 1
  invoke void @llvm.seh.try.begin()
          to label %try unwind label %catch.lpad

try:
  invoke void @T_ctor(ptr %t)
          to label %scope unwind label %ctor.lpad

scope:
  invoke void @llvm.seh.scope.begin()
          to label %body unwind label %cleanup.lpad

body:
  store i32 1, ptr %p, align 4
  invoke void @g()
          to label %body.cont unwind label %cleanup.lpad

body.cont:
  invoke void @llvm.seh.scope.end()
          to label %scope.end unwind label %cleanup.lpad

scope.end:
  call void @T_dtor(ptr %t)
  invoke void @llvm.seh.try.end()
          to label %exit unwind label %catch.lpad

catch.lpad:
  %lp0 = landingpad { ptr, i32 }
          catch ptr null
  br label %catch

ctor.lpad:
  %lp1 = landingpad { ptr, i32 }
          catch ptr null
  br label %catch

cleanup.lpad:
  %lp2 = landingpad { ptr, i32 }
          catch ptr null
  call void @T_dtor(ptr %t)
  br label %catch

catch:
  %lp = phi { ptr, i32 } [ %lp0, %catch.lpad ], [ %lp1, %ctor.lpad ], [ %lp2, %cleanup.lpad ]
  %exn = extractvalue { ptr, i32 } %lp, 0
  %obj = call ptr @__cxa_begin_catch(ptr %exn)
  call void @__cxa_end_catch()
  br label %exit

exit:
  %r = phi i32 [ 0, %scope.end ], [ 1, %catch ]
  store i32 2, ptr %q, align 4
  ret i32 %r
}

declare void @llvm.seh.try.begin()
declare void @llvm.seh.try.end()
declare void @llvm.seh.scope.begin()
declare void @llvm.seh.scope.end()
declare i32 @__gxx_personality_seh0(...)
declare void @T_ctor(ptr)
declare void @T_dtor(ptr)
declare void @g()
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"eh-asynch", i32 1}

; The store inside the object's lifetime is preceded by a label and a nop, so
; that the byte before the faulting instruction is inside the range, and its
; range shares the invoke's landing pad.
; CHECK-LABEL: f:
; CHECK:      callq T_ctor
; CHECK-NEXT: .Ltmp{{[0-9]+}}:
; CHECK-NEXT: # %bb.2:
; CHECK-NEXT: [[BODY:.Ltmp[0-9]+]]:
; CHECK-NEXT: nop
; CHECK-NEXT: movl $1, (%r{{[a-z0-9]+}})
; CHECK-NEXT: [[BODY_END:.Ltmp[0-9]+]]:
; CHECK-NEXT: .Ltmp{{[0-9]+}}:
; CHECK-NEXT: callq g
; CHECK-NEXT: [[G_END:.Ltmp[0-9]+]]:
; The destructor call after the scope, still inside the try, gets its own
; range with the catch landing pad; the store after the try gets none.
; CHECK-NEXT: # %bb.3:
; CHECK-NEXT: [[DTOR:.Ltmp[0-9]+]]:
; CHECK-NEXT: leaq
; CHECK-NEXT: callq T_dtor
; CHECK-NEXT: xorl
; CHECK-NEXT: [[DTOR_END:.Ltmp[0-9]+]]:
; CHECK-NEXT: .LBB0_4:
; CHECK-NEXT: movl $2, (%r{{[a-z0-9]+}})

; The body range and the invoke of g share the cleanup pad and merge; the
; destructor range stands alone with the catch pad.
; CHECK:      GCC_except_table0:
; CHECK:      .uleb128 [[BODY]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[G_END]]-[[BODY]]
; CHECK-NEXT: .uleb128 [[CLEANUP:.Ltmp[0-9]+]]-.Lfunc_begin0
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 [[DTOR]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[DTOR_END]]-[[DTOR]]
; CHECK-NEXT: .uleb128 [[CATCH:.Ltmp[0-9]+]]-.Lfunc_begin0
; CHECK-NEXT: .byte 1
