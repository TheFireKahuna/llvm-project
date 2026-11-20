; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s

; A push that the processor pairs with its pop keeps the pop, so the function
; does not end in `leave`.

declare void @ext(ptr)
declare void @exti(i64)

define i64 @ppx(i64 %a) "frame-pointer"="all" "target-features"="+ppx" uwtable {
; CHECK-LABEL: ppx:
; CHECK:       pushp %rbp
; CHECK:       pushp %rsi
; CHECK:       .seh_startepilogue
; CHECK-NEXT:  addq $56, %rsp
; CHECK-NEXT:  popp %rsi
; CHECK-NEXT:  popp %rbp
; CHECK-NEXT:  .seh_endepilogue
; CHECK-NEXT:  retq
  %b = alloca [24 x i8]
  call void @ext(ptr %b)
  call void @exti(i64 %a)
  ret i64 %a
}
