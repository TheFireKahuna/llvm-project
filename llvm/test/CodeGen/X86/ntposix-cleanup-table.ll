; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-itanium < %s | FileCheck %s --check-prefix=ITANIUM

; On NT-POSIX every function's handler data begins with the cleanup table,
; empty here, whose header alone measures it, so a personality is handed the
; LSDA past it; an Itanium triple without the property emits the LSDA alone.

declare void @may_throw()
declare void @drop_a() nounwind
declare i32 @rust_eh_personality(...)

; NTPOSIX-LABEL: plain:
; NTPOSIX: .seh_handlerdata
; NTPOSIX: .section .xdata
; NTPOSIX-NEXT: .p2align 2
; NTPOSIX-NEXT: .byte 1
; NTPOSIX-NEXT: .uleb128 [[END:.Lcleanup_end[0-9]+]]-[[BEGIN:.Lcleanup_begin[0-9]+]]
; NTPOSIX-NEXT: [[BEGIN]]:
; NTPOSIX-NEXT: [[END]]:
; NTPOSIX-NEXT: GCC_except_table0:
; ITANIUM-LABEL: plain:
; ITANIUM: .seh_handlerdata
; ITANIUM: .section .xdata
; ITANIUM-NEXT: .p2align 2
; ITANIUM-NEXT: GCC_except_table0:
define void @plain() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %done unwind label %pad
done:
  ret void
pad:
  %e = landingpad { ptr, i32 } cleanup
  call void @drop_a()
  resume { ptr, i32 } %e
}

; A call outside every invoke's range whose callee may unwind gets, on
; NT-POSIX, an entry with no landing pad spanning that one instruction, and
; the rest of the region between the invokes stays undescribed; a call to a
; nounwind callee gets nothing. An Itanium triple writes one entry with no
; landing pad for the whole region.
declare void @plain_call()
declare void @quiet_call() nounwind

; NTPOSIX-LABEL: gap:
; NTPOSIX: [[PB:.Lplaincall_begin[0-9]+]]:
; NTPOSIX-NEXT: callq plain_call
; NTPOSIX-NEXT: [[PE:.Lplaincall_end[0-9]+]]:
; NTPOSIX-NEXT: callq quiet_call
; NTPOSIX-NEXT: .Ltmp{{[0-9]+}}:
; NTPOSIX: GCC_except_table1:
; NTPOSIX: .Lcst_begin1:
; NTPOSIX-NEXT: .uleb128 [[G1:.Ltmp[0-9]+]]-.Lfunc_begin1
; NTPOSIX-NEXT: .uleb128 {{.*}}-[[G1]]
; NTPOSIX-NEXT: .uleb128 {{.*}}-.Lfunc_begin1
; NTPOSIX-NEXT: .byte 0
; NTPOSIX-NEXT: .uleb128 [[PB]]-.Lfunc_begin1
; NTPOSIX-NEXT: .uleb128 [[PE]]-[[PB]]
; NTPOSIX-NEXT: .byte 0
; NTPOSIX-NEXT: .byte 0
; NTPOSIX-NEXT: .uleb128 [[G2:.Ltmp[0-9]+]]-.Lfunc_begin1
; NTPOSIX-NEXT: .uleb128 {{.*}}-[[G2]]
; NTPOSIX-NEXT: .uleb128 {{.*}}-.Lfunc_begin1
; NTPOSIX-NEXT: .byte 0
; NTPOSIX: .Lcst_end1:
; ITANIUM-LABEL: gap:
; ITANIUM-NOT: .Lplaincall_begin
; ITANIUM: .Lcst_begin1:
; ITANIUM: .byte 0
; ITANIUM: .byte 0
; ITANIUM: .byte 0
; ITANIUM: .Lcst_end1:
define void @gap() personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %mid unwind label %pad
mid:
  call void @plain_call()
  call void @quiet_call()
  invoke void @may_throw() to label %done unwind label %pad
done:
  ret void
pad:
  %e = landingpad { ptr, i32 } cleanup
  call void @drop_a()
  resume { ptr, i32 } %e
}

; In a nounwind function the same call gets no entry: an unwind reaching it
; ends at the gap under every class, the answer of a body that cannot unwind.
; NTPOSIX-LABEL: sealed:
; NTPOSIX-NOT: .Lplaincall_begin
; NTPOSIX: callq plain_call
; NTPOSIX: .Lcst_begin{{[0-9]+}}:
; NTPOSIX-NEXT: .uleb128 [[S1:.Ltmp[0-9]+]]-.Lfunc_begin{{[0-9]+}}
; NTPOSIX-NEXT: .uleb128 {{.*}}-[[S1]]
; NTPOSIX-NEXT: .uleb128 {{.*}}-.Lfunc_begin{{[0-9]+}}
; NTPOSIX-NEXT: .byte 0
; NTPOSIX-NEXT: .uleb128 [[S2:.Ltmp[0-9]+]]-.Lfunc_begin{{[0-9]+}}
; NTPOSIX: .Lcst_end{{[0-9]+}}:
define void @sealed() nounwind personality ptr @rust_eh_personality {
entry:
  invoke void @may_throw() to label %mid unwind label %pad
mid:
  call void @plain_call()
  invoke void @may_throw() to label %done unwind label %pad
done:
  ret void
pad:
  %e = landingpad { ptr, i32 } cleanup
  call void @drop_a()
  resume { ptr, i32 } %e
}
