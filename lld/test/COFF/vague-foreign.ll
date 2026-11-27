; REQUIRES: x86, aarch64

;; Beside a fork object, a foreign object's vague entities are what its COMDATs
;; show (variables; functions its address-significance table lists): offered,
;; yielding to a DLL's, and their address-takes load from the import's entry.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{t} = x86_64
; DEFINE: %{fork} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{gnu} = llc -mtriple=%{t}-windows-gnu -filetype=obj -addrsig
; DEFINE: %{link} = lld-link -entry:main -subsystem:console -out:exe.exe \
; DEFINE:   -lldmap:exe.map
; DEFINE: %{imports} = llvm-readobj --coff-imports exe.exe | FileCheck %s
; DEFINE: %{run} = %{fork} dll.ll -o dll.obj && %{fork} main.ll -o main.obj && \
; DEFINE:   %{fork} bare.ll -o bare.obj && %{fork} big.ll -o forkbig.obj && \
; DEFINE:   %{gnu} gnu.ll -o gnu.obj && %{gnu} direct.ll -o direct.obj && \
; DEFINE:   %{gnu} big.ll -o big.obj && \
; DEFINE:   lld-link -dll -noentry dll.obj -out:a.dll -implib:a.lib && \
; DEFINE:   lld-link -dll -noentry bare.obj -out:b.dll -implib:b.lib && \
; DEFINE:   %{link} main.obj gnu.obj a.lib && %{imports} --check-prefix=ALL && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn exe.exe | \
; DEFINE:     FileCheck --check-prefix=%{t} %s && \
; DEFINE:   FileCheck --check-prefix=GONE --input-file=exe.map %s \
; DEFINE:     --implicit-check-not='$iv' --implicit-check-not='$_ZGV2iv' && \
; DEFINE:   %{link} main.obj gnu.obj direct.obj a.lib && \
; DEFINE:   %{imports} --check-prefix=FN --implicit-check-not=iv && \
; DEFINE:   %{link} main.obj gnu.obj a.lib -ignore-offers:A.dll && \
; DEFINE:   %{imports} --check-prefix=NONE --allow-empty && \
; DEFINE:   lld-link -entry:addr -subsystem:console -out:exe.exe gnu.obj \
; DEFINE:     a.lib && %{imports} --check-prefix=NONE --allow-empty && \
; DEFINE:   lld-link -dll -noentry main.obj gnu.obj -export:addr \
; DEFINE:     -out:g.dll -implib:g.lib && \
; DEFINE:   llvm-readobj --coff-exports g.dll | FileCheck %s \
; DEFINE:     --check-prefix=OFFERS --implicit-check-not=Name: && \
; DEFINE:   cat gnu.ll guard.ll | llc -mtriple=%{t}-windows-gnu -filetype=obj \
; DEFINE:     -o guarded.obj && \
; DEFINE:   lld-link -dll -noentry main.obj guarded.obj -export:addr \
; DEFINE:     -out:g.dll -implib:guarded.lib && \
; DEFINE:   llvm-readobj --coff-exports g.dll | FileCheck %s \
; DEFINE:     --check-prefix=OFFERS --implicit-check-not=Name: && \
; DEFINE:   cat gnu.ll tables.ll | llc -mtriple=%{t}-windows-gnu -filetype=obj \
; DEFINE:     -o tables.obj && \
; DEFINE:   lld-link -dll -noentry main.obj tables.obj -export:addr \
; DEFINE:     -out:g.dll -implib:tables.lib && \
; DEFINE:   llvm-readobj --coff-exports g.dll | FileCheck %s \
; DEFINE:     --check-prefixes=OFFERS,TABLES --implicit-check-not=Name: && \
; DEFINE:   %{fork} none.ll -o none.obj && llvm-lib gnu.obj -out:gnu.lib && \
; DEFINE:   lld-link -entry:ivaddr -subsystem:console -out:exe.exe none.obj \
; DEFINE:     gnu.lib a.lib -ignore-function-address-equality:gnu.lib && \
; DEFINE:   %{imports} --check-prefix=IGNORED && \
; DEFINE:   not %{link} main.obj big.obj a.lib 2>&1 | FileCheck %s \
; DEFINE:     --check-prefix=SIZE -DOBJ=big.obj -DN=12 -DM=8 -DDLL=a.dll && \
; DEFINE:   not %{link} forkbig.obj g.lib 2>&1 | FileCheck %s \
; DEFINE:     --check-prefix=SIZE -DOBJ=forkbig.obj -DN=12 -DM=8 -DDLL=g.dll && \
; DEFINE:   not %{link} main.obj gnu.obj b.lib 2>&1 | FileCheck %s \
; DEFINE:     --check-prefix=GUARD

