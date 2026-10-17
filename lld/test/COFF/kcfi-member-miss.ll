; REQUIRES: x86

;; Under -guard:cf, the thunk that tests a member tag of an
;; LTO image sends a target of its type that fails it to
;; __llvm_kcfi_member_miss_<type>, or to __llvm_kcfi_member_outside_miss_<type>
;; outside the image's code range, which clang makes weak aliases of the thunk
;; for the type alone; a target of another type goes to the type's mismatch
;; routine. When no object the linker did not compile from bitcode has an
;; unsealed prefix of that type, no target of the type in the image is outside
;; the tagged set, so the linker points the misses at the mismatch routine of
;; the type, which here is the trap. Otherwise, and without -guard:cf, the
;; misses keep the thunk for the type. The miss of a local member thunk,
;; __llvm_kcfi_member_local_miss_<type>, is narrowed in the same way from the
;; local thunk for the type. LTO hashes the output's base name into the tags.

; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
; RUN: llvm-as main.ll -o main.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc guard.s -o guard.obj
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc native.s -o native.obj

; RUN: lld-link main.bc guard.obj -guard:cf -entry:main \
; RUN:   -debug:symtab -opt:ref -out:narrow.exe
; RUN: llvm-objdump -d narrow.exe | FileCheck %s --check-prefix=NARROW
; RUN: llvm-objdump -t narrow.exe | FileCheck %s --check-prefix=NARROW-SYM

; NARROW:      <__llvm_kcfi_member_dispatch_22222222_{{[0-9a-f]+}}>:
; NARROW:        jne 0x[[TAIL:[0-9a-f]+]]
; NARROW:        jmp {{.*}} <__llvm_kcfi_trap>
; NARROW:        je 0x[[TAIL]]
; NARROW:        jmp {{.*}} <__llvm_kcfi_trap>
; NARROW:        jmpq *{{.*}} <__guard_dispatch_icall_fptr>
; NARROW-NEXT:   [[TAIL]]: {{.*}}jmp {{.*}} <__llvm_kcfi_trap>

