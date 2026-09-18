; RUN: split-file %s %t
; RUN: opt -passes=verify -S < %t/ok.ll | FileCheck %s --check-prefix=OK
; RUN: not opt -passes=verify -S < %t/call.ll 2>&1 | FileCheck %s --check-prefix=CALL
; RUN: not opt -passes=verify -S < %t/two.ll 2>&1 | FileCheck %s --check-prefix=TWO
; RUN: not opt -passes=verify -S < %t/aggregate.ll 2>&1 | FileCheck %s --check-prefix=AGGREGATE
; RUN: not opt -passes=verify -S < %t/personality.ll 2>&1 | FileCheck %s --check-prefix=PERSONALITY
; RUN: not opt -passes=verify -S < %t/funclet.ll 2>&1 | FileCheck %s --check-prefix=FUNCLET

; A fault probe is a callbr with exactly one fault destination, over a
; scalar or vector, in a function whose personality reads an Itanium-style
; exception table, where the destination is recorded as a landing pad.

;--- ok.ll
declare i32 @__gxx_personality_v0(...)
declare i64 @llvm.fault.probe.load.i64.p0(ptr, i32)
declare void @llvm.fault.probe.store.i64.p0(i64, ptr, i32)

; OK-LABEL: @probes(
; OK: callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8)
; OK-NEXT: to label %loaded [label %fault]
; OK: callbr void @llvm.fault.probe.store.i64.p0(i64 %v, ptr %p, i32 8)
; OK-NEXT: to label %stored [label %fault]
define i64 @probes(ptr %p) personality ptr @__gxx_personality_v0 {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %loaded [label %fault]
loaded:
  callbr void @llvm.fault.probe.store.i64.p0(i64 %v, ptr %p, i32 8) to label %stored [label %fault]
stored:
  ret i64 %v
fault:
  ret i64 -1
}

;--- call.ll
declare i32 @__gxx_personality_v0(...)
declare i64 @llvm.fault.probe.load.i64.p0(ptr, i32)

; CALL: fault probe must be a callbr
define i64 @plain(ptr %p) personality ptr @__gxx_personality_v0 {
  %v = call i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8)
  ret i64 %v
}

;--- two.ll
declare i32 @__gxx_personality_v0(...)
declare i64 @llvm.fault.probe.load.i64.p0(ptr, i32)

; TWO: fault probe has exactly one fault destination
define i64 @two(ptr %p) personality ptr @__gxx_personality_v0 {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %ok [label %a, label %b]
ok:
  ret i64 %v
a:
  ret i64 -1
b:
  ret i64 -2
}

;--- aggregate.ll
declare i32 @__gxx_personality_v0(...)
declare { i64, i64 } @llvm.fault.probe.load.sl_i64i64s.p0(ptr, i32)

; AGGREGATE: fault probe accesses a scalar or vector
define i64 @aggregate(ptr %p) personality ptr @__gxx_personality_v0 {
  %v = callbr { i64, i64 } @llvm.fault.probe.load.sl_i64i64s.p0(ptr %p, i32 8) to label %ok [label %fault]
ok:
  %w = extractvalue { i64, i64 } %v, 0
  ret i64 %w
fault:
  ret i64 -1
}

;--- personality.ll
declare i64 @llvm.fault.probe.load.i64.p0(ptr, i32)

; PERSONALITY: fault probe needs a personality function
define i64 @bare(ptr %p) {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %ok [label %fault]
ok:
  ret i64 %v
fault:
  ret i64 -1
}

;--- funclet.ll
declare i32 @__CxxFrameHandler3(...)
declare i64 @llvm.fault.probe.load.i64.p0(ptr, i32)

; FUNCLET: fault probe needs an Itanium-style personality
define i64 @funclet(ptr %p) personality ptr @__CxxFrameHandler3 {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %ok [label %fault]
ok:
  ret i64 %v
fault:
  ret i64 -1
}
