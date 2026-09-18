; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefixes=CHECK,COFF
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-msvc < %s | FileCheck %s --check-prefixes=CHECK,COFF
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-msvc -fast-isel=false < %s | FileCheck %s --check-prefixes=CHECK,COFF
; RUN: llc -O2 -verify-machineinstrs -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s --check-prefixes=CHECK,ELF

; A probing access is the load or store itself, wrapped in a faulting op whose
; handler is the callbr's fault destination. The destination is a landing pad
; of the one-instruction call site around the access, so the function's
; exception table pairs the access with it; the pad's one catch clause names
; the function itself, which tells the site from an invoke's; and under EH
; continuation guard the destination is a continuation target. A pointer known to be
; dereferenceable is accessed plainly and records nothing.

declare i32 @__gxx_personality_v0(...)
declare i64 @llvm.fault.probe.load.i64.p0(ptr, i32)
declare void @llvm.fault.probe.store.i64.p0(i64, ptr, i32)

; CHECK-LABEL: load_probe:
; COFF: .seh_handler __gxx_personality_v0, @unwind, @except
; CHECK: [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: .Ltmp{{[0-9]+}}:
; CHECK-NEXT: movq (%{{[a-z0-9]+}}), %{{[a-z0-9]+}} # on-fault: [[HANDLER:.LBB0_[0-9]+]]
; CHECK-NEXT: [[END:.Ltmp[0-9]+]]:
; CHECK: [[HANDLER]]:
; COFF: $ehgcr_0_{{[0-9]+}}:
; CHECK: [[PAD:.Ltmp[0-9]+]]:
; CHECK: movq $-1, %rax
; CHECK: GCC_except_table0:
; CHECK: .Lexception0:
; CHECK-NEXT: .byte 255
; COFF-NEXT: .byte 0
; ELF-NEXT: .byte 3
; CHECK-NEXT: .uleb128 .Lttbase0-.Lttbaseref0
; CHECK-NEXT: .Lttbaseref0:
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 .Lcst_end0-.Lcst_begin0
; CHECK-NEXT: .Lcst_begin0:
; CHECK-NEXT: .uleb128 [[BEGIN]]-.Lfunc_begin0
; CHECK-NEXT: .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT: .uleb128 [[PAD]]-.Lfunc_begin0
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .Lcst_end0:
; CHECK-NEXT: .byte 1
; CHECK: .byte 0
; COFF: .quad load_probe
; ELF: .long load_probe
; CHECK: .Lttbase0:
define i64 @load_probe(ptr %p) personality ptr @__gxx_personality_v0 {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %ok [label %fault]
ok:
  ret i64 %v
fault:
  ret i64 -1
}

; CHECK-LABEL: store_probe:
; CHECK: [[BEGIN:.Ltmp[0-9]+]]:
; CHECK-NEXT: .Ltmp{{[0-9]+}}:
; CHECK-NEXT: movq %{{[a-z0-9]+}}, (%{{[a-z0-9]+}}) # on-fault: [[HANDLER:.LBB1_[0-9]+]]
; CHECK-NEXT: [[END:.Ltmp[0-9]+]]:
; CHECK: [[HANDLER]]:
; COFF: $ehgcr_1_{{[0-9]+}}:
; CHECK: [[PAD:.Ltmp[0-9]+]]:
; CHECK: GCC_except_table1:
; CHECK: .uleb128 [[BEGIN]]-.Lfunc_begin1
; CHECK-NEXT: .uleb128 [[END]]-[[BEGIN]]
; CHECK-NEXT: .uleb128 [[PAD]]-.Lfunc_begin1
; CHECK-NEXT: .byte 1
; COFF: .quad store_probe
; ELF: .long store_probe
define i32 @store_probe(ptr %p, i64 %v) personality ptr @__gxx_personality_v0 {
  callbr void @llvm.fault.probe.store.i64.p0(i64 %v, ptr %p, i32 8) to label %ok [label %fault]
ok:
  ret i32 1
fault:
  ret i32 0
}

; CHECK-LABEL: safe_probe:
; CHECK-NOT: on-fault
; CHECK-NOT: $ehgcr_2
; CHECK: movq (%{{[a-z0-9]+}}), %rax
; CHECK-NOT: on-fault
; CHECK-NOT: $ehgcr_2
; CHECK-NOT: GCC_except_table2
; CHECK: ret
define i64 @safe_probe(ptr dereferenceable(8) %p) personality ptr @__gxx_personality_v0 {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %ok [label %fault]
ok:
  ret i64 %v
fault:
  ret i64 -1
}

; Two probes sharing one fault destination are two call sites, one
; instruction each, never one range over the code between them.
; CHECK-LABEL: two_probes:
; CHECK: [[BEGIN1:.Ltmp[0-9]+]]:
; CHECK-NEXT: .Ltmp{{[0-9]+}}:
; CHECK-NEXT: {{movq|ldr}} {{.*}} on-fault: [[HANDLER:.LBB3_[0-9]+]]
; CHECK-NEXT: [[END1:.Ltmp[0-9]+]]:
; CHECK: [[BEGIN2:.Ltmp[0-9]+]]:
; CHECK-NEXT: .Ltmp{{[0-9]+}}:
; CHECK-NEXT: {{movq|ldr}} {{.*}} on-fault: [[HANDLER]]
; CHECK-NEXT: [[END2:.Ltmp[0-9]+]]:
; CHECK: GCC_except_table3:
; CHECK: .uleb128 [[BEGIN1]]-.Lfunc_begin2
; CHECK-NEXT: .uleb128 [[END1]]-[[BEGIN1]]
; CHECK-NEXT: .uleb128 [[PAD:.Ltmp[0-9]+]]-.Lfunc_begin2
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .uleb128 [[BEGIN2]]-.Lfunc_begin2
; CHECK-NEXT: .uleb128 [[END2]]-[[BEGIN2]]
; CHECK-NEXT: .uleb128 [[PAD]]-.Lfunc_begin2
; CHECK-NEXT: .byte 1
; CHECK-NEXT: .Lcst_end2:
define i64 @two_probes(ptr %p, ptr %q) personality ptr @__gxx_personality_v0 {
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %second [label %fault]
second:
  %w = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %q, i32 8) to label %ok [label %fault]
ok:
  %s = add i64 %v, %w
  ret i64 %s
fault:
  ret i64 -1
}

; A fault destination that normal control flow reaches as well is laid out
; as an ordinary block: the branch into it from the other path survives.
; CHECK-LABEL: shared_dest:
; CHECK-NOT: ud2
; CHECK-DAG: on-fault: .LBB4_{{[0-9]+}}
; CHECK-DAG: callq sink
; CHECK-NOT: ud2
; CHECK: GCC_except_table4:
define i64 @shared_dest(ptr %p, i1 %skip) personality ptr @__gxx_personality_v0 {
  br i1 %skip, label %fault, label %probe
probe:
  %v = callbr i64 @llvm.fault.probe.load.i64.p0(ptr %p, i32 8) to label %ok [label %fault]
ok:
  ret i64 %v
fault:
  %r = call i64 @sink()
  ret i64 %r
}
declare i64 @sink()

; The continuation table names the four handlers; the safe one is in neither
; it nor any exception table. COFF carries no fault map: the exception
; tables hold the pairs. ELF keeps the fault map beside them.
; COFF: .section .gehcont$y
; COFF-NEXT: .symidx $ehgcr_0_{{[0-9]+}}
; COFF-NEXT: .symidx $ehgcr_1_{{[0-9]+}}
; COFF-NEXT: .symidx $ehgcr_3_{{[0-9]+}}
; COFF-NEXT: .symidx $ehgcr_4_{{[0-9]+}}
; COFF-NOT: .symidx
; COFF-NOT: .llvm_faultmaps
; ELF-NOT: gehcont
; ELF: .section .llvm_faultmaps,"a",@progbits
; ELF: __LLVM_FaultMaps:
; ELF-NEXT: .byte 1
; ELF-NEXT: .byte 0
; ELF-NEXT: .short 0
; ELF-NEXT: .long 4
; ELF-NEXT: .quad load_probe
; ELF-NEXT: .long 1
; ELF-NEXT: .long 0
; ELF-NEXT: .long 1
; ELF-NEXT: .long .Ltmp{{[0-9]+}}-load_probe
; ELF-NEXT: .long .LBB0_{{[0-9]+}}-load_probe
; ELF-DAG: .quad shared_dest
; ELF-DAG: .quad store_probe
; ELF-DAG: .quad two_probes
; ELF-NOT: safe_probe

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"ehcontguard", i32 1}
