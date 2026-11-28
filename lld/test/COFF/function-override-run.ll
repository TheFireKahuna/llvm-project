; REQUIRES: x86, system-windows, target=x86_64-{{.*}}
; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llc -filetype=obj def.ll -o def.obj
; RUN: llc -filetype=obj use.ll -o use.obj
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows \
; RUN:   %S/Inputs/loadconfig-cfg-x64.s -o lc.obj
; RUN: lld-link -entry:entry -subsystem:console -nodefaultlib -out:run.exe \
; RUN:   def.obj use.obj lc.obj
; RUN: llvm-readobj --coff-load-config run.exe | FileCheck %s
; RUN: %t/run.exe

;; Windows selects the body of a multiversioned function of an object that
;; LLVM made for Windows Itanium, linked with no option: f's stub jumps to
;; f.default, and where the processor has ERMSB, capability 0, Windows
;; rewrites the stub's jump and each site to reach f.ermsb as it maps the
;; image. The sites are the direct call and tail call of f's unit and those
;; of another unit through f's pointer, which the link makes direct: a call,
;; a jump and a conditional jump. The image has no import and no start-up,
;; and exits with 0 where every one of them, and the call through f's
;; address, returns what CPUID says. No resolver runs.

; CHECK:         FunctionOverride [
; CHECK-COUNT-8:   Type: X64_REL32
; CHECK:           Value: 0x0

;--- def.ll
target triple = "x86_64-unknown-windows-itanium"

@f = ifunc i32 (), ptr @f.resolver
@p = global ptr @f
@unknown = internal global i1 false

define hidden ptr @f.resolver() {
  %c = load i1, ptr @unknown
  %x = select i1 %c, ptr @f.ermsb, ptr @f.default
  ret ptr %x
}

define hidden i32 @f.ermsb() "fmv-features"="ermsb" {
  ret i32 1
}

define hidden i32 @f.default() "fmv-features" {
  ret i32 0
}

define hidden i32 @tail() noinline {
  %r = tail call i32 @f()
  ret i32 %r
}

declare i32 @call()
declare i32 @jump()
declare i32 @cond(i32)

define i32 @entry() {
  %id = call { i32, i32, i32, i32 } asm "cpuid", "={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}"(i32 7, i32 0)
  %ebx = extractvalue { i32, i32, i32, i32 } %id, 1
  %shift = lshr i32 %ebx, 9
  %erms = and i32 %shift, 1
  %direct = call i32 @f()
  %tail = call i32 @tail()
  %fp = load volatile ptr, ptr @p
  %indirect = call i32 %fp()
  %call = call i32 @call()
  %jump = call i32 @jump()
  %cond = call i32 @cond(i32 1)
  %and1 = and i32 %direct, %tail
  %and2 = and i32 %and1, %indirect
  %and3 = and i32 %and2, %call
  %and4 = and i32 %and3, %jump
  %and = and i32 %and4, %cond
  %or1 = or i32 %direct, %tail
  %or2 = or i32 %or1, %indirect
  %or3 = or i32 %or2, %call
  %or4 = or i32 %or3, %jump
  %or = or i32 %or4, %cond
  %a = xor i32 %and, %erms
  %b = xor i32 %or, %erms
  %r = or i32 %a, %b
  ret i32 %r
}

;--- use.ll
target triple = "x86_64-unknown-windows-itanium"

declare i32 @f() nonlazybind

define i32 @call() noinline {
  %r = call i32 @f()
  %s = add i32 %r, 0
  ret i32 %s
}

define i32 @jump() noinline {
  %r = tail call i32 @f()
  ret i32 %r
}

define i32 @cond(i32 %x) noinline {
  %c = icmp eq i32 %x, 0
  br i1 %c, label %zero, label %other
zero:
  ret i32 2
other:
  %r = tail call i32 @f()
  ret i32 %r
}
