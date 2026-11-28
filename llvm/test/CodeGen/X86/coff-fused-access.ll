; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED
; RUN: llc < %s -mtriple=x86_64-pc-windows-ntposix | \
; RUN:   FileCheck %s --check-prefixes=CHECK,FUSED
; RUN: llc < %s -mtriple=x86_64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=CHECK,PLAIN --implicit-check-not=FUSED
; RUN: llc < %s -mtriple=x86_64-unknown-windows-itanium -O0 | \
; RUN:   FileCheck %s --check-prefix=O0 --implicit-check-not=FUSED
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-unknown-windows-itanium | \
; RUN:   FileCheck %s --check-prefixes=A64,A64F %}
; RUN: %if aarch64-registered-target %{ llc < %s -mtriple=aarch64-pc-windows-msvc | \
; RUN:   FileCheck %s --check-prefixes=A64,A64P --implicit-check-not=IF_LOCAL %}

; On Windows Itanium and NT-POSIX an optimized function marks each load or
; store whose base, with no index, only the load of an import pointer reaches,
; naming the pointer, for the linker to make direct. A load whose register only
; such sites read is a fused read.

%struct = type { i32, i64 }
@v = external dllimport global i32
@s = external dllimport global %struct
@arr = external dllimport global [8 x i32]
declare void @taking(ptr)

