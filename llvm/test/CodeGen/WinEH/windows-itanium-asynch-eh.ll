; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,X64
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,ARM64 %}
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-itanium -O0 < %s | FileCheck %s --check-prefix=O0 %}

; Asynchronous exceptions with a landing-pad personality: the instructions
; between the llvm.seh.try.* and llvm.seh.scope.* markers become call-site
; ranges of the landing pad in effect, so a hardware exception raised by a
; plain store or by a call the compiler took as not throwing has an entry
; in the table. Code outside every scope is covered by entries with no
; landing pad, so that a fault there unwinds to the caller.
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

; The store inside the object's lifetime shares the landing pad of the
; invoke after it. On x86 its range starts with a nop, so that the byte before
; the faulting instruction is inside the range; AArch64 needs none.
; X64-LABEL: f:
; X64:      callq T_ctor
; X64-NEXT: .Ltmp{{[0-9]+}}:
; X64-NEXT: # %bb.2:
; X64-NEXT: [[BODY:.Ltmp[0-9]+]]:
; X64-NEXT: nop
; X64-NEXT: movl $1, (%r{{[a-z0-9]+}})
; X64-NEXT: .Ltmp{{[0-9]+}}:
; X64-NEXT: .Ltmp{{[0-9]+}}:
; X64-NEXT: callq g
; X64-NEXT: [[G_END:.Ltmp[0-9]+]]:
; The destructor call after the scope, still inside the try, gets its own
; range with the catch landing pad; the store after the try gets none.
; X64-NEXT: # %bb.3:
; X64-NEXT: [[DTOR:.Ltmp[0-9]+]]:
; X64-NEXT: leaq
; X64-NEXT: callq T_dtor
; X64-NEXT: xorl
; X64-NEXT: [[DTOR_END:.Ltmp[0-9]+]]:
; X64-NEXT: .LBB0_4:
; X64-NEXT: movl $2, (%r{{[a-z0-9]+}})

; ARM64-LABEL: f:
; ARM64:      bl T_ctor
; ARM64-NEXT: .Ltmp{{[0-9]+}}:
; ARM64-NEXT: // %bb.2:
; ARM64-NEXT: [[BODY:.Ltmp[0-9]+]]:
; ARM64-NEXT: mov w8, #1
; ARM64-NEXT: str w8, [x{{[0-9]+}}]
; ARM64-NEXT: .Ltmp{{[0-9]+}}:
; ARM64-NEXT: .Ltmp{{[0-9]+}}:
; ARM64-NEXT: bl g
; ARM64-NEXT: [[G_END:.Ltmp[0-9]+]]:
; ARM64-NEXT: // %bb.3:
; ARM64-NEXT: [[DTOR:.Ltmp[0-9]+]]:
; ARM64-NEXT: add x0, sp, #31
; ARM64-NEXT: bl T_dtor
; ARM64-NEXT: [[DTOR_END:.Ltmp[0-9]+]]:
; ARM64:      .LBB0_5:
; ARM64-NEXT: mov w8, #2
; ARM64-NEXT: str w8, [x{{[0-9]+}}]

