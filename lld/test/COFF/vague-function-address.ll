; REQUIRES: x86, aarch64

;; An offered vague function's address in an image defining it is the DLL's
;; (loaded from the entry, slots for data words), while calls stay direct to
;; the image's copy, dropped once nothing branches to it.

; RUN: rm -rf %t && split-file %s %t && cd %t

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
; DEFINE:   lld-link -dll -noentry dll.obj -out:f.dll -implib:f.lib && \
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
