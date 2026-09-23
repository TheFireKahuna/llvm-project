; RUN: split-file %s %t
; RUN: opt -passes='cgscc(argpromotion)' -S %t/arguments.ll | FileCheck %s --check-prefix=ARGS
; RUN: opt -passes=mergefunc -S %t/merge.ll | FileCheck %s --check-prefix=MERGE
; RUN: opt -passes='coff-abi-requirements,ipsccp' -S %t/inferred.ll | FileCheck %s --check-prefix=INFER --implicit-check-not='load i32'
; RUN: %python %t/reverse.py %t/inferred.ll > %t/reversed.ll
; RUN: opt -passes='coff-abi-requirements,ipsccp' -S %t/reversed.ll | FileCheck %s --check-prefix=INFER --implicit-check-not='load i32'

; A load moved out of a function must carry its original layout assumptions,
; including when this transformation is run without the standard pipeline.
; ARGS: define internal i32 @read(i32 %p.0.val) !coff.abi.uses ![[ARGS:[0-9]+]]
; ARGS: define i32 @caller(ptr %p) !coff.abi.uses ![[ARGS]]
; ARGS: load i32, ptr %p
; ARGS: ![[ARGS]] = !{![[USE:[0-9]+]]}
; ARGS: ![[USE]] = !{ptr @layout, !"original-layout"}

; The body and its new thunk need the combined assumptions. A rejected merge
; of tiny functions must not add the other function's witness to either one.
; MERGE: define i32 @first(i32 %x) !coff.abi.uses ![[BOTH:[0-9]+]]
; MERGE: define weak i32 @tiny_first() !coff.abi.uses ![[FIRST:[0-9]+]]
; MERGE: define weak i32 @tiny_second() !coff.abi.uses ![[SECOND:[0-9]+]]
; MERGE: define i32 @second(i32 %0) !coff.abi.uses ![[BOTH]]
; MERGE: tail call i32 @first
; MERGE-DAG: ![[BOTH]] = !{![[A:[0-9]+]], ![[B:[0-9]+]]}
; MERGE-DAG: ![[FIRST]] = !{![[A]]}
; MERGE-DAG: ![[SECOND]] = !{![[B]]}
; MERGE-DAG: ![[A]] = !{ptr @layout_a, !"layout-a"}
; MERGE-DAG: ![[B]] = !{ptr @layout_b, !"layout-b"}

; The initial graph reaches entry, where the function pointer is supplied,
; but cannot see the eventual call edge inside dispatch. IPSCCP discovers it.
; Preserve the contract before replacing the indirect call. The same inference
; exposes a constant interior data address whose load is folded away.
; The result must not depend on the order functions are simplified.
; INFER-DAG: define internal i32 @dispatch(ptr %fp) !coff.abi.uses ![[USES:[0-9]+]]
; INFER-DAG: call i32 @implementation()
; INFER-DAG: define internal i32 @read_interior(ptr %p) !coff.abi.uses ![[USES]]
; INFER-NOT: load i32
; INFER: ![[USES]] = !{![[USE:[0-9]+]]}
; INFER: ![[USE]] = !{ptr @layout, !"original-layout"}

;--- arguments.ll
target triple = "x86_64-pc-windows-itanium"
@layout = external constant i8
define internal i32 @read(ptr %p) !coff.abi.uses !0 {
  %v = load i32, ptr %p
  ret i32 %v
}
define i32 @caller(ptr %p) {
  %v = call i32 @read(ptr %p)
  ret i32 %v
}
!0 = !{!1}
!1 = !{ptr @layout, !"original-layout"}

;--- merge.ll
target triple = "x86_64-pc-windows-itanium"
@layout_a = external constant i8
@layout_b = external constant i8
define i32 @first(i32 %x) !coff.abi.uses !0 {
  %v = add i32 %x, 3
  ret i32 %v
}
define i32 @second(i32 %x) !coff.abi.uses !1 {
  %v = add i32 %x, 3
  ret i32 %v
}
define weak i32 @tiny_first() !coff.abi.uses !0 {
  ret i32 1
}
define weak i32 @tiny_second() !coff.abi.uses !1 {
  ret i32 1
}
!0 = !{!2}
!1 = !{!3}
!2 = !{ptr @layout_a, !"layout-a"}
!3 = !{ptr @layout_b, !"layout-b"}

;--- inferred.ll
target triple = "x86_64-pc-windows-itanium"
@layout = external constant i8
@table = internal constant [2 x i32] [i32 11, i32 22], !coff.abi.uses !0
define internal i32 @dispatch(ptr %fp) {
  %v = call i32 %fp()
  ret i32 %v
}
define internal i32 @implementation() nounwind memory(none) !coff.abi.uses !0 {
  ret i32 37
}
define internal i32 @read_interior(ptr %p) {
  %v = load i32, ptr %p
  ret i32 %v
}
define i32 @entry() {
  %v = call i32 @dispatch(ptr @implementation)
  %w = call i32 @read_interior(ptr getelementptr ([2 x i32], ptr @table, i32 0, i32 1))
  %r = add i32 %v, %w
  ret i32 %r
}
!0 = !{!1}
!1 = !{ptr @layout, !"original-layout"}

;--- reverse.py
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
functions = list(re.finditer(r"^define .*?^}\n", source, re.M | re.S))
print(source[:functions[0].start()] +
      "\n".join(f.group() for f in reversed(functions)) +
      source[functions[-1].end():])
