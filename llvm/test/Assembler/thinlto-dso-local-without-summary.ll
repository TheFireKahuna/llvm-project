;; A value without a summary that the linker resolved to a definition in the
;; linkage unit round-trips through the bitcode index, which keeps no other
;; value without a summary.
; RUN: llvm-as %s -o - | llvm-dis -o - | FileCheck %s

^0 = module: (path: "main.o", hash: (0, 0, 0, 0, 0))
^1 = gv: (guid: 1, summaries: (function: (module: ^0, flags: (linkage: external, visibility: default, notEligibleToImport: 0, live: 0, dsoLocal: 1), insts: 1, calls: ((callee: ^2)), refs: (^3, ^4))))
^2 = gv: (guid: 2, dsoLocal: 1)
^3 = gv: (guid: 3)
^4 = gv: (guid: 4, dsoLocal: 1)

; CHECK-NOT: guid: 3
; CHECK:     = gv: (guid: 2, dsoLocal: 1)
; CHECK-NOT: guid: 3
; CHECK:     = gv: (guid: 4, dsoLocal: 1)
; CHECK-NOT: guid: 3
