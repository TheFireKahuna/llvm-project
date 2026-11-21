; RUN: llc -mtriple=x86_64-pc-windows-msvc -verify-machineinstrs < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=x86_64-pc-windows-msvc -verify-machineinstrs -stop-after=kcfi < %s | FileCheck %s --check-prefix=KCFI
; RUN: not llc -mtriple=x86_64-pc-windows-msvc -code-model=large < %s 2>&1 | FileCheck %s --check-prefix=LARGE

; LARGE: LLVM ERROR: KCFI checks of calls through the Control Flow Guard dispatch function are not supported in the large code model

;; A call through the Control Flow Guard dispatch function takes its target in
;; RAX, so the KCFI check tests RAX and the call keeps going through the
;; dispatch function.

define void @f1(ptr noundef %x) {
; ASM-LABEL: f1:
; ASM:         movq %rcx, %rax
; ASM-NEXT:    movl $4282621618, %r10d # imm = 0xFF439EB2
; ASM-NEXT:    addl -4(%rax), %r10d
; ASM-NEXT:    je .Ltmp0
; ASM-NEXT:  .Ltmp1:
; ASM-NEXT:    ud2
; ASM:       .Ltmp0:
; ASM-NEXT:    callq *__guard_dispatch_icall_fptr(%rip)

; KCFI-LABEL: name: f1
; KCFI:       BUNDLE{{.*}} {
; KCFI-NEXT:    KCFI_CHECK $rax, 12345678, implicit-def $r10, implicit-def $r11, implicit-def $eflags
; KCFI-NEXT:    CALL64m $rip, 1, $noreg, @__guard_dispatch_icall_fptr, $noreg,
; KCFI-NEXT:  }
  call void %x() [ "kcfi"(i32 12345678) ]
  ret void
}

define void @f2(ptr noundef %x) {
; ASM-LABEL: f2:
; ASM:         movq %rcx, %rax
; ASM-NEXT:    movl $4282621618, %r10d # imm = 0xFF439EB2
; ASM-NEXT:    addl -4(%rax), %r10d
; ASM:         rex64 jmpq *__guard_dispatch_icall_fptr(%rip) # TAILCALL

; KCFI-LABEL: name: f2
; KCFI:       BUNDLE{{.*}} {
; KCFI-NEXT:    KCFI_CHECK $rax, 12345678, implicit-def $r10, implicit-def $r11, implicit-def $eflags
; KCFI-NEXT:    TAILJMPm64_REX $rip, 1, $noreg, @__guard_dispatch_icall_fptr, $noreg,
; KCFI-NEXT:  }
  tail call void %x() [ "kcfi"(i32 12345678) ]
  ret void
}

declare i32 @__CxxFrameHandler3(...)

define void @f3(ptr noundef %x) personality ptr @__CxxFrameHandler3 {
; ASM-LABEL: f3:
; ASM:         movq %rcx, %rax
; ASM-NEXT:  .Ltmp{{[0-9]+}}: # EH_LABEL
; ASM-NEXT:    movl $4282621618, %r10d # imm = 0xFF439EB2
; ASM-NEXT:    addl -4(%rax), %r10d
; ASM:         callq *__guard_dispatch_icall_fptr(%rip)

; KCFI-LABEL: name: f3
; KCFI:       BUNDLE{{.*}} {
; KCFI-NEXT:    KCFI_CHECK $rax, 12345678, implicit-def $r10, implicit-def $r11, implicit-def $eflags
; KCFI-NEXT:    CALL64m $rip, 1, $noreg, @__guard_dispatch_icall_fptr, $noreg,
; KCFI-NEXT:  }
  invoke void %x() [ "kcfi"(i32 12345678) ]
    to label %cont
    unwind label %cleanup
cont:
  ret void
cleanup:
  %pad = cleanuppad within none []
  cleanupret from %pad unwind to caller
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 2, !"cfguard", i32 2}
