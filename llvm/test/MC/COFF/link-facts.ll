
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s \
; RUN:   | FileCheck %s --check-prefix=ASM --implicit-check-not=.link_fact
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

;; Neither a declaration nor a function of another environment has a fact.
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s \
; RUN:   | FileCheck %s --allow-empty --implicit-check-not=.link_fact
; RUN: opt -mtriple=x86_64-pc-windows-msvc %s -o %t.msvc.bc
; RUN: llvm-lto2 dump-symtab %t.msvc.bc \
; RUN:   | FileCheck %s --check-prefix=NONE --implicit-check-not="link facts"

;; The fact is stated where the prefix is emitted: x86 emits one only in a
;; module with the kcfi flag, and AArch64 in either.
; RUN: sed 's/!{!0, !1}/!{!1}/' %s > %t.nokcfi.ll
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %t.nokcfi.ll | FileCheck %s \
; RUN:   --check-prefix=NOKCFI --implicit-check-not=.link_fact \
; RUN:   --implicit-check-not=__cfi_
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-unknown-windows-ntposix < %t.nokcfi.ll \
; RUN:   | FileCheck %s --check-prefixes=ASM,VAGUE,PREFIX --implicit-check-not=.link_fact %}
; RUN:   | FileCheck %s --check-prefixes=ASM,PREFIX --implicit-check-not=.link_fact
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
; OBJ-NOT:    Fact

;; f1 is symbol 0, f3 symbol 2 and f4 symbol 3: each fact is the index, the
;; tag, the size of its operands and the operands.
; SYMTAB:      version: 5
; SYMTAB:      link facts: 000001000200010003000101
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

!llvm.module.flags = !{!0, !1}
!0 = !{i32 4, !"kcfi", i32 1}
!1 = !{i32 1, !"function-type-prefix", i32 16}
!2 = !{i32 12345678}
!3 = !{i32 305419896}