; Each access of several is one.
define void @write(i32 %a, i64 %b) {
; CHECK-LABEL: write:
; FUSED:         movq __imp_s@FUSED(%rip), %rax
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_s
; PLAIN:         movq __imp_s(%rip), %rax
; CHECK-NEXT:    movl %e{{[a-z]+}}, (%rax)
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_s
; CHECK-NEXT:    movq %r{{[a-z]+}}, 8(%rax)
; O0-LABEL: write:
; O0:            movq __imp_s(%rip), %rax
; A64-LABEL: write:
; A64F:          ldr x8, [x8, :fused_lo12:__imp_s]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_s
; A64P:          ldr x8, [x8, :lo12:__imp_s]
; A64-NEXT:      str w0, [x8]
; A64F-NEXT:   [[SITE:.Ltmp[0-9]+]]:
; A64F-NEXT:     .reloc [[SITE]], IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_s
; A64-NEXT:      str x1, [x8, #8]
  store i32 %a, ptr @s
  %f = getelementptr %struct, ptr @s, i32 0, i32 1
  store i64 %b, ptr %f
  ret void
}

; An access with an index is none, and its load stays one.
define i32 @indexed(i64 %i) {
; CHECK-LABEL: indexed:
; CHECK:         movq __imp_arr(%rip), %rax
; CHECK-NEXT:    movl (%rax,%r{{[a-z]+}},4), %eax
; A64-LABEL: indexed:
; A64:           ldr x8, [x8, :lo12:__imp_arr]
; A64-NEXT:      ldr w0, [x8, x0, lsl #2]
  %p = getelementptr [8 x i32], ptr @arr, i64 0, i64 %i
  %x = load i32, ptr %p
  ret i32 %x
}

; Where another reader keeps the load, x86-64 states an access in the load's
; block, which needs no register once the linker has made it direct. On
; AArch64 an access takes a page offset only with its load dropped.
define i32 @kept() {
; CHECK-LABEL: kept:
; CHECK:         movq __imp_v(%rip), [[REG:%r[a-z]+]]
; FUSED-NEXT:  [[SITE:.Ltmp[0-9]+]]:
; FUSED-NEXT:    .reloc [[SITE]], IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
; CHECK-NEXT:    movl ([[REG]]), %e{{[a-z]+}}
; CHECK-NEXT:    callq taking
; A64-LABEL: kept:
; A64:           ldr x0, [x0, :lo12:__imp_v]
; A64-NEXT:      ldr w19, [x0]
  %x = load i32, ptr @v
  call void @taking(ptr @v)
  ret i32 %x
}

; A pointer the compiler chose is loaded again behind a call when read on both
; sides, as the linker may remove each load, where a callee-saved register
; would stay. A dllimport variable's pointer, whose load stays, is held; AArch64
; holds both, as other targets do.
@chosen = external global [8 x i32]

define i32 @across() {
; CHECK-LABEL: across:
; FUSED:         movq __imp_chosen@FUSED(%rip), %rax
; FUSED:         movq __imp_taking(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq taking
; FUSED-NEXT:    movq __imp_arr@FUSED(%rip), %r[[HELD:[a-z0-9]+]]
; FUSED:         movq __imp_chosen@FUSED(%rip), %rax
; FUSED:         movq __imp_taking(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq taking
; FUSED:         addl 4(%r[[HELD]]), %e
; PLAIN:         movq __imp_arr(%rip), %r[[HELD:[a-z0-9]+]]
; PLAIN:         callq taking
; PLAIN:         addl 4(%r[[HELD]]), %e
; A64-LABEL: across:
; A64F:          ldr x[[CHOSEN:19|2[0-9]]], [x[[CHOSEN]], :fused_lo12:__imp_chosen]
; A64F:          bl taking
; A64F:          ldr w{{[0-9]+}}, [x[[CHOSEN]], #4]
  %a = load i32, ptr @chosen
  call void @taking(ptr @taking)
  %b = load i32, ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 1)
  %c = load i32, ptr @arr
  call void @taking(ptr @taking)
  %d = load i32, ptr getelementptr ([8 x i32], ptr @arr, i64 0, i64 1)
  %ab = add i32 %a, %b
  %cd = add i32 %c, %d
  %sum = add i32 %ab, %cd
  ret i32 %sum
}

; The address of a function is an instruction wherever it is loaded again,
; so one that is read in a loop that contains a call is held, as the sum of
; a pointer and a constant is. A copy of it is no such read: the load takes
; the copy's place.
define void @repeated(ptr %o, i32 %n) {
; CHECK-LABEL: repeated:
; FUSED:         movq __imp_taking(%rip), %r[[HELD:bx|si|di|bp|1[2-5]]]
; FUSED:       .LBB{{.*}}:
; FUSED:         movq %r[[HELD]], (%r
; FUSED-NEXT:    movq __imp_get(%rip), %r{{cx|di}}
; FUSED-NEXT:    callq taking
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  store ptr @taking, ptr %o
  call void @taking(ptr @get)
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret void
}

; So is the pointer of a variable where it is read as an address: only a
; load or a store through it takes the load of the pointer into itself.
define void @address(ptr %o, i32 %n) {
; CHECK-LABEL: address:
; FUSED:         movq __imp_chosen(%rip), %r[[HELD:bx|si|di|bp|1[2-5]]]
; FUSED:       .LBB{{.*}}:
; FUSED:         movq %r[[HELD]], (%r
; FUSED-NOT:     __imp_chosen
; FUSED:         callq taking
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  store ptr @chosen, ptr %o
  call void @taking(ptr %o)
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret void
}

; Splitting loads the pointer again where a part of its range begins, so a
; part has several loads of the one pointer: it is that pointer still, and
; no callee-saved register holds it around the calls of a loop.
define i32 @split(ptr %o, i32 %n) {
; CHECK-LABEL: split:
; FUSED-NOT:     __imp_chosen
; FUSED:         callq taking
; FUSED-NEXT:    movq __imp_chosen@FUSED(%rip), %rax
; FUSED:         addl 8(%rax), %e
; FUSED:         callq get
; FUSED-NEXT:    movq __imp_chosen{{.*}}(%rip), %r[[AGAIN:[a-z0-9]+]]
; FUSED:         movl (%r[[AGAIN]]), %e
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %latch ]
  %g = call i32 @get(ptr %o)
  %a = load i32, ptr @chosen
  %ag = add i32 %a, %g
  %odd = icmp eq i32 %g, 0
  br i1 %odd, label %left, label %right
left:
  call void @taking(ptr %o)
  br label %latch
right:
  call void @taking(ptr null)
  br label %latch
latch:
  %b = load i32, ptr getelementptr ([8 x i32], ptr @chosen, i64 0, i64 2)
  %v = add i32 %ag, %b
  %acc2 = add i32 %acc, %v
  %next = add i32 %i, 1
  %more = icmp ne i32 %next, %n
  br i1 %more, label %body, label %done
done:
  ret i32 %acc2
}
declare i32 @get(ptr)
