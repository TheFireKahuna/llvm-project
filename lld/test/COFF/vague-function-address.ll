; REQUIRES: x86, aarch64

;; An offered vague function's address in an image defining it is the DLL's
;; (loaded from the entry, slots for data words), while calls stay direct to
;; the image's copy, dropped once nothing branches to it.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %S/Inputs/fill-entry.s -o fill-x86_64.obj
; RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium %S/Inputs/fill-entry.s -o fill-aarch64.obj

; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{link} = lld-link -entry:main -subsystem:console -out:exe.exe \
; DEFINE:   -lldmap:exe.map
; DEFINE: %{imports} = llvm-readobj --coff-imports exe.exe | \
; DEFINE:   FileCheck --check-prefix=IMPORTS %s
; DEFINE: %{check} = llvm-objdump -d --no-show-raw-insn exe.exe | \
; DEFINE:     FileCheck --check-prefix=%{t} %s && \
; DEFINE:   %{imports} && llvm-readobj --coff-load-config exe.exe | \
; DEFINE:     FileCheck --check-prefix=GUARD %s
; DEFINE: %{run} = %{llc} f.ll -o f.obj && %{llc} dll.ll -o dll.obj && \
; DEFINE:   %{llc} nocall.ll -o nocall.obj && %{llc} rel.ll -o rel.obj && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium f.ll -o f.bc && \
; DEFINE:   llvm-mc -triple=%{t}-windows-itanium -filetype=obj \
; DEFINE:     %S/Inputs/loadconfig-cfg-iat.s -o config.obj && \
; DEFINE:   lld-link -dll -noentry fill-%{t}.obj dll.obj -out:f.dll -implib:f.lib && \
; DEFINE:   %{link} -guard:cf f.obj f.lib config.obj && %{check} && \
; DEFINE:   %{link} -guard:cf f.lib f.obj config.obj && %{check} && \
; DEFINE:   %{link} f.bc f.lib -include:addr && %{imports} && \
; DEFINE:   %{link} f.lib nocall.obj && %{imports} && \
; DEFINE:   FileCheck --check-prefix=NOCALL --input-file=exe.map %s \
; DEFINE:     --implicit-check-not=fn && \
; DEFINE:   %{link} f.obj f.lib -delayload:f.dll && \
; DEFINE:   llvm-readobj --coff-imports exe.exe | \
; DEFINE:     FileCheck --check-prefix=NONE --allow-empty %s && \
; DEFINE:   not %{link} -opt:noref f.obj rel.obj f.lib 2>&1 | \
; DEFINE:     FileCheck --check-prefix=REL %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

;; On AArch64 the code of a weakly defined function's address puts the adrp
;; directly before its add on one register, whichever registers allocation
;; gave the two, so the link can make the pair a load of the entry.
; RUN: %{llc} pair.ll -o pair.obj
; RUN: %{link} pair.obj f.lib
; RUN: llvm-objdump -d --no-show-raw-insn exe.exe | FileCheck --check-prefix=PAIR %s
; PAIR:      str x8, [x24]
; PAIR-NEXT: ldr x9, 0x

; x86_64:      movq {{.*}}(%rip), %rax
; x86_64-NEXT: retq
; x86_64:      callq 0x[[FN:[0-9a-f]+]]
; x86_64:      [[FN]]: movl $0x7, %eax

; aarch64:      ldr x0, 0x
; aarch64-NEXT: ret
; aarch64:      bl 0x[[FN:[0-9a-f]+]]
; aarch64:      [[FN]]: mov w0, #0x7

