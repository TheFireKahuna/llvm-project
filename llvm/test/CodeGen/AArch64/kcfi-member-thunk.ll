; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s

;; A function that LTO gave a membership tag carries it in the word before its
;; marker, which the object's records tell from a second type. A call whose
;; target LTO tests by membership calls the type's member check thunk with the
;; target in X15, which compares the type as the ordinary thunk does, and the 8
;; bytes 16 bytes before the target with each tag and the start of the marker.
;; A target that matches returns inside the code range and continues into the
;; guard function outside it; any other continues into the type's ordinary
;; check thunk through a weak symbol, or fails fast for a thunk without a type.

; CHECK:      __cfi_member:
; CHECK-NEXT:   .linkkcfitag __cfi_member
; CHECK-NEXT:   .word 43981
; CHECK-NEXT:   .ascii "\017\037\200"
; CHECK-NEXT:   .word 3735928559
; CHECK-NEXT:   .byte 184
; CHECK-NEXT:   .word 305419896
define void @member() !kcfi_type !10 !kcfi_member_tag !11 { ret void }

; CHECK-LABEL: call:
; CHECK:         mov x15, x0
; CHECK-NEXT:    bl __llvm_kcfi_member_check_12345678_0000abcd
; CHECK-NEXT:    blr x0
define void @call(ptr %p) !kcfi_type !10 {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 305419896) ]
  ret void
}

;; A call whose targets are all in the image takes the local member thunk.
; CHECK-LABEL: local:
; CHECK:         bl __llvm_kcfi_member_local_check_12345678_0000abcd
define void @local(ptr %p) !kcfi_type !10 {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 305419896) ], !kcfi_local !{}
  ret void
}

; CHECK-LABEL: unchecked:
; CHECK:         bl __llvm_kcfi_member_check_00000000_0000abcd
define void @unchecked(ptr %p) !kcfi_type !10 {
  %t = call i1 @llvm.kcfi.member.test(ptr %p, metadata i32 43981)
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() "guard_nocf"
  ret void
}

; CHECK:      __llvm_kcfi_check_12345678:
; CHECK:      .weak __llvm_kcfi_member_check_miss_12345678
; CHECK-NEXT: __llvm_kcfi_member_check_miss_12345678 = __llvm_kcfi_check_12345678
; CHECK:      __llvm_kcfi_member_check_12345678_0000abcd:
; CHECK-NEXT:   adrp x16, __llvm_code_start
; CHECK-NEXT:   add x16, x16, :lo12:__llvm_code_start
; CHECK-NEXT:   cmp x15, x16
; CHECK-NEXT:   b.lo [[OUT:\.Ltmp[0-9]+]]
; CHECK-NEXT:   adrp x16, __llvm_code_end
; CHECK-NEXT:   add x16, x16, :lo12:__llvm_code_end
; CHECK-NEXT:   cmp x15, x16
; CHECK-NEXT:   b.hs [[OUT]]
;; The marker's last bytes and the type, as the ordinary thunk compares.
; CHECK-NEXT:   ldur x16, [x15, #-8]
; CHECK-NEXT:   mov x17, #44478
; CHECK-NEXT:   movk x17, #47326, lsl #16
; CHECK-NEXT:   movk x17, #22136, lsl #32
; CHECK-NEXT:   movk x17, #4660, lsl #48
; CHECK-NEXT:   cmp x16, x17
; CHECK-NEXT:   b.ne __llvm_kcfi_member_check_miss_12345678
; CHECK-NEXT:   ldur x16, [x15, #-16]
;; The tag, then 0F 1F 80 and the marker's low byte.
; CHECK-NEXT:   mov x17, #43981
; CHECK-NEXT:   movk x17, #0, lsl #16
; CHECK-NEXT:   movk x17, #7951, lsl #32
; CHECK-NEXT:   movk x17, #61312, lsl #48
; CHECK-NEXT:   cmp x16, x17
; CHECK-NEXT:   b.eq [[HIT:\.Ltmp[0-9]+]]
; CHECK-NEXT:   b __llvm_kcfi_member_check_miss_12345678
; CHECK-NEXT: [[HIT]]:
; CHECK-NEXT:   ret
; CHECK-NEXT: [[OUT]]:
; CHECK-NEXT:   tst x15, #0xff0
; CHECK-NEXT:   b.eq __llvm_kcfi_member_check_miss_12345678
; CHECK-NEXT:   ldur x16, [x15, #-8]
; CHECK:        b.ne __llvm_kcfi_member_check_miss_12345678
; CHECK-NEXT:   ldur x16, [x15, #-16]
; CHECK:        b.eq [[GUARD:\.Ltmp[0-9]+]]
; CHECK-NEXT:   b __llvm_kcfi_member_check_miss_12345678
; CHECK-NEXT: [[GUARD]]:
; CHECK-NEXT:   adrp x16, __guard_check_icall_fptr

;; A local member thunk fails fast on a target outside a sealed image's range,
;; and misses into the local thunk.
; CHECK:      .weak __llvm_kcfi_member_local_check_miss_12345678
; CHECK-NEXT: __llvm_kcfi_member_local_check_miss_12345678 = __llvm_kcfi_local_check_12345678
; CHECK:      __llvm_kcfi_member_local_check_12345678_0000abcd:
; CHECK-NEXT:   adrp x16, __llvm_code_start
; CHECK-NEXT:   add x16, x16, :lo12:__llvm_code_start
; CHECK-NEXT:   adrp x17, __llvm_code_end
; CHECK-NEXT:   add x17, x17, :lo12:__llvm_code_end
; CHECK-NEXT:   cmp x15, x16
; CHECK-NEXT:   b.lo [[LOUT:\.Ltmp[0-9]+]]
; CHECK-NEXT:   cmp x15, x17
; CHECK-NEXT:   b.hs [[LOUT]]
; CHECK:      [[LOUT]]:
; CHECK-NEXT:   cmp x16, x17
; CHECK-NEXT:   b.ne [[LTRAP:\.Ltmp[0-9]+]]
; CHECK-NEXT:   tst x15, #0xff0
; CHECK-NEXT:   b.eq __llvm_kcfi_member_local_check_miss_12345678
; CHECK:        br x16
; CHECK-NEXT: [[LTRAP]]:
; CHECK-NEXT:   mov w0, #64
; CHECK-NEXT:   brk #0xf003

;; Without a type, only the tags are compared.
; CHECK:      __llvm_kcfi_member_check_00000000_0000abcd:
; CHECK-NOT:    [x15, #-8]
; CHECK:        b __llvm_kcfi_trap
; CHECK:      __llvm_kcfi_trap:
; CHECK-NEXT:   mov w0, #64
; CHECK-NEXT:   brk #0xf003

declare i1 @llvm.kcfi.member.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"function-type-prefix", i32 -559038737}
!10 = !{i32 305419896}
!11 = !{i32 43981}
