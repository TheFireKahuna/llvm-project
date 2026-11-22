;; A function with !overridable is one that a program replaces for every
;; image, or a definition that replaces one. Each of its names states its
;; visibility, tag 1, and that it is one, tag 9; the name of a default also
;; states the default's slot in the override table, then the slot of the
;; default it is specified as calling, then 1 where that one takes its
;; arguments without the second. Any other weak definition of a function that
;; is not ODR and has default visibility is replaced by its name, tag 11.

; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s \
; RUN:   | FileCheck %s --implicit-check-not=.link_fact
; RUN: llc -mtriple=aarch64-unknown-windows-ntposix -filetype=obj %s -o %t.o
; RUN: llvm-readobj --coff-link-records %t.o | FileCheck %s --check-prefix=OBJ
; RUN: opt -mtriple=x86_64-unknown-windows-itanium %s -o %t.bc
; RUN: llvm-lto2 dump-symtab %t.bc | FileCheck %s --check-prefix=SYMTAB
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s \
; RUN:   | FileCheck %s --check-prefix=NONE --implicit-check-not=.link_fact
; NONE: replacement:

; CHECK:      .link_fact replacement, 1, 1
; CHECK-NEXT: .link_fact replacement, 9
; CHECK-NEXT: .link_fact private, 1, 3
; CHECK-NEXT: .link_fact private, 9
; CHECK-NEXT: .link_fact slot, 1, 1
; CHECK-NEXT: .link_fact slot, 9, 4
; CHECK-NEXT: .link_fact forward, 1, 1
; CHECK-NEXT: .link_fact forward, 9, 5, 4
; CHECK-NEXT: .link_fact drop, 1, 1
; CHECK-NEXT: .link_fact drop, 9, 6, 5, 1
; CHECK-NEXT: .link_fact byname, 11
;; An alias is another name of the function and not of its slot. A weak alias
;; is not replaced by its name.
; CHECK-NEXT: .link_fact alias, 1, 1
; CHECK-NEXT: .link_fact alias, 9

; OBJ:        Symbol: replacement ({{.*}})
; OBJ-NEXT:   Tag: Visibility (0x1)
; OBJ:        Symbol: replacement ({{.*}})
; OBJ-NEXT:   Tag: Overridable (0x9)
; OBJ-NEXT:   Operands (
; OBJ-NEXT:   )
; OBJ:        Symbol: drop ({{.*}})
; OBJ:        Symbol: drop ({{.*}})
; OBJ-NEXT:   Tag: Overridable (0x9)
; OBJ-NEXT:   Operands (
; OBJ-NEXT:     0000: 060501
; OBJ-NEXT:   )
; OBJ:        Symbol: byname ({{.*}})
; OBJ-NEXT:   Tag: OverridableByName (0xB)

;; Each fact is the symbol's index, the tag, the size of its operands and the
;; operands.
; SYMTAB: link facts: 0001010100090001010103010900020101010209010403010101030902050404010101040903060501050B00{{.*}}

define void @replacement() !overridable !0 {
  ret void
}

define hidden void @private() !overridable !0 {
  ret void
}

define weak void @slot() !overridable !1 {
  ret void
}

define weak void @forward() !overridable !2 {
  ret void
}

define weak void @drop() !overridable !3 {
  ret void
}

define weak void @byname() {
  ret void
}

define weak hidden void @hidden() {
  ret void
}

define linkonce void @discardable() {
  ret void
}

@alias = alias void (), ptr @slot
@weakalias = weak alias void (), ptr @hidden

!0 = !{}
!1 = !{i32 4}
!2 = !{i32 5, i32 4}
!3 = !{i32 6, i32 5, i32 1}