; NARROW-SYM-DAG: 0x[[#%.8x,TRAP:]] __llvm_kcfi_trap
; NARROW-SYM-DAG: 0x[[#TRAP]] __llvm_kcfi_member_miss_22222222
; NARROW-SYM-DAG: 0x[[#TRAP]] __llvm_kcfi_member_outside_miss_22222222
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
; KEEP:        jne 0x[[KTAIL:[0-9a-f]+]]
; KEEP:        jmp {{.*}} <__llvm_kcfi_{{member_miss|member_outside_miss|dispatch}}_22222222>
; KEEP:        je 0x[[KTAIL]]
; KEEP:        jmp {{.*}} <__llvm_kcfi_{{member_outside_miss|member_miss|dispatch}}_22222222>
; KEEP:        [[KTAIL]]: {{.*}}jmp {{.*}} <__llvm_kcfi_{{trap|mismatch_22222222}}>

; KEEP-SYM-DAG: 0x[[#%.8x,HASH:]] __llvm_kcfi_dispatch_22222222
; KEEP-SYM-DAG: 0x[[#HASH]] __llvm_kcfi_member_miss_22222222
; KEEP-SYM-DAG: 0x[[#%.8x,LOCAL:]] __llvm_kcfi_local_dispatch_22222222
; KEEP-SYM-DAG: 0x[[#LOCAL]] __llvm_kcfi_member_local_miss_22222222

; NOGUARD-SYM-DAG: 0x[[#%.8x,HASH:]] __llvm_kcfi_dispatch_22222222
; NOGUARD-SYM-DAG: 0x[[#HASH]] __llvm_kcfi_member_outside_miss_22222222

;; In the sealed image the linker replaces the type's ordinary thunk, which the
;; misses reach, but leaves the member thunk as clang wrote it. A target outside
;; the image never carries the image's tags, so it passes only through the
;; ordinary thunk, whose form tests the code ranges of the DLLs the image
;; imports. The outside miss, whose type the member thunk has checked, takes
;; the form's entry for such a target, which tests those ranges alone and
;; continues into the guard function.
; RUN: llvm-objdump -d native.exe | FileCheck %s --check-prefix=FORMS
; RUN: llvm-objdump -t noguard.exe | FileCheck %s --check-prefix=NOGUARD-SYM

; FORMS:      <__llvm_kcfi_member_miss_22222222>:
; FORMS-NEXT:   leaq {{.*}}(%rip), %r10
; FORMS-NEXT:   movq %rax, %r11
; FORMS-NEXT:   subq %r10, %r11
; FORMS:      <__llvm_kcfi_member_outside_miss_22222222>:
; FORMS-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>
; FORMS-NEXT:   jmpq *%rax
; FORMS:      <__llvm_kcfi_member_dispatch_22222222_{{[0-9a-f]+}}>:
; FORMS-NEXT:   leaq {{.*}}(%rip), %r10
; FORMS-NEXT:   cmpq %r10, %rax
; FORMS-NEXT:   jb

;; A function in another image carries none of the image's tags, so the
;; outside miss of a type of which it may be a valid target keeps the thunk
;; for the type, at its entry for a checked target: one that an object opens,
;; here dynamically, and one that the linker opens for an import. Their misses
;; inside the range, and both misses of a type that the linker opens only for
;; a definition in the image without a prefix, are narrowed to the mismatch
;; routine, the one the linker makes for the last.
; RUN: llvm-as open.ll -o open.bc
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc plain.s -o plain.obj
; RUN: llvm-dlltool -m i386:x86-64 -d lib.def -D lib.dll -l lib.lib
; RUN: lld-link main.bc open.bc guard.obj plain.obj lib.lib -guard:cf \
; RUN:   -entry:main -include:open -debug:symtab -opt:ref -out:open.exe
; RUN: llvm-objdump -t open.exe | FileCheck %s --check-prefix=OPEN
; RUN: llvm-objdump -d open.exe | FileCheck %s --check-prefix=OPEN-FORMS

; OPEN-DAG: 0x[[#%.8x,DYNAMIC:]] __llvm_kcfi_mismatch_33333333
; OPEN-DAG: 0x[[#DYNAMIC]] __llvm_kcfi_member_miss_33333333
; OPEN-DAG: 0x[[#%.8x,IMPORTED:]] __llvm_kcfi_mismatch_44444444
; OPEN-DAG: 0x[[#IMPORTED]] __llvm_kcfi_member_miss_44444444
; OPEN-DAG: 0x[[#%.8x,PLAIN:]] __llvm_kcfi_mismatch_55555555
; OPEN-DAG: 0x[[#PLAIN]] __llvm_kcfi_member_miss_55555555
; OPEN-DAG: 0x[[#PLAIN]] __llvm_kcfi_member_outside_miss_55555555

; OPEN-FORMS:      <__llvm_kcfi_member_outside_miss_33333333>:
; OPEN-FORMS-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>
; OPEN-FORMS-NEXT:   jmpq *%rax
; OPEN-FORMS:      <__llvm_kcfi_member_outside_miss_44444444>:
; OPEN-FORMS-NEXT:   jmpq *{{.*}}(%rip) {{.*}}<__guard_dispatch_icall_fptr>
; OPEN-FORMS-NEXT:   jmpq *%rax

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
;--- open.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define void @call_dynamic(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid.dynamic")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 858993459) ]
  ret void
}

define void @call_imported(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid.imported")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 1145324612) ]
  ret void
}

define void @call_plain(ptr %p) noinline !kcfi_type !10 {
  %t = call i1 @llvm.type.test(ptr %p, metadata !"typeid.plain")
  br i1 %t, label %cont, label %trap
trap:
  call void @llvm.ubsantrap(i8 64)
  unreachable
cont:
  call void %p() [ "kcfi"(i32 1431655765) ]
  ret void
}

define void @open(ptr %p) !kcfi_type !10 {
  call void @call_dynamic(ptr %p)
  call void @call_imported(ptr %p)
  call void @call_plain(ptr %p)
  ret void
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

!10 = !{i32 572662306}
!kcfi.dynamic = !{!20}
!20 = !{i32 858993459}
!llvm.module.flags = !{!1, !2, !3}
!1 = !{i32 4, !"kcfi", i32 1}
!2 = !{i32 4, !"function-type-prefix", i32 119298566}
!3 = !{i32 2, !"cfguard", i32 2}

;--- plain.s
        .globl @feat.00
@feat.00 = 0x800

        .text
        .globl plain
plain:
        retq

        .data
        .quad imported
        .quad plain

        .weak __kcfi_typeid_imported
__kcfi_typeid_imported = 0x44444444
        .weak __kcfi_typeid_plain
__kcfi_typeid_plain = 0x55555555

;--- lib.def
LIBRARY lib.dll
EXPORTS
imported
