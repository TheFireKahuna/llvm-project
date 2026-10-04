;; A reference whose definition no summary describes, such as one in a native
;; object, is made dso_local in the backend when the linker resolves every
;; reference to it to a definition in the linkage unit.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: opt -module-hash -module-summary main.ll -o main.bc
; RUN: opt -module-summary other.ll -o other.bc

;; native_fn and native_var are final in both modules. imported is not, and
;; mixed is final in main.bc only.
; RUN: llvm-lto2 run main.bc other.bc -o out -save-temps \
; RUN:   -r=main.bc,use,plx -r=main.bc,__imp_native_fn,l \
; RUN:   -r=main.bc,native_var,l -r=main.bc,__imp_imported, \
; RUN:   -r=main.bc,mixed,l \
; RUN:   -r=other.bc,use2,plx -r=other.bc,native_fn,l -r=other.bc,__imp_mixed,
; RUN: llvm-dis out.1.3.import.bc -o - | FileCheck %s

; CHECK-DAG: @native_var = external dso_local global i32
; CHECK-DAG: declare dso_local void @native_fn()
; CHECK-DAG: declare dllimport void @imported()
; CHECK-DAG: declare void @mixed()

;; Distributed backends receive the values in their index files.
; RUN: llvm-lto2 run main.bc other.bc -o dist -thinlto-distributed-indexes \
; RUN:   -r=main.bc,use,plx -r=main.bc,__imp_native_fn,l \
; RUN:   -r=main.bc,native_var,l -r=main.bc,__imp_imported, \
; RUN:   -r=main.bc,mixed,l \
; RUN:   -r=other.bc,use2,plx -r=other.bc,native_fn,l -r=other.bc,__imp_mixed,
; RUN: llvm-dis main.bc.thinlto.bc -o - | FileCheck --check-prefix=INDEX %s
; RUN: llvm-bcanalyzer -dump main.bc.thinlto.bc | \
; RUN:   FileCheck --check-prefix=BC %s
; RUN: opt -passes=function-import -summary-file=main.bc.thinlto.bc main.bc \
; RUN:   -S -o - | FileCheck %s

; INDEX-COUNT-2: = gv: (guid: {{[0-9]+}}, dsoLocal: 1)
; INDEX-NOT:     dsoLocal: 1)
; BC: <DSO_LOCAL_WITHOUT_SUMMARY op0={{-?[0-9]+}} op1={{-?[0-9]+}}/>

;; The values join the cache key of the modules that reference them.
; RUN: llvm-lto2 run main.bc -o cached -cache-dir cache \
; RUN:   -r=main.bc,use,plx -r=main.bc,__imp_native_fn,l \
; RUN:   -r=main.bc,native_var,l -r=main.bc,__imp_imported, -r=main.bc,mixed,l
; RUN: llvm-lto2 run main.bc -o cached -cache-dir cache \
; RUN:   -r=main.bc,use,plx -r=main.bc,__imp_native_fn, \
; RUN:   -r=main.bc,native_var,l -r=main.bc,__imp_imported, -r=main.bc,mixed,l
; RUN: ls cache | count 2

;--- main.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare dllimport void @native_fn()
@native_var = external global i32
declare dllimport void @imported()
declare void @mixed()

define i32 @use() {
  call void @native_fn()
  call void @imported()
  call void @mixed()
  %v = load i32, ptr @native_var
  ret i32 %v
}

;--- other.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare void @native_fn()
declare dllimport void @mixed()

define void @use2() {
  call void @native_fn()
  call void @mixed()
  ret void
}
