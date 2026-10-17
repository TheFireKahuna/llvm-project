; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -enable-new-pm | \
; RUN:   FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -verify-machineinstrs \
; RUN:   -o /dev/null
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=NONE
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefix=NONE

; A load of the import pointer of data whose register only the memory
; operands of instructions read, as their base with no index, is described
; with each of them, so that the linker can make each access RIP-relative and
; the load dead where the data is in the image. One whose register is next
; given a constant is described with the add, which the linker can fold into
; the load there.

; NONE-NOT: .linkfused

$iv = comdat any
@iv = linkonce_odr global [8 x i32] zeroinitializer, comdat, align 4
@ext = external global i32
@slot = external global ptr
@arr = external global [64 x float]
declare void @f()
declare dllimport void @imp()

; Every reader of the pointer's register accesses data through it, so each is
; described with the load, which the linker may then dispose of.
define i32 @access() {
; CHECK-LABEL: access:
; CHECK:       [[L0:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movq __imp_ext(%rip), [[R0:%[a-z0-9]+]]
; CHECK-NEXT:    .linkfusedaccess [[L0]], [[R0]]
; CHECK-NEXT:    movl ([[R0]]), %eax
; CHECK-NEXT:    .linkfusedaccess [[L0]], [[R0]]
; CHECK-NEXT:    addl 4([[R0]]), %eax
  %a = load i32, ptr @ext
  %p = getelementptr i8, ptr @ext, i64 4
  %b = load i32, ptr %p
  %s = add i32 %a, %b
  ret i32 %s
}

; The register next receives a constant, which the linker may fold into the
; load.
define ptr @add() {
; CHECK-LABEL: add:
; CHECK:       [[L1:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_iv(%rip), %rax
; CHECK-NEXT:    .linkfusedadd [[L1]], %rax
; CHECK-NEXT:    addq $12, %rax
  ret ptr getelementptr (i8, ptr @iv, i64 12)
}

; A load hoisted out of a loop serves the access in it.
define void @loop(i32 %n) {
; CHECK-LABEL: loop:
; CHECK:       [[L2:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movq __imp_iv(%rip), [[R2:%[a-z0-9]+]]
; CHECK:         .linkfusedaccess [[L2]], [[R2]]
; CHECK-NEXT:    addl %eax, ([[R2]])
entry:
  br label %l
l:
  %i = phi i32 [0, %entry], [%i1, %l]
  %v = load i32, ptr @iv
  %v1 = add i32 %v, %i
  store i32 %v1, ptr @iv
  %i1 = add i32 %i, 1
  %c = icmp ult i32 %i1, %n
  br i1 %c, label %l, label %e
e:
  ret void
}

; A register stored as a value is no base: the address of iv stays loaded.
define void @storeaddr() {
; CHECK-LABEL: storeaddr:
; CHECK-NOT:     .linkfused
; CHECK:         movq __imp_iv(%rip), %rax
; CHECK:         .linkfusedaccess {{.*}}, %rcx
; CHECK-NEXT:    movq %rax, (%rcx)
  store ptr @iv, ptr @slot
  ret void
}

; An access with an index is not described, since only an index-free
; operand has a RIP-relative form; one alone keeps the load.
define i32 @indexed(i64 %i) {
; CHECK-LABEL: indexed:
; CHECK-NOT:     .linkfused
; CHECK:         retq
  %p = getelementptr [8 x i32], ptr @iv, i64 0, i64 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; VEX and EVEX accesses are described too; EVEX scales an 8-bit displacement.
define <8 x float> @vex() "target-features"="+avx" {
; CHECK-LABEL: vex:
; CHECK:       [[L4:\.Lfused[0-9]+]]:
; CHECK-NEXT:    .linkfusedread
; CHECK-NEXT:    movq __imp_arr(%rip), %rax
; CHECK-NEXT:    .linkfusedaccess [[L4]], %rax
; CHECK-NEXT:    vmovaps (%rax), %ymm0
  %v = load <8 x float>, ptr @arr
  ret <8 x float> %v
}

define <16 x float> @evex() "target-features"="+avx512f" {
; CHECK-LABEL: evex:
; CHECK:         .linkfusedaccess {{.*}}, %rax
; CHECK-NEXT:    vmovaps 128(%rax), %zmm0
  %p = getelementptr i8, ptr @arr, i64 128
  %v = load <16 x float>, ptr %p
  ret <16 x float> %v
}

; An access next to the load is described though another use keeps the load,
; which is then not described as one only described instructions read.
define float @partial(i64 %i) {
; CHECK-LABEL: partial:
; CHECK:       [[L5:\.Lfused[0-9]+]]:
; CHECK-NEXT:    movq __imp_arr(%rip), %rax
; CHECK-NEXT:    .linkfusedaccess [[L5]], %rax
; CHECK-NEXT:    movss (%rax), %xmm0
; CHECK-NEXT:    addss (%rax,%{{[a-z0-9]+}},4), %xmm0
  %a = load float, ptr @arr
  %p = getelementptr [64 x float], ptr @arr, i64 0, i64 %i
  %b = load float, ptr %p
  %s = fadd float %a, %b
  ret float %s
}

; A use away from the load that keeps it is not described: the access would
; only grow.
define float @partialloop(i64 %i, i32 %n) {
; CHECK-LABEL: partialloop:
; CHECK-NOT:     .linkfused
; CHECK:         retq
entry:
  %p = getelementptr [64 x float], ptr @arr, i64 0, i64 %i
  %b = load float, ptr %p
  br label %l
l:
  %k = phi i32 [0, %entry], [%k1, %l]
  %acc = phi float [%b, %entry], [%acc1, %l]
  %a = load volatile float, ptr @arr
  %acc1 = fadd float %acc, %a
  %k1 = add i32 %k, 1
  %c = icmp ult i32 %k1, %n
  br i1 %c, label %l, label %e
e:
  ret float %acc1
}

define void @call() {
  call void @imp()
  ret void
}
