; RUN: opt -passes=coff-output-locality -S %s | FileCheck %s
; RUN: llc -filetype=obj %s -o %t.obj
; RUN: llvm-readobj --sections --relocations %t.obj | FileCheck %s --check-prefix=OBJECT

target triple = "x86_64-unknown-windows-itanium"

@local = global i32 1, partition "pe:"
@foreign = global i32 2, partition "pe:child"
@image = global i32 3, partition "image:"
@external = external constant i32, partition "dll:owner.dll"

; Reuse one acquisition across dominated blocks. An image-local runtime
; object remains direct in either output, including after cross-image inlining.
; CHECK-LABEL: define dso_local i32 @read(
; CHECK: [[P:%.*]] = load ptr, ptr @"\01__imp_foreign", align 8, !invariant.load
; CHECK: load i32, ptr [[P]]
; CHECK: load i32, ptr @image
; CHECK: load i32, ptr @local
; CHECK: next:
; CHECK-NOT: load ptr
; CHECK: load i32, ptr [[P]]
define i32 @read(i1 %condition) partition "pe:" {
entry:
  %a = load i32, ptr @foreign
  %b = load i32, ptr @image
  %c = load i32, ptr @local
  br i1 %condition, label %next, label %done
next:
  %d = load i32, ptr @foreign
  ret i32 %d
done:
  %ab = add i32 %a, %b
  %abc = add i32 %ab, %c
  ret i32 %abc
}

; PHI operands acquire their address in the incoming block, never among PHIs.
; CHECK-LABEL: define dso_local ptr @choose(
; CHECK: left:
; CHECK: [[E:%.*]] = load ptr, ptr @"\01__imp_external", align 8, !invariant.load
; CHECK: br label %join
; CHECK: join:
; CHECK-NEXT: %p = phi ptr [ [[E]], %left ], [ @foreign, %right ]
define ptr @choose(i1 %condition) partition "pe:child" {
entry:
  br i1 %condition, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %p = phi ptr [ @external, %left ], [ @foreign, %right ]
  ret ptr %p
}

; One registration contribution per output/priority. Registration placement
; does not add relocations or roots to the compact link-only sidecar.
@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 65535, ptr @init, ptr null },
  { i32, ptr, ptr } { i32 65535, ptr @child_init, ptr null }]
define internal void @init() partition "pe:" { ret void }
define internal void @child_init() partition "pe:child" { ret void }

; OBJECT-COUNT-2: Name: .CRT$XCU
; OBJECT: Name: .llvm.place
; OBJECT: RelocationCount: 0
; OBJECT-NOT: Name: .llvm.part

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.output-set", i32 1}
