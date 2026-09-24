; RUN: llc -O2 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=aarch64-pc-windows-ntposix -global-isel=false -fast-isel=false < %s | FileCheck %s
; RUN: llc -O2 -mtriple=aarch64-pc-windows-ntposix -filetype=obj < %s | llvm-readobj --unwind - | FileCheck %s --check-prefix=UNWIND

; An invoke of llvm.fault.* is the memory access itself between the invoke's
; labels: its call-site range names the landing pad, and the pad is a
; successor of the block.

declare void @cleanup_effect() nounwind
declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)

; CHECK-LABEL: load:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: ldr x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK-NEXT: [[END:.Ltmp[0-9]+]]:
; CHECK: [[PAD:.Ltmp[0-9]+]]:
; CHECK: bl cleanup_effect
; CHECK: GCC_except_table
; CHECK: .uleb128 [[BEGIN]]-[[FUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT: .uleb128 [[PAD]]-[[FUNC]]
define i64 @load(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; CHECK-LABEL: pair:
; CHECK: [[B1:.Ltmp[0-9]+]]:
; CHECK-NEXT: ldr x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK: str x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK-NEXT: [[E2:.Ltmp[0-9]+]]:
; CHECK: [[PPAD:.Ltmp[0-9]+]]:
; CHECK: GCC_except_table
; CHECK: .uleb128 [[B1]]-[[PFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[E2]]-[[B1]]
; CHECK-NEXT: .uleb128 [[PPAD]]-[[PFUNC]]
define void @pair(ptr %p, ptr %q) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %store unwind label %exception
store:
  invoke void @llvm.fault.store.i64.p0(i64 %v, ptr %q, i32 8) to label %normal unwind label %exception
normal:
  ret void
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; A frameless leaf whose only pad traps still gets an unwind record naming
; its handler: without one the pad is a continuation target in no described
; function.
; CHECK-LABEL: trap_leaf:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: brk #0x1
; UNWIND: Function: trap_leaf
; UNWIND: ByteCodeLength: 4
; UNWIND: 0xe4 ; end
; UNWIND: ExceptionHandler [
; UNWIND-NEXT: Routine: rust_eh_personality
define i64 @trap_leaf(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = invoke i64 @llvm.fault.load.i64.p0(ptr %p, i32 8) to label %normal unwind label %exception
normal:
  ret i64 %v
exception:
  %value = landingpad { ptr, i32 } filter [0 x ptr] zeroinitializer
  call void @llvm.trap()
  unreachable
}

declare void @llvm.trap()

; A call that may unwind: the access between labels of its own, a site with
; no pad, in a frameless leaf that still declares its handler.
; CHECK-LABEL: to_caller:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: [[TB:.Ltmp[0-9]+]]:
; CHECK-NEXT: ldr x0, [x0]
; CHECK-NEXT: [[TE:.Ltmp[0-9]+]]:
; CHECK: .uleb128 [[TB]]-.Lfunc_begin{{[0-9]+}}
; CHECK-NEXT: .uleb128 [[TE]]-[[TB]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; UNWIND-LABEL: Function: to_caller
; UNWIND: ExceptionHandler [
define i64 @to_caller(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = call i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
  ret i64 %v
}

; A function that cannot unwind and calls nothing keeps its table.
; CHECK-LABEL: sealed:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; UNWIND-LABEL: Function: sealed
; UNWIND: ExceptionHandler [
define void @sealed() nounwind personality ptr @rust_eh_personality {
entry:
  ret void
}

; An asm window that may unwind, outside every invoke: a site of its own.
; CHECK-LABEL: asm_to_caller:
; CHECK: [[AB:.Ltmp[0-9]+]]:
; CHECK: [[AE:.Ltmp[0-9]+]]:
; CHECK: .uleb128 [[AB]]-.Lfunc_begin{{[0-9]+}}
; CHECK-NEXT: .uleb128 [[AE]]-[[AB]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
define i64 @asm_to_caller(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = call i64 asm sideeffect unwind "ldr $0, [$1]", "=r,r,~{memory}"(ptr %p)
  ret i64 %v
}
