; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,X86
; RUN: %if aarch64-registered-target %{ llc -mtriple=aarch64-pc-windows-ntposix < %s | FileCheck %s %}

; Every name of a function passed its thread pointer has both entries: X$tp,
; which a call passing the thread pointer names, and the plain entry X, which
; every other call and every address names. An alias, an alias of an alias
; and a weak alias name the entry of their aliasee of the same kind. A weak
; external, a destructor whose base destructor another unit defines, defines
; both of its names, each falling back to its alternate's entry of the same
; kind, so that the image defines every name once wherever the alternate is.
; What llvm.used keeps, it keeps with its plain entry.

@tv = thread_local global i32 0

define void @f() "thread-pointer-argument" "thread-pointer-unused" {
  ret void
}
@a = unnamed_addr alias void (), ptr @f
@aa = unnamed_addr alias void (), ptr @a

$g = comdat any
define linkonce_odr void @g() "thread-pointer-argument" "thread-pointer-unused" comdat {
  ret void
}
@w = weak_odr unnamed_addr alias void (), ptr @g

define void @h() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @tv)
  store i32 1, ptr %p
  ret void
}
@ah = unnamed_addr alias void (), ptr @h
@aah = unnamed_addr alias void (), ptr @ah

declare extern_weak dso_local void @e() "thread-pointer-argument" "weak-alternate"="base"
declare extern_weak dso_local dllexport void @ee() "thread-pointer-argument" "weak-alternate"="e"

@llvm.used = appending global [2 x ptr] [ptr @aa, ptr @e], section "llvm.metadata"
@addr = global ptr @ee

; X86-LABEL: holds$tp:
; X86:       callq e$tp
define void @holds() "thread-pointer-argument" {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @tv)
  store i32 1, ptr %p
  call void @e()
  ret void
}

; X86-LABEL: none$tp:
; X86:       jmp e # TAILCALL
define void @none() "thread-pointer-argument" "thread-pointer-unused" {
  tail call void @e()
  ret void
}

; CHECK-LABEL: addr:
; CHECK-NEXT:  {{.quad|.xword}} ee{{$}}

; CHECK:       .ascii " -export:ee$tp"
; CHECK:       .ascii " -export:\"ee\""
; CHECK:       .ascii " /INCLUDE:aa$tp"
; CHECK:       .ascii " /INCLUDE:e$tp"
; CHECK:       .ascii " /INCLUDE:\"aa\""
; CHECK:       .ascii " /INCLUDE:\"e\""
; CHECK:       {{^}}a$tp = f$tp{{$}}
; CHECK:       {{^}}aa$tp = a$tp{{$}}
; CHECK:       .weak w$tp{{$}}
; CHECK:       {{^}}w$tp = g$tp{{$}}
; CHECK:       {{^}}ah$tp = h$tp{{$}}
; CHECK:       {{^}}aah$tp = ah$tp{{$}}
; CHECK:       {{^}}f = f$tp{{$}}
; CHECK:       .weak g{{$}}
; CHECK:       {{^}}g = g$tp{{$}}
; CHECK:       {{^}}a = f{{$}}
; CHECK:       {{^}}aa = a{{$}}
; CHECK:       .weak w{{$}}
; CHECK:       {{^}}w = g{{$}}
; CHECK:       {{^}}ah = h{{$}}
; CHECK:       {{^}}aah = ah{{$}}
; CHECK:       .weak e$tp{{$}}
; CHECK:       {{^}}e$tp = base$tp{{$}}
; CHECK:       .weak ee$tp{{$}}
; CHECK:       {{^}}ee$tp = e$tp{{$}}
; CHECK:       .weak e{{$}}
; CHECK:       {{^}}e = base{{$}}
; CHECK:       .weak ee{{$}}
; CHECK:       {{^}}ee = e{{$}}

declare ptr @llvm.threadlocal.address.p0(ptr)
