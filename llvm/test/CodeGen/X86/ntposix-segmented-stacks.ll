; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s

; NT-POSIX runs on the Windows x64 platform, whose TEB slot at gs:0x28 holds a
; segmented stack's limit, whatever calling convention the code uses.

declare void @dummy_use(ptr, i32)

define void @test_basic() "split-stack" {
; CHECK-LABEL: test_basic:
; CHECK:       cmpq %gs:40, %rsp
; CHECK-NEXT:  jbe [[MORE:\.LBB[0-9_]+]]
; CHECK:       [[MORE]]:
; CHECK-NEXT:  movl $40, %r10d
; CHECK-NEXT:  movl $0, %r11d
; CHECK-NEXT:  callq __morestack
  %mem = alloca i32, i32 10
  call void @dummy_use(ptr %mem, i32 10)
  ret void
}
