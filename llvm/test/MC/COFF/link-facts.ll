;; Link facts travel as .link_fact, in the Symbols link records and in the
;; bitcode symbol table: a type prefix (tag 0) and, for every name of a vague
;; entity, its visibility (1: 0 unique, 1 default, 2 protected, 3 hidden),
;; initialization (2), type_info (3) and address significance (5).

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s \
; RUN:   | FileCheck %s --check-prefixes=ASM,VAGUE --implicit-check-not=.link_fact
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
; RUN: llvm-readobj --coff-link-records %t.o | FileCheck %s --check-prefix=OBJ
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-ntposix -filetype=obj %s -o %t.a64.o %}
; RUN: %if aarch64-registered-target %{ llvm-readobj --coff-link-records %t.a64.o \
; RUN:   | FileCheck %s --check-prefix=OBJ %}
; RUN: opt -mtriple=x86_64-unknown-windows-itanium %s -o %t.bc
; RUN: llvm-lto2 dump-symtab %t.bc | FileCheck %s --check-prefix=SYMTAB
;; A reader rebuilds the symbol table of another producer's bitcode from its
;; IR, with the same facts.
; RUN: env LLVM_OVERRIDE_PRODUCER=other opt -mtriple=x86_64-unknown-windows-itanium \
; RUN:   %s -o %t.other.bc
; RUN: llvm-lto2 dump-symtab %t.other.bc | FileCheck %s --check-prefix=SYMTAB

;; Nothing of another environment has a fact.
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s \
; RUN:   | FileCheck %s --allow-empty --implicit-check-not=.link_fact
; RUN: opt -mtriple=x86_64-pc-windows-msvc %s -o %t.msvc.bc
; RUN: llvm-lto2 dump-symtab %t.msvc.bc \
; RUN:   | FileCheck %s --check-prefix=NONE --implicit-check-not="link facts"

;; The fact is stated where the prefix is emitted: x86 emits one only in a
;; module with the kcfi flag, and AArch64 in either.
; RUN: sed 's/!{!0, !1}/!{!1}/' %s > %t.nokcfi.ll
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %t.nokcfi.ll | FileCheck %s \
; RUN:   --check-prefixes=NOKCFI,VAGUE --implicit-check-not=.link_fact \
; RUN:   --implicit-check-not=__cfi_
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-ntposix < %t.nokcfi.ll \
; RUN:   | FileCheck %s --check-prefixes=ASM,VAGUE,PREFIX --implicit-check-not=.link_fact %}
; NOKCFI: f1:
; PREFIX: __cfi_f1:

; ASM: .link_fact f1, 0, 0
; ASM: .link_fact f3, 0, 0
; ASM: .link_fact f4, 0, 1

; OBJ:      Kind: Symbols (0x1)
; OBJ:        Symbol: f1 ({{.*}})
; OBJ-NEXT:   Tag: TypePrefix (0x0)
; OBJ:          0000: 00
; OBJ:        Symbol: f3 ({{.*}})
; OBJ:          0000: 00
; OBJ:        Symbol: f4 ({{.*}})
; OBJ:          0000: 01
;; A function whose address is not significant and a hidden function state
;; nothing, and a weak function that is not ODR is no vague entity; a weak
;; reference states a visibility that is not default.
; VAGUE:      .link_fact g1, 1, 1
; VAGUE-NEXT: .link_fact g1, 5
; VAGUE-NEXT: .link_fact g4, 1, 2
; VAGUE-NEXT: .link_fact g4, 5
; VAGUE-NEXT: .link_fact g5, 11
; VAGUE-NEXT: .link_fact w3, 1, 2
;; Default and neither constant nor dynamically initialized; hidden and
;; constant; protected, which unique does not change, and initialized where
;; control passes it; unique and initialized with the image; a weak variable.
; VAGUE-NEXT: .link_fact v1, 1, 1
; VAGUE-NEXT: .link_fact v1, 2, 1
; VAGUE-NEXT: .link_fact v2, 1, 3
; VAGUE-NEXT: .link_fact v2, 2, 0
; VAGUE-NEXT: .link_fact v3, 1, 2
; VAGUE-NEXT: .link_fact v3, 2, 2
; VAGUE-NEXT: .link_fact v4, 1, 0
; VAGUE-NEXT: .link_fact v4, 2, 3
;; A guard in its variable's COMDAT is a name of the variable's group.
; VAGUE-NEXT: .link_fact _ZGV2v4, 1, 1
; VAGUE-NEXT: .link_fact _ZGV2v4, 2, 1
; VAGUE-NEXT: .link_fact v6, 1, 1
; VAGUE-NEXT: .link_fact v6, 2, 1
;; A type_info object. Its name, a guard in a COMDAT of its own, a
;; thread-local variable and a common one state nothing.
; VAGUE-NEXT: .link_fact _ZTI1A, 1, 1
; VAGUE-NEXT: .link_fact _ZTI1A, 2, 0
; VAGUE-NEXT: .link_fact _ZTI1A, 3
; VAGUE-NEXT: .link_fact w1, 1, 3
;; A thread-local variable with a record states its guard, tag 7, and one
;; that a thread initializes when it first uses it the initialization 4. A
;; record whose guard is gone names none.
; VAGUE-NEXT: .link_fact t1, 7, __tls_set
; VAGUE-NEXT: .link_fact t2, 7, __tls_guard
; VAGUE-NEXT: .link_fact t2, 2, 4
; VAGUE-NEXT: .link_fact t3, 7
;; An alias is another name of its variable, at its address or inside it,
;; with its own visibility.
; VAGUE-NEXT: .link_fact a1, 1, 1
; VAGUE-NEXT: .link_fact a1, 2, 1
; VAGUE-NEXT: .link_fact a2, 1, 3
; VAGUE-NEXT: .link_fact a2, 2, 3
; VAGUE-NEXT: .link_fact a3, 1, 1
; VAGUE-NEXT: .link_fact a3, 2, 1