; RUN: %{run}
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu refused.s -o refused.obj
; DEFINE: %{refused} = lld-link -entry:take -subsystem:console -out:exe.exe \
; DEFINE:   bare.obj refused.obj a.lib
; RUN: not %{refused} 2>&1 | FileCheck %s --check-prefix=REFUSED
; RUN: %{refused} -ignore-function-address-equality:refused.obj
; RUN: %{imports} --check-prefix=OWN --implicit-check-not='Symbol: f'
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; ALL: Symbol: _ZGV2iv (
; ALL: Symbol: f (
; ALL: Symbol: iv (

;; The image keeps a variable that the code of direct.obj reads in place.
; FN: Symbol: f (

; NONE-NOT: Symbol:

; OWN: Symbol: iv (

; REFUSED: error: refused.obj: the reference to f at .text+0x3 is not classified: no proven path reaches it, decoded from proven starts

;; addr calls the image's f and called and loads the DLL's f's address; ivaddr
;; and guardaddr load theirs.
; x86_64:      pushq %rsi
; x86_64:      callq
; x86_64-NEXT: movq {{.*}}(%rip), %rsi
; x86_64-NEXT: callq
; x86_64:      movq {{.*}}(%rip), %rax
; x86_64-NEXT: retq
; x86_64:      movq {{.*}}(%rip), %rax
; x86_64-NEXT: retq

; aarch64:      stp x19, x30
; aarch64-NEXT: bl
; aarch64-NEXT: adrp x19,
; aarch64-NEXT: ldr x19, [x19
; aarch64-NEXT: bl
; aarch64:      adrp x0,
; aarch64-NEXT: ldr x0, [x0
; aarch64:      adrp x0,
; aarch64-NEXT: ldr x0, [x0

;; The variable's and the guard's sections left the image.
; GONE: gnu.obj:(.text$f)

;; A DLL offers them but no only-called function, by its address-significance
;; or CFG table; an unguarded object's table proves nothing, so all are offered.
; OFFERS: Name: _ZGV2iv
; OFFERS: Name: addr
; TABLES: Name: called
; OFFERS: Name: f
; OFFERS: Name: iv

;; Ignoring an archive member's function addresses leaves its variables shared.
; IGNORED: Symbol: _ZGV2iv (
; IGNORED: Symbol: iv (

;; Copies that two producers made agree in what the link sees of them.
; SIZE: error: [[OBJ]]: iv has [[N]] bytes, and [[M]] in [[DLL]], which provides it; the two are of different producers

; GUARD: error: gnu.obj: iv is provided by b.dll, but guard variable for iv, which its COMDAT group also defines, is not; give both the visibility they have in b.dll

;--- dll.ll
$iv = comdat any
$f = comdat any

@iv = linkonce_odr global [2 x i32] zeroinitializer, comdat
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)

define linkonce_odr void @f() comdat {
  ret void
}

define dllexport ptr @get() {
  %a = load volatile i32, ptr @iv
  %b = load volatile i64, ptr @_ZGV2iv
  ret ptr @f
}

;--- bare.ll
$iv = comdat any

@iv = linkonce_odr global [2 x i32] zeroinitializer, comdat

define dllexport ptr @get() {
  ret ptr @iv
}

;--- gnu.ll
$iv = comdat any
$_ZGV2iv = comdat any
$f = comdat any
$called = comdat any

@iv = linkonce_odr dso_local global [2 x i32] zeroinitializer, comdat
@_ZGV2iv = linkonce_odr dso_local global i64 0, comdat
@fword = dso_local global ptr @f
@ivword = dso_local global ptr @iv

define linkonce_odr dso_local void @f() comdat {
  ret void
}

define linkonce_odr dso_local void @called() local_unnamed_addr comdat {
  ret void
}

define dso_local ptr @addr() {
  call void @f()
  call void @called()
  ret ptr @f
}

define dso_local ptr @ivaddr() {
  ret ptr @iv
}

define dso_local ptr @guardaddr() {
  ret ptr @_ZGV2iv
}

;--- guard.ll
!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 2}

;--- tables.ll
!llvm.module.flags = !{!0}
!0 = !{i32 2, !"cfguard", i32 1}

;--- none.ll
define void @none() {
  ret void
}

;--- refused.s
.section .text$f,"xr",discard,f
.def f; .scl 2; .type 32; .endef
.globl f
f:
  retq
.text
.globl take
take:
  leaq f(%rip), %rax
  retq

;--- direct.ll
$iv = comdat any

@iv = linkonce_odr dso_local global [2 x i32] zeroinitializer, comdat

define dso_local i32 @read() {
  %v = load i32, ptr @iv
  ret i32 %v
}

;--- big.ll
$iv = comdat any

@iv = linkonce_odr dso_local global [3 x i32] zeroinitializer, comdat

define dso_local ptr @addr() {
  ret ptr @iv
}

;--- main.ll
@iv = external global [2 x i32]

declare ptr @addr()
declare ptr @ivaddr()
declare ptr @guardaddr()

define i32 @main() {
  %a = call ptr @addr()
  %b = call ptr @ivaddr()
  %c = call ptr @guardaddr()
  %v = load volatile i32, ptr @iv
  ret i32 %v
}
