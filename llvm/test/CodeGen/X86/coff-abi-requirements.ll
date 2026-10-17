; The ABI contracts that abi.requirements lists are emitted with each section
; that references their symbol, naming its import pointer where the code
; reaches the symbol through one, and only while the symbol has users and may
; be in another image.

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s

; CHECK-DAG: .linkcontract f1, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
; CHECK-DAG: .linkcontract f2, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
; CHECK-DAG: .linkcontract table, __imp_g, type, "1T", "000102030405060708090a0b0c0d0e0f"
; CHECK-DAG: .linkcontract f1, __imp_d, data, "d", "101112131415161718191a1b1c1d1e1f"
; CHECK-DAG: .linkcontract f1, __imp_vague, data, "vague", "101112131415161718191a1b1c1d1e1f"
; CHECK-DAG: .linkcontract f1, __imp_vague, type, "1U", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
; CHECK-NOT: .linkcontract

target triple = "x86_64-unknown-windows-itanium"

$vague = comdat any
$f2 = comdat any

@d = external dllimport global i32
@vague = linkonce_odr global i32 0, comdat
@local = external dso_local global i32
@table = global ptr @g

declare dllimport void @g(ptr)
declare dllimport void @h()

define i32 @f1(ptr %p) {
  call void @g(ptr %p)
  %v = load i32, ptr @d
  %w = load i32, ptr @vague
  %x = load i32, ptr @local
  %vw = add i32 %v, %w
  %r = add i32 %vw, %x
  ret i32 %r
}

define linkonce_odr void @f2() comdat {
  call void @g(ptr null)
  ret void
}

; @h has no users, @local is in the image, and the symbol of the last row was
; deleted.
!abi.requirements = !{!0, !1, !2, !3, !4, !5}
!0 = !{ptr @g, !10}
!1 = !{ptr @d, !11}
!2 = !{ptr @h, !10}
!3 = !{ptr @local, !10}
!4 = !{ptr @vague, !11, !12}
!5 = !{null, !10}
!10 = !{!"1T", !"\00\01\02\03\04\05\06\07\08\09\0A\0B\0C\0D\0E\0F"}
!11 = !{!"\10\11\12\13\14\15\16\17\18\19\1A\1B\1C\1D\1E\1F"}
!12 = !{!"1U", !"\F0\F1\F2\F3\F4\F5\F6\F7\F8\F9\FA\FB\FC\FD\FE\FF"}
