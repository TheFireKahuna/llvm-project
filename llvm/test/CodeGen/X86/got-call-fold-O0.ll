; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefix=NTPOSIX
; RUN: llc -O0 -verify-machineinstrs -mtriple=x86_64-unknown-linux-gnu -relocation-model=pic < %s | FileCheck %s --check-prefix=ELF

; Without optimization a callee's GOT or import-slot load still folds into the
; call, the form a linker relaxes to a direct call, also when the call is
; selected by SelectionDAG because its block holds an invoke. The load stays
; unfolded wherever the slot's value is used other than as a callee.

declare void @ext() nonlazybind
declare void @other() nonlazybind
declare void @use(ptr) nonlazybind
declare i32 @rust_eh_personality(...)

define void @f() personality ptr @rust_eh_personality {
; NTPOSIX-LABEL: f:
; NTPOSIX:     callq *__imp_ext(%rip)
; NTPOSIX:     callq *__imp_other(%rip)
; NTPOSIX:     movq __imp_ext(%rip), %rdi
; NTPOSIX:     callq *__imp_use(%rip)
;
; ELF-LABEL: f:
; ELF:         callq *ext@GOTPCREL(%rip)
; ELF:         callq *other@GOTPCREL(%rip)
; ELF:         movq ext@GOTPCREL(%rip), %rdi
; ELF:         callq *use@GOTPCREL(%rip)
  call void @ext()
  invoke void @other() to label %ok unwind label %lp
ok:
  invoke void @use(ptr @ext) to label %done unwind label %lp
done:
  ret void
lp:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"RtLibUseGOT", i32 1}