;; The entry in the import address table, which the code loads, and the slot.
; IMPORTS:     Name: f.dll
; IMPORTS:     Symbol: fn (
; IMPORTS-NOT: Symbol:
; IMPORTS:     Name: f.dll
; IMPORTS:     Symbol: fn (

;; Only the entry point is a call target, and both pointers hold one.
; GUARD:      GuardFidTable [
; GUARD-NEXT:   0x
; GUARD-NEXT: ]
; GUARD-NEXT: GuardIatTable [
; GUARD-NEXT:   0x
; GUARD-NEXT:   0x
; GUARD-NEXT: ]

; NOCALL: main

;; A DLL that the image loads on demand gives no address at the link.
; NONE-NOT: Symbol:

;; A reference that is neither a call nor an address with room for the load or
;; the loader's word is an error.
; REL: error: rel.obj: .data+0x0: IMAGE_REL_{{.*}}_REL32 needs an address for fn, which f.dll provides

;; A function passed its thread pointer is named by fn, its plain entry and
;; its address, and by fn$tp, the entry that callers passing the pointer call;
;; where it never reads the pointer, fn is an alias of fn$tp's body. The offer
;; of fn binds every address-take, and the calls stay with the image's copy,
;; which is no call target and whose fn$tp the image imports from no one.
;; A plugin's fill finds the function by that name too.
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj tp.ll -o tp.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj tpdll.ll -o tpdll.obj
; RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj \
; RUN:   %S/Inputs/loadconfig-cfg-iat.s -o config-x86_64.obj
; RUN: lld-link -dll -noentry fill-x86_64.obj tpdll.obj -out:tp.dll -implib:tp.lib
; RUN: llvm-readobj --coff-link-records tp.lib | FileCheck --check-prefix=TP-KEPT %s
; RUN: lld-link -entry:main -subsystem:console -out:tp.exe -guard:cf tp.obj \
; RUN:   tp.lib config-x86_64.obj
; RUN: llvm-objdump -d --no-show-raw-insn tp.exe | FileCheck --check-prefix=TP %s
; RUN: llvm-readobj --coff-imports --coff-load-config tp.exe | \
; RUN:   FileCheck --check-prefix=TP-IMPORTS %s

; TP:      movq {{.*}}(%rip), %rax
; TP-NEXT: retq
; TP:      callq 0x[[FN:[0-9a-f]+]]
; TP:      [[FN]]: movl $0x7, %eax

; TP-IMPORTS:     Name: tp.dll
; TP-IMPORTS:     Symbol: fn (
; TP-IMPORTS-NOT: Symbol:
; TP-IMPORTS:     GuardFidTable [
; TP-IMPORTS-NEXT:  0x
; TP-IMPORTS-NEXT: ]

;; The DLL keeps the function under the name of its address, fn.
; TP-KEPT:      Entity {
; TP-KEPT-NEXT:   Name: fn{{$}}
; TP-KEPT-NOT:  Entity {

;--- f.ll
$fn = comdat any

@fp = global ptr @fn

define linkonce_odr i32 @fn() comdat {
  ret i32 7
}

define ptr @addr() {
  ret ptr @fn
}

define i32 @main() {
  %a = call i32 @fn()
  %p = call ptr @addr()
  %q = load volatile ptr, ptr @fp
  ret i32 %a
}

define void @__delayLoadHelper2() {
  ret void
}

;--- dll.ll
$fn = comdat any

define linkonce_odr i32 @fn() comdat {
  ret i32 7
}

define dllexport ptr @get() {
  ret ptr @fn
}

;--- nocall.ll
$fn = comdat any

@fp = global ptr @fn

define linkonce_odr i32 @fn() comdat {
  ret i32 7
}

define ptr @main() {
  %q = load volatile ptr, ptr @fp
  ret ptr @fn
}

;--- rel.ll
$fn = comdat any

@rel = global i32 trunc (i64 sub (i64 ptrtoint (ptr @fn to i64), i64 ptrtoint (ptr @rel to i64)) to i32)

define linkonce_odr i32 @fn() comdat {
  ret i32 7
}

;--- pair.ll
$fn = comdat any

define linkonce_odr i32 @fn() comdat {
  ret i32 7
}

define void @main(ptr %p, i64 %n, ptr %q, i64 %m, ptr %s, i64 %k) {
entry:
  %buf = alloca [64 x i8], align 8
  br label %loop

loop:
  %c = phi ptr [ null, %entry ], [ %next, %loop ]
  %i = load i64, ptr null, align 1
  %next = getelementptr i8, ptr %q, i64 %m
  call void @g()
  %e = getelementptr i8, ptr %s, i64 %i
  store i8 0, ptr %e, align 1
  %a = ptrtoint ptr %c to i64
  %o = or i64 %a, %k
  %u = call i64 @llvm.umin.i64(i64 %o, i64 %n)
  store i64 %u, ptr %p, align 8
  %f = getelementptr i8, ptr %buf, i64 48
  store ptr @fn, ptr %f, align 8
  %b = getelementptr i8, ptr %buf, i64 56
  store i64 1, ptr %b, align 8
  br label %loop
}

define void @g() {
  ret void
}

;--- tp.ll
$fn = comdat any

define linkonce_odr i32 @fn() "thread-pointer-argument" "thread-pointer-unused" comdat {
  ret i32 7
}

define ptr @addr() "thread-pointer-argument" "thread-pointer-unused" {
  ret ptr @fn
}

define i32 @main() "thread-pointer-argument" "thread-pointer-unused" {
  %a = call i32 @fn()
  %p = call ptr @addr()
  ret i32 %a
}

;--- tpdll.ll
$fn = comdat any

define linkonce_odr i32 @fn() "thread-pointer-argument" "thread-pointer-unused" comdat {
  ret i32 7
}

define dllexport ptr @get() "thread-pointer-argument" "thread-pointer-unused" {
  ret ptr @fn
}
