; Test the KCFI membership tags of a combined summary: the tag the thin link
; gave a function and the tags of a type resolved by membership.
; RUN: llvm-as %s -o %t.bc
; RUN: llvm-dis %t.bc -o - | FileCheck %s
; RUN: llvm-bcanalyzer -dump %t.bc | FileCheck %s --check-prefix=BC

^0 = module: (path: "a.o", hash: (0, 0, 0, 0, 0))
^1 = gv: (guid: 1, summaries: (function: (module: ^0, flags: (linkage: external, visibility: default, notEligibleToImport: 0, live: 1, dsoLocal: 1, canAutoHide: 0, importType: definition, noRenameOnPromotion: 0), insts: 1, typeIdInfo: (typeTests: (^3)))), kcfiMemberTag: 7)
^2 = gv: (guid: 2, summaries: (function: (module: ^0, flags: (linkage: external, visibility: default, notEligibleToImport: 0, live: 1, dsoLocal: 1, canAutoHide: 0, importType: definition, noRenameOnPromotion: 0), insts: 1)))
^3 = typeid: (name: "_ZTSFvvE", summary: (typeTestRes: (kind: members, sizeM1BitWidth: 0, memberTags: (7, 9))))

; CHECK: ^1 = gv: (guid: 1, summaries: (function: ({{.*}})), kcfiMemberTag: 7)
; CHECK: ^2 = gv: (guid: 2, summaries: (function: ({{.*}})))
; CHECK: ^3 = typeid: (name: "_ZTSFvvE", summary: (typeTestRes: (kind: members, sizeM1BitWidth: 0, memberTags: (7, 9)))) ; guid = 9080559750644022485

; BC: <KCFI_MEMBER_TAGS op0=1 op1=7/>
; BC: <TYPE_ID_MEMBER_TAGS op0={{[0-9]+}} op1=8 op2=7 op3=9/>
