; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=BYTES

;; A function that LTO gave a membership tag carries it in the word before its
;; marker. A call whose target LTO tests by membership goes through the
;; type's member thunk, which compares the type as the ordinary thunk does, and
;; the 8 bytes 16 bytes before the target with each tag and the start of the
;; marker. A target that matches is taken directly inside the code range and
;; through the guard function outside it; any other continues into the type's
;; ordinary thunk through a weak symbol, or fails fast for a member check thunk
;; without a type.

; CHECK:      __cfi_member:
; CHECK-NEXT:   .long 43981
; CHECK-NEXT:   {disp32} nopl -559038737(%rax)
; CHECK-NEXT:   movl $305419896, %eax
; CHECK-NEXT: member:
; BYTES:      <__cfi_member>:
; BYTES-NEXT: cd ab
; BYTES-NEXT: 00 00
; BYTES-NEXT: 0f 1f 80 ef be ad de
; BYTES-NEXT: b8 78 56 34 12
; BYTES-EMPTY:
; BYTES-NEXT: <member>:
define void @member() !kcfi_type !10 !kcfi_member_tag !11 { ret void }

; CHECK-LABEL: call:
; CHECK:         callq __llvm_kcfi_member_dispatch_12345678_0000abcd
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
; CHECK:         callq __llvm_kcfi_member_local_dispatch_12345678_0000abcd
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
; CHECK:         callq __llvm_kcfi_member_check_00000000_0000abcd
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

;; The miss falls back on the ordinary thunk, which is emitted though no call
;; refers to it directly.
; CHECK:      __llvm_kcfi_dispatch_12345678:
; CHECK:      .weak __llvm_kcfi_member_miss_12345678
; CHECK-NEXT: __llvm_kcfi_member_miss_12345678 = __llvm_kcfi_dispatch_12345678
; CHECK:      __llvm_kcfi_member_dispatch_12345678_0000abcd:
; CHECK-NEXT:   leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:   cmpq %r10, %rax
; CHECK-NEXT:   jb [[OUT:\.Ltmp[0-9]+]]
; CHECK-NEXT:   leaq __llvm_code_end(%rip), %r10
; CHECK-NEXT:   cmpq %r10, %rax
; CHECK-NEXT:   jae [[OUT]]
;; The marker's last bytes and the type, as the ordinary thunk compares.
; CHECK-NEXT:   movabsq $1311768467969322430, %r11 # imm = 0x12345678B8DEADBE
; CHECK-NEXT:   cmpq %r11, -8(%rax)
; CHECK-NEXT:   jne __llvm_kcfi_member_miss_12345678
;; The tag, then 0F 1F 80 and the marker's low byte.
; CHECK-NEXT:   movabsq $-1188916152340796467, %r11 # imm = 0xEF801F0F0000ABCD
; CHECK-NEXT:   cmpq %r11, -16(%rax)
; CHECK-NEXT:   je [[HIT:\.Ltmp[0-9]+]]
; CHECK-NEXT:   jmp __llvm_kcfi_member_miss_12345678
; CHECK-NEXT: [[HIT]]:
; CHECK-NEXT:   jmpq *%rax
; CHECK-NEXT: [[OUT]]:
; CHECK-NEXT:   testl $4080, %eax
; CHECK-NEXT:   je __llvm_kcfi_member_miss_12345678
; CHECK-NEXT:   movabsq $1311768467969322430, %r11
; CHECK-NEXT:   cmpq %r11, -8(%rax)
; CHECK-NEXT:   jne __llvm_kcfi_member_miss_12345678
; CHECK-NEXT:   movabsq $-1188916152340796467, %r11
; CHECK-NEXT:   cmpq %r11, -16(%rax)
; CHECK-NEXT:   je [[GUARD:\.Ltmp[0-9]+]]
; CHECK-NEXT:   jmp __llvm_kcfi_member_miss_12345678
; CHECK-NEXT: [[GUARD]]:
; CHECK-NEXT:   jmpq *__guard_dispatch_icall_fptr(%rip)


;; A local member thunk fails fast on a target outside a sealed image's range,
;; and misses into the local thunk.
; CHECK:      .weak __llvm_kcfi_member_local_miss_12345678
; CHECK-NEXT: __llvm_kcfi_member_local_miss_12345678 = __llvm_kcfi_local_dispatch_12345678
; CHECK:      __llvm_kcfi_member_local_dispatch_12345678_0000abcd:
; CHECK-NEXT:   leaq __llvm_code_start(%rip), %r10
; CHECK-NEXT:   leaq __llvm_code_end(%rip), %r11
; CHECK-NEXT:   cmpq %r10, %rax
; CHECK-NEXT:   jb [[LOUT:\.Ltmp[0-9]+]]
; CHECK-NEXT:   cmpq %r11, %rax
; CHECK-NEXT:   jae [[LOUT]]
; CHECK:      [[LOUT]]:
; CHECK-NEXT:   cmpq %r11, %r10
; CHECK-NEXT:   jne [[LTRAP:\.Ltmp[0-9]+]]
; CHECK-NEXT:   testl $4080, %eax
; CHECK-NEXT:   je __llvm_kcfi_member_local_miss_12345678
; CHECK:        jmpq *__guard_dispatch_icall_fptr(%rip)
; CHECK-NEXT: [[LTRAP]]:
; CHECK-NEXT:   movl $64, %ecx
; CHECK-NEXT:   int $41

;; Without a type, only the tags are compared.
; CHECK:      __llvm_kcfi_member_check_00000000_0000abcd:
; CHECK-NOT:    -8(%rcx)
; CHECK:        jmp __llvm_kcfi_trap
; CHECK:        retq
; CHECK:      __llvm_kcfi_trap:
; CHECK-NEXT:   movl $64, %ecx
; CHECK-NEXT:   int $41

declare i1 @llvm.kcfi.member.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 4, !"kcfi-marker", i32 -559038737}
!10 = !{i32 305419896}
!11 = !{i32 43981}
