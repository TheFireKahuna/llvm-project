; REQUIRES: x86, aarch64
;; The compiler's tail calls reach the link as jumps it may delete, and the
;; link lays a chain of them out in a row: a falls into b and b into c, though
;; the object holds them in the other order.
; DEFINE: %{triple} = x86_64-unknown-windows-itanium
; DEFINE: %{build} = \
; DEFINE:   llc -mtriple=%{triple} -filetype=obj -function-sections %s -o %t.obj && \
; DEFINE:   lld-link -entry:main -subsystem:console %t.obj -out:%t.exe \
; DEFINE:     -debug:symtab && llvm-objdump -d --no-show-raw-insn %t.exe
; RUN: %{build} | FileCheck %s --check-prefixes=CHECK,X64
; REDEFINE: %{triple} = aarch64-pc-windows-ntposix
; RUN: %{build} | FileCheck %s

; CHECK-LABEL: <a>:
; X64-NEXT:      movl $0x1,
; CHECK-NOT:     {{jmp|[^a-z]b[^a-z]}}
; CHECK-LABEL: <b>:
; X64-NEXT:      movl $0x2,
; CHECK-NOT:     {{jmp|[^a-z]b[^a-z]}}
; CHECK-LABEL: <c>:
; X64-NEXT:      movl $0x3,
; CHECK:         ret

@g = dso_local global i32 0

define dso_local void @c() {
  store volatile i32 3, ptr @g
  ret void
}

define dso_local void @b() {
  store volatile i32 2, ptr @g
  tail call void @c()
  ret void
}

define dso_local void @a() {
  store volatile i32 1, ptr @g
  tail call void @b()
  ret void
}

define dso_local void @main() {
  call void @a()
  ret void
}
