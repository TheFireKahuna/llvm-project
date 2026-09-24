; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix -fast-isel=false < %s | FileCheck %s

; An invoke of llvm.fault.* is the memory access itself between the invoke's
; labels: its call-site range names the landing pad, and the pad is a
; successor of the block, so a value the pad needs is live across the access.
; A call of one that may unwind is the access between labels of its own,
; whose site names no pad, so a search passes the frame there; in a function
; that cannot unwind the same labels describe nothing, a gap. A call that
; cannot unwind is the plain access.

declare void @cleanup_effect() nounwind
declare i32 @rust_eh_personality(...)
declare i64 @llvm.fault.load.i64.p0(ptr, i32)
declare i64 @llvm.fault.load.volatile.i64.p0(ptr, i32)
declare void @llvm.fault.store.i64.p0(i64, ptr, i32)
declare void @llvm.fault.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; CHECK-LABEL: load:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK-NOT: fault.load
; CHECK: [[BEGIN:.Ltmp[0-9]+]]:
; CHECK: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; CHECK: [[END:.Ltmp[0-9]+]]:
; CHECK: [[PAD:.Ltmp[0-9]+]]:
; CHECK: callq cleanup_effect
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

; Two accesses under one pad are one table entry: the ranges are adjacent and
; name the same pad, so the table merges them.
; CHECK-LABEL: pair:
; CHECK: [[B1:.Ltmp[0-9]+]]:
; CHECK: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; CHECK: movq %{{[a-z0-9]+}}, (%{{[a-z0-9]+}})
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

; A copy is the memcpy call inside the labels.
; CHECK-LABEL: copy:
; CHECK: [[CB:.Ltmp[0-9]+]]:
; CHECK: callq memcpy
; CHECK: [[CE:.Ltmp[0-9]+]]:
; CHECK: GCC_except_table
; CHECK: .uleb128 [[CB]]-[[CFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[CE]]-[[CB]]
define void @copy(ptr %dst, ptr %src, i64 %len) personality ptr @rust_eh_personality {
entry:
  invoke void @llvm.fault.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 %len, i1 false) to label %normal unwind label %exception
normal:
  ret void
exception:
  %value = landingpad { ptr, i32 } cleanup
  call void @cleanup_effect()
  resume { ptr, i32 } %value
}

; A call that cannot unwind is the plain access: no labels, no table.
; CHECK-LABEL: plain:
; CHECK-NOT: .Ltmp
; CHECK: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; CHECK-NOT: GCC_except_table
; CHECK: retq
define i64 @plain(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = call i64 @llvm.fault.load.volatile.i64.p0(ptr %p, i32 8) nounwind
  ret i64 %v
}

; A call that may unwind: the access between its own labels, a site with no
; pad and no action, and no funclet.
; CHECK-LABEL: to_caller:
; CHECK: [[TB:.Ltmp[0-9]+]]:
; CHECK-NEXT: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; CHECK-NEXT: [[TE:.Ltmp[0-9]+]]:
; CHECK: .seh_handlerdata
; CHECK: .uleb128 [[CE:.Lcleanup_end[0-9]+]]-[[CB:.Lcleanup_begin[0-9]+]]
; CHECK-NEXT: [[CB]]:
; CHECK-NEXT: [[CE]]:
; CHECK: .uleb128 [[TB]]-[[TFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[TE]]-[[TB]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .Lcst_end
define i64 @to_caller(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = call i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
  ret i64 %v
}

; The same call in a function that cannot unwind: the table is there and
; names no site, so the access is a gap.
; CHECK-LABEL: sealed_access:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}}
; CHECK: .Lcst_begin{{[0-9]+}}:
; CHECK-NEXT: .Lcst_end{{[0-9]+}}:
define i64 @sealed_access(ptr %p) nounwind personality ptr @rust_eh_personality {
entry:
  %v = call i64 @llvm.fault.load.i64.p0(ptr %p, i32 8)
  ret i64 %v
}

; A function that cannot unwind and calls nothing still has its table.
; CHECK-LABEL: sealed:
; CHECK: .seh_handler rust_eh_personality, @unwind, @except
; CHECK: .Lcst_begin{{[0-9]+}}:
; CHECK-NEXT: .Lcst_end{{[0-9]+}}:
define void @sealed() nounwind personality ptr @rust_eh_personality {
entry:
  ret void
}

; An asm window that may unwind, outside every invoke: a site of its own.
; CHECK-LABEL: asm_to_caller:
; CHECK: [[AB:.Ltmp[0-9]+]]:
; CHECK-NEXT: #APP
; CHECK: #NO_APP
; CHECK-NEXT: [[AE:.Ltmp[0-9]+]]:
; CHECK: .uleb128 [[AB]]-[[AFUNC:.Lfunc_begin[0-9]+]]
; CHECK-NEXT: .uleb128 [[AE]]-[[AB]]
; CHECK-NEXT: .byte 0
; CHECK-NEXT: .byte 0
define i64 @asm_to_caller(ptr %p) personality ptr @rust_eh_personality {
entry:
  %v = call i64 asm sideeffect unwind "movq ($1), $0", "=r,r,~{memory}"(ptr %p)
  ret i64 %v
}
