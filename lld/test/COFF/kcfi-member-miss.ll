; REQUIRES: x86

;; Under -guard:cf, the thunk that tests a member tag of an
;; LTO image sends a target that fails it to __llvm_kcfi_member_miss_<type>,
;; which clang makes a weak alias of the thunk for the type alone. When no
;; object the linker did not compile from bitcode has an unsealed prefix of
;; that type, no target of the type is outside the tagged set, so the linker
;; points the miss at the mismatch routine of the type, which here is the trap.
;; Otherwise, and without -guard:cf, the miss keeps the thunk for the type.
;; The miss of a local member thunk, __llvm_kcfi_member_local_miss_<type>, is
;; narrowed in the same way from the local thunk for the type.
;; LTO hashes the output's base name into the tags.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc guard.s -o guard.obj
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc native.s -o native.obj

; RUN: lld-link main.bc guard.obj -guard:cf -entry:main \
; RUN:   -debug:symtab -opt:ref -out:narrow.exe
; RUN: llvm-objdump -d narrow.exe | FileCheck %s --check-prefix=NARROW
; RUN: llvm-objdump -t narrow.exe | FileCheck %s --check-prefix=NARROW-SYM

; NARROW:      <__llvm_kcfi_member_dispatch_22222222_{{[0-9a-f]+}}>:
; NARROW:        jmp {{.*}} <__llvm_kcfi_trap>
; NARROW:        je {{.*}} <__llvm_kcfi_trap>
; NARROW:        jmp {{.*}} <__llvm_kcfi_trap>
; NARROW:        jmpq *{{.*}} <__guard_dispatch_icall_fptr>

; NARROW-SYM-DAG: 0x[[#%.8x,TRAP:]] __llvm_kcfi_trap
; NARROW-SYM-DAG: 0x[[#TRAP]] __llvm_kcfi_member_miss_22222222
; NARROW-SYM-DAG: 0x[[#TRAP]] __llvm_kcfi_member_local_miss_22222222

; RUN: lld-link main.bc guard.obj -guard:cf -entry:main \
; RUN:   -lldemit:llvm -out:%t.dir/image.bc
; RUN: llvm-dis image.bc -o - | FileCheck %s --check-prefix=IMAGE

; IMAGE: !{i32 4, !"kcfi-image", !"image.bc"}

; RUN: lld-link main.bc guard.obj native.obj -guard:cf \
; RUN:   -entry:main -include:table -debug:symtab -opt:ref -out:native.exe
; RUN: llvm-objdump -d native.exe | FileCheck %s --check-prefix=KEEP
; RUN: llvm-objdump -t native.exe | FileCheck %s --check-prefix=KEEP-SYM
; RUN: lld-link main.bc guard.obj -entry:main -debug:symtab \
; RUN:   -opt:ref -out:noguard.exe
; RUN: llvm-objdump -d noguard.exe | FileCheck %s --check-prefix=KEEP
; RUN: llvm-objdump -t noguard.exe | FileCheck %s --check-prefix=KEEP-SYM

; KEEP:      <__llvm_kcfi_member_dispatch_22222222_{{[0-9a-f]+}}>:
; KEEP:        jmp {{.*}} <__llvm_kcfi_member_miss_22222222>
; KEEP:        je {{.*}} <__llvm_kcfi_member_miss_22222222>
; KEEP:        jmp {{.*}} <__llvm_kcfi_member_miss_22222222>

; KEEP-SYM-DAG: 0x[[#%.8x,HASH:]] __llvm_kcfi_dispatch_22222222
; KEEP-SYM-DAG: 0x[[#HASH]] __llvm_kcfi_member_miss_22222222
; KEEP-SYM-DAG: 0x[[#%.8x,LOCAL:]] __llvm_kcfi_local_dispatch_22222222
; KEEP-SYM-DAG: 0x[[#LOCAL]] __llvm_kcfi_member_local_miss_22222222

;; In the sealed image the linker replaces the type's ordinary thunk, which the
;; miss reaches, but leaves the member thunk as clang wrote it. A target outside
;; the image never carries the image's tags, so it passes only through the
;; ordinary thunk, whose form tests the code ranges of the image and of the
;; DLLs it imports; testing them in the member thunk would change nothing.
; RUN: llvm-objdump -d native.exe | FileCheck %s --check-prefix=FORMS

; FORMS:      <__llvm_kcfi_member_miss_22222222>:
; FORMS-NEXT:   leaq {{.*}}(%rip), %r10
; FORMS-NEXT:   movq %rax, %r11
; FORMS-NEXT:   subq %r10, %r11
; FORMS:      <__llvm_kcfi_member_dispatch_22222222_{{[0-9a-f]+}}>:
; FORMS-NEXT:   leaq {{.*}}(%rip), %r10
; FORMS-NEXT:   cmpq %r10, %rax
; FORMS-NEXT:   jb

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@fp = global ptr @member

define void @member() !type !0 !kcfi_type !10 { ret void }

define void @call(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 572662306) ]
  ret void
}

define void @call_local(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 572662306) ], !kcfi_local !{}
  ret void
}

define void @main() !kcfi_type !10 {
  %p = load volatile ptr, ptr @fp
  call void @call(ptr %p)
  call void @call_local(ptr %p)
  ret void
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!0 = !{i64 0, !"typeid"}
!10 = !{i32 572662306}
!llvm.module.flags = !{!1, !2, !3}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"function-type-prefix", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}

;--- guard.s
        .globl @feat.00
@feat.00 = 0x800

        .section .rdata,"dr"
        .p2align 3
        .globl __guard_check_icall_fptr
__guard_check_icall_fptr:
        .quad 0
        .globl __guard_dispatch_icall_fptr
__guard_dispatch_icall_fptr:
        .quad 0
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0

;--- native.s
        .globl @feat.00
@feat.00 = 0x800

        .def other; .scl 2; .type 32; .endef
        .text
        .p2align 4
        .fill 4, 1, 0x90
__cfi_other:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl other
other:
        retq

        .data
        .globl table
table:
        .quad other

        .section .gfids$y,"dr"
        .symidx other