; OBJ:        Symbol: g1 ({{.*}})
; OBJ-NEXT:   Tag: Visibility (0x1)
; OBJ:          0000: 01
; OBJ:        Symbol: g1 ({{.*}})
; OBJ-NEXT:   Tag: AddressSignificant (0x5)
; OBJ:        Symbol: w3 ({{.*}})
; OBJ:        Symbol: v1 ({{.*}})
; OBJ:        Symbol: v1 ({{.*}})
; OBJ-NEXT:   Tag: Init (0x2)
; OBJ:        Symbol: _ZTI1A ({{.*}})
; OBJ:        Symbol: _ZTI1A ({{.*}})
; OBJ:        Symbol: _ZTI1A ({{.*}})
; OBJ-NEXT:   Tag: Descriptor (0x3)
; OBJ:        Symbol: w1 ({{.*}})
; OBJ-NEXT:   Tag: Visibility (0x1)
; OBJ:          0000: 03
; OBJ:        Symbol: t1 ({{.*}})
; OBJ-NEXT:   Tag: TlsRecord (0x7)
; OBJ-NEXT:   Guard: __tls_set ({{.*}})
; OBJ:        Symbol: t2 ({{.*}})
; OBJ-NEXT:   Tag: TlsRecord (0x7)
; OBJ-NEXT:   Guard: __tls_guard ({{.*}})
; OBJ:        Symbol: t2 ({{.*}})
; OBJ-NEXT:   Tag: Init (0x2)
; OBJ:          0000: 04
; OBJ:        Symbol: t3 ({{.*}})
; OBJ-NEXT:   Tag: TlsRecord (0x7)
; OBJ-NEXT:   Operands (
; OBJ-NEXT:   )

;; f1 is symbol 0, f3 symbol 2 and f4 symbol 3: each fact is the index, the
;; tag, the size of its operands and the operands. g1 is symbol 5 and g4
;; symbol 8, each with its visibility and then its significant address. t1 is
;; symbol 24 and its guard symbol 27; t2's guard, symbol 28, is local.
; SYMTAB:      version: 5
; SYMTAB:      link facts: 000001000200010003000101{{0501010105050008010102080500.*1807011B1907011C190201041A0700}}
; SYMTAB-NEXT: D------X f1
; SYMTAB-NEXT: D------X f2
; SYMTAB-NEXT: D------X f4

; NONE: version: 5

define void @f1() !kcfi_type !2 {
  ret void
}

define void @f2() {
  ret void
}

define internal void @f3() "patchable-function-prefix"="2" !kcfi_type !2 {
  ret void
}

define void @f4() !kcfi_type !2 !kcfi_vfn_type !3 {
  call void @f3()
  call void @f5()
  ret void
}

declare !kcfi_type !2 void @f5()

$v1 = comdat any
$v2 = comdat any
$v3 = comdat any
$v4 = comdat any
$v5 = comdat any
$v6 = comdat any
$_ZTI1A = comdat any
$_ZTS1A = comdat any
$g1 = comdat any
$g2 = comdat any
$g3 = comdat any
$g4 = comdat any
$g5 = comdat any
$_ZGV2v6 = comdat any

@v1 = linkonce_odr global [2 x i32] [i32 1, i32 2], comdat
@v2 = linkonce_odr hidden constant i32 2, comdat
@v3 = weak_odr protected global i32 0, comdat #0
@v4 = linkonce_odr global i32 0, comdat #1
@_ZGV2v4 = linkonce_odr global i64 0, comdat($v4)
@v5 = linkonce_odr thread_local global i32 0, comdat
@v6 = weak global i32 0, comdat
@_ZGV2v6 = linkonce_odr global i64 0, comdat
@v7 = common global i32 0
@_ZTI1A = linkonce_odr constant ptr @_ZTS1A, comdat #2
@_ZTS1A = linkonce_odr constant [3 x i8] c"1A\00", comdat
@w1 = extern_weak hidden global i32
@w2 = extern_weak global i32
@t1 = thread_local global i32 0, !tls_record !4
@t2 = thread_local global i32 0, !tls_record !5 #3
@t3 = thread_local global i32 0, !tls_record !6
@__tls_set = linkonce_odr hidden thread_local constant i8 1
@__tls_guard = internal thread_local global i8 0

@a1 = alias i32, ptr @v1
@a2 = hidden alias i32, ptr @v4
@a3 = alias i32, getelementptr (i32, ptr @v1, i64 1)

define linkonce_odr void @g1() comdat {
  ret void
}

define linkonce_odr void @g2() local_unnamed_addr comdat {
  ret void
}

define linkonce_odr hidden void @g3() comdat {
  ret void
}

define weak_odr protected void @g4() comdat {
  ret void
}

define weak void @g5() comdat {
  ret void
}

declare extern_weak protected void @w3()

attributes #0 = { "dynamic-init"="guarded" "gnu-unique" }
attributes #1 = { "dynamic-init"="unordered" "gnu-unique" }
attributes #2 = { "type-info" }
attributes #3 = { "dynamic-init"="thread-local" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 305419896}
!4 = !{ptr @__tls_set}
!5 = !{ptr @__tls_guard}
!6 = !{null}
