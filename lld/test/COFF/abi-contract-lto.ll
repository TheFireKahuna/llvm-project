; REQUIRES: x86
; RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir

; A bitcode DLL provides its contracts through the linker options of its
; symbol table, which the link reads before LTO.
; RUN: llvm-as d.ll -o d.bc
; RUN: lld-link -import-slots -dll -noentry d.bc -out:d.dll
; RUN: llvm-readobj d.lib | FileCheck --check-prefix=IMPLIB %s
; IMPLIB: Export name: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw

; The requirements of bitcode are those of the code LTO emits.
; RUN: llvm-as main.ll -o main.bc
; RUN: opt -module-summary main.ll -o main.thin.bc
; RUN: lld-link -import-slots -entry:main -subsystem:console main.bc d.lib \
; RUN:   -out:main.exe
; RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=IMPORTS %s
; RUN: lld-link -import-slots -entry:main -subsystem:console main.thin.bc \
; RUN:   d.lib -out:main.thin.exe
; RUN: llvm-readobj --coff-imports main.thin.exe \
; RUN:   | FileCheck --check-prefix=IMPORTS %s
; IMPORTS:      Name: d.dll
; IMPORTS:      Symbol: __abiw$Z7E5CzMxeVE$AAECAwQFBgcICQoLDA0ODw (0)
; IMPORTS-NEXT: Symbol: g (1)

; RUN: llvm-as skew.ll -o skew.bc
; RUN: not lld-link -import-slots -entry:main -subsystem:console skew.bc \
; RUN:   d.lib -out:skew.exe 2>&1 | FileCheck --check-prefix=SKEW %s
; SKEW: error: ABI mismatch for 'T': {{.*}} was compiled with a different layout than d.dll exports (needed because {{.*}} uses g)

;--- d.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define dllexport void @g() {
  ret void
}

!llvm.linker.options = !{!0}
!0 = !{!"/abiprovide:Z7E5CzMxeVEAAECAwQFBgcICQoLDA0ODw"}

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

declare dllimport void @g()

define void @main() {
  call void @g()
  ret void
}

!abi.requirements = !{!0}
!0 = !{ptr @g, !1}
!1 = !{!"1T", !"\00\01\02\03\04\05\06\07\08\09\0A\0B\0C\0D\0E\0F"}

;--- skew.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

declare dllimport void @g()

define void @main() {
  call void @g()
  ret void
}

!abi.requirements = !{!0}
!0 = !{ptr @g, !1}
!1 = !{!"1T", !"\F0\F1\F2\F3\F4\F5\F6\F7\F8\F9\FA\FB\FC\FD\FE\FF"}