; At -O0 AArch64 selects instructions with GlobalISel, which leaves a function
; with invokes of the markers to SelectionDAG, so the store has its range too.
; O0-LABEL: f:
; O0:      bl T_ctor
; O0:      // %body
; O0-NEXT: [[O0BODY:.Ltmp[0-9]+]]:
; O0-NEXT: ldr x{{[0-9]+}}, [sp, #{{[0-9]+}}]
; O0-NEXT: mov w8, #1
; O0-NEXT: str w8, [x{{[0-9]+}}]
; O0-NEXT: .Ltmp{{[0-9]+}}:
; O0-NEXT: .Ltmp{{[0-9]+}}:
; O0-NEXT: bl g
; O0-NEXT: [[O0G_END:.Ltmp[0-9]+]]:
; O0:      .Lcst_begin0:
; O0:      .uleb128 [[O0BODY]]-.Lfunc_begin0
; O0-NEXT: .uleb128 [[O0G_END]]-[[O0BODY]]
; O0-NEXT: .uleb128 .Ltmp{{[0-9]+}}-.Lfunc_begin0
; O0-NEXT: .byte 1

; The prologue before the first range and the code after the try, up to the
; destructor call in the cleanup pad, have entries with no landing pad. The
; body range and the invoke of g share the cleanup pad and merge; the two
; ranges of the catch pad stay apart, since the entry between them does not.
; CHECK:      .Lcst_begin0:
; CHECK-NEXT: .uleb128 .Lfunc_begin0-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[CTOR:.Ltmp[0-9]+]]-.Lfunc_begin0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .uleb128 [[CTOR]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 .Ltmp{{[0-9]+}}-[[CTOR]]
; CHECK-NEXT: .uleb128 .Ltmp{{[0-9]+}}-.Lfunc_begin0
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 [[BODY]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[G_END]]-[[BODY]]
; CHECK-NEXT: .uleb128 .Ltmp{{[0-9]+}}-.Lfunc_begin0
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 [[DTOR]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[DTOR_END]]-[[DTOR]]
; CHECK-NEXT: .uleb128 [[CATCH:.Ltmp[0-9]+]]-.Lfunc_begin0
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 [[DTOR_END]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[PAD_DTOR:.Ltmp[0-9]+]]-[[DTOR_END]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .uleb128 [[PAD_DTOR]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[PAD_DTOR_END:.Ltmp[0-9]+]]-[[PAD_DTOR]]
; CHECK-NEXT: .uleb128 [[CATCH]]-.Lfunc_begin0
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 [[PAD_DTOR_END]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 .Lfunc_end0-[[PAD_DTOR_END]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .Lcst_end0:

; One scope entered from inside a try block and from outside it: the scope's
; own code has the cleanup pad, but the code after it ends has none, however
; the paths are ordered. The front end does not share a scope like this; the
; function checks that the result does not depend on the order in which the
; blocks are visited.

define void @h(i1 %c, ptr %p) personality ptr @__gxx_personality_seh0 {
entry:
  br i1 %c, label %inner, label %outer

outer:
  invoke void @llvm.seh.try.begin()
          to label %outer.body unwind label %catch.lpad

outer.body:
  invoke void @llvm.seh.scope.begin()
          to label %scoped unwind label %cleanup.lpad

inner:
  invoke void @llvm.seh.scope.begin()
          to label %scoped unwind label %cleanup.lpad

scoped:
  store volatile i32 1, ptr %p, align 4
  invoke void @llvm.seh.scope.end()
          to label %after unwind label %cleanup.lpad

after:
  store volatile i32 2, ptr %p, align 4
  ret void

cleanup.lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  store volatile i32 3, ptr %p, align 4
  resume { ptr, i32 } %lp

catch.lpad:
  %lp0 = landingpad { ptr, i32 }
          catch ptr null
  ret void
}

; X64-LABEL: h:
; X64:      [[SCOPED:.Ltmp[0-9]+]]:
; X64-NEXT: nop
; X64-NEXT: movl $1, (%r{{[a-z0-9]+}})
; X64-NEXT: [[SCOPED_END:.Ltmp[0-9]+]]:
; X64-NOT:  .Ltmp
; X64:      movl $2, (%r{{[a-z0-9]+}})

; ARM64-LABEL: h:
; ARM64:      [[SCOPED:.Ltmp[0-9]+]]:
; ARM64-NEXT: mov [[ONE:w[0-9]+]], #1
; ARM64-NEXT: str [[ONE]], [x{{[0-9]+}}]
; ARM64-NEXT: [[SCOPED_END:.Ltmp[0-9]+]]:
; ARM64-NOT:  .Ltmp
; ARM64:      mov [[TWO:w[0-9]+]], #2
; ARM64-NEXT: str [[TWO]], [x{{[0-9]+}}]

; CHECK:      .Lcst_begin1:
; CHECK-NEXT: .uleb128 .Lfunc_begin1-.Lfunc_begin1
; CHECK-NEXT: .uleb128 [[SCOPED]]-.Lfunc_begin1
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .uleb128 [[SCOPED]]-.Lfunc_begin1
; CHECK-NEXT: .uleb128 [[SCOPED_END]]-[[SCOPED]]
; CHECK-NEXT: .uleb128 .Ltmp{{[0-9]+}}-.Lfunc_begin1
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .uleb128 [[SCOPED_END]]-.Lfunc_begin1
; CHECK-NEXT: .uleb128 .Lfunc_end1-[[SCOPED_END]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .Lcst_end1:
