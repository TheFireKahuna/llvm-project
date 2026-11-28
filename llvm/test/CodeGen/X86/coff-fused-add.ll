; DEFINE: %{triple} = x86_64-unknown-windows-itanium
; DEFINE: %{prefix} = FUSED
; DEFINE: %{run} = llc < %s -mtriple=%{triple} | FileCheck %s --check-prefixes=CHECK,%{prefix}
; RUN: %{run}
; REDEFINE: %{triple} = x86_64-pc-windows-ntposix
; RUN: %{run}
; REDEFINE: %{triple} = x86_64-pc-windows-msvc
; REDEFINE: %{prefix} = PLAIN
; RUN: %{run} --implicit-check-not=FUSED_ADD
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=O0 --implicit-check-not=FUSED_ADD
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=A64,A64P --implicit-check-not=IF_LOCAL %}

; On Windows Itanium and NT-POSIX an optimized function states the constant it
; adds to an import pointer, for the linker to fold into the load: on x86-64 a
; lea directly after the load under a marker naming nothing, on AArch64 an add
; under a marker naming the pointer, whose load is a fused read.

@arr = external dllimport global [8 x i32]
declare void @taking(ptr)

define ptr @offset() {
; CHECK-LABEL: offset:
; CHECK:         movq __imp_arr(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ADD
; FUSED-NEXT:    leaq 8(%rax), %rax
; PLAIN-NEXT:    addq $8, %rax
; O0-LABEL: offset:
; O0:            addq $8, %rax
; A64-LABEL: offset:
; A64F:          ldr x8, [x8, :fused_lo12:__imp_arr]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_arr
; A64P:          ldr x8, [x8, :lo12:__imp_arr]
; A64-NEXT:      add x0, x8, #8
  ret ptr getelementptr ([8 x i32], ptr @arr, i64 0, i64 2)
}

; A negative constant is one on x86-64. AArch64 subtracts, which no page
; offset goes into.
define ptr @before() {
; CHECK-LABEL: before:
; FUSED:         .reloc {{.*}}, IMAGE_REL_LLVM_FUSED_ADD
; FUSED-NEXT:    leaq -4(%rax), %rax
; PLAIN:         addq $-4, %rax
; A64-LABEL: before:
; A64:           ldr x8, [x8, :lo12:__imp_arr]
; A64-NEXT:      sub x0, x8, #4
  ret ptr getelementptr ([8 x i32], ptr @arr, i64 0, i64 -1)
}

; An add whose flags are read stays one.
define i1 @flags() {
; CHECK-LABEL: flags:
; CHECK:         movq __imp_arr(%rip), %rax
; CHECK-NEXT:    addq $8, %rax
  %sum = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 ptrtoint (ptr @arr to i64), i64 8)
  %carry = extractvalue { i64, i1 } %sum, 1
  ret i1 %carry
}

; With another reader of the load the add is none on x86-64, where the load
; could not take the sum. On AArch64 each reader is a site.
define i32 @shared() {
; CHECK-LABEL: shared:
; CHECK:         movq __imp_arr(%rip), [[REG:%r[a-z]+]]
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_arr
; CHECK-NEXT:    movl 4([[REG]]), %e{{[a-z]+}}
; FUSED-NEXT:    leaq 8([[REG]]), %r{{[a-z]+}}
; PLAIN-NEXT:    addq $8, [[REG]]
; A64-LABEL: shared:
; A64F:          ldr x8, [x8, :fused_lo12:__imp_arr]
; A64F:          .reloc {{.*}}, IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_arr
; A64F:          .reloc {{.*}}, IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_arr
  %x = load i32, ptr getelementptr ([8 x i32], ptr @arr, i64 0, i64 1)
  call void @taking(ptr getelementptr ([8 x i32], ptr @arr, i64 0, i64 2))
  ret i32 %x
}

; A sum that is read on both sides of a call is formed again behind it, in a
; cleanup as elsewhere, by a load and a lea that the linker makes one lea, and
; no callee-saved register holds it. AArch64, where the linker leaves two
; instructions of three, holds it.
@chosen = external global [8 x i32]
declare void @throwing(ptr)
declare i32 @__gxx_personality_v0(...)

define void @across(ptr %o) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: across:
; FUSED:         pushq %r[[THIS:[a-z]+]]
; FUSED-NOT:     pushq
; FUSED:         leaq 8(%rax), %rax
; FUSED-NEXT:    movq %rax, (%r{{cx|di}})
; FUSED:         callq throwing
; FUSED:         movq __imp_chosen(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ADD
; FUSED-NEXT:    leaq 8(%rax), %rax
; FUSED-NEXT:    movq %rax, (%r[[THIS]])
; FUSED:         retq
; FUSED:         movq __imp_chosen(%rip), %rcx
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ADD
; FUSED-NEXT:    leaq 8(%rcx), %rcx
; FUSED-NEXT:    movq %rcx, (%r[[THIS]])
; A64-LABEL: across:
; A64F:          add x[[SUM:19|20]], x{{[0-9]+}}, #8
; A64F:          bl throwing
; A64F:          str x[[SUM]], [x
entry:
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  invoke void @throwing(ptr %o) to label %done unwind label %cleanup
done:
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  ret void
cleanup:
  %caught = landingpad { ptr, i32 } cleanup
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  resume { ptr, i32 } %caught
}

; Formed again, the sum is an instruction at each use, so where it is read in
; a loop that contains a call, or behind calls more than twice on one path, a
; register holds it as on other targets.
define void @repeated(ptr %o, i32 %n) {
; CHECK-LABEL: repeated:
; FUSED:         movq __imp_chosen(%rip), %r[[SUM:bx|si|di|bp|1[2-5]]]
; FUSED:         leaq 8(%r[[SUM]]), %r[[SUM]]
; FUSED:       .LBB{{.*}}:
; FUSED-NOT:     __imp_chosen
; FUSED:         callq throwing
; FUSED:         j{{.*}} .LBB
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  call void @throwing(ptr %o)
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret void
}

define void @thrice(ptr %o) {
; CHECK-LABEL: thrice:
; FUSED:         movq __imp_chosen(%rip), %r[[SUM:bx|si|di|bp|1[2-5]]]
; FUSED:         leaq 8(%r[[SUM]]), %r[[SUM]]
; FUSED-NOT:     __imp_chosen
; FUSED:         retq
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  call void @throwing(ptr %o)
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  call void @throwing(ptr %o)
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  call void @throwing(ptr %o)
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  ret void
}

; Paths that each read it once behind the call form it again.
define void @either(ptr %o, i32 %x) {
; CHECK-LABEL: either:
; FUSED:         callq throwing
; FUSED-NEXT:    movq __imp_chosen(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ADD
; FUSED-NEXT:    leaq 8(%rax), %rax
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  call void @throwing(ptr %o)
  switch i32 %x, label %c [ i32 0, label %a
                            i32 1, label %b ]
a:
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o
  ret void
b:
  %o1 = getelementptr ptr, ptr %o, i64 1
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o1
  ret void
c:
  %o2 = getelementptr ptr, ptr %o, i64 2
  store ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2), ptr %o2
  ret void
}
