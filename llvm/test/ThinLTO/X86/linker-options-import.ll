; ThinLTO import leaves a module's linker options with that module: the
; importing module's object does not repeat them.

; RUN: opt -module-summary %s -o %t1.bc
; RUN: opt -module-summary %p/Inputs/linker-options-import.ll -o %t2.bc
; RUN: llvm-lto -thinlto-action=thinlink -o %t.index.bc %t1.bc %t2.bc

; RUN: llvm-lto -thinlto-action=import %t1.bc -thinlto-index=%t.index.bc -o - | llvm-dis -o - | FileCheck %s --check-prefix=MAIN
; RUN: llvm-lto -thinlto-action=import %t2.bc -thinlto-index=%t.index.bc -o - | llvm-dis -o - | FileCheck %s --check-prefix=FOO

; MAIN: define available_externally void @foo()
; MAIN: !llvm.linker.options = !{![[#MAIN:]]}
; MAIN: ![[#MAIN]] = !{!"/include:main_option"}
; MAIN-NOT: foo_option

; FOO: !llvm.linker.options = !{![[#FOO:]]}
; FOO: ![[#FOO]] = !{!"/include:foo_option"}
; FOO-NOT: main_option

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @main() {
entry:
  call void @foo()
  ret i32 0
}

declare void @foo()

!llvm.linker.options = !{!0}
!0 = !{!"/include:main_option"}
