; REQUIRES: x86, aarch64
; RUN: split-file %s %t
; DEFINE: %{arch} = x86_64
; DEFINE: %{link} = llc -mtriple=%{arch}-unknown-windows-itanium \
; DEFINE:     -function-splitting=all -mfs-split-ehcode \
; DEFINE:     -function-sections -filetype=obj %t/a.ll -o %t/a.obj && \
; DEFINE:   llvm-mc -triple=%{arch}-unknown-windows-itanium -filetype=obj \
; DEFINE:     %t/b.s -o %t/b.obj && \
; DEFINE:   lld-link -entry:live -include:caught -include:fold1 \
; DEFINE:     -include:fold2 -include:fold3 -subsystem:console -opt:ref \
; DEFINE:     -map:%t/a.map -out:%t/a.exe %t/a.obj %t/b.obj && \
; DEFINE:   llvm-objdump -s -j .rdata %t/a.exe
; DEFINE: %{parts} = cat %t/a.map > %t/parts && \
; DEFINE:   llvm-readobj --unwind %t/a.exe >> %t/parts && \
; DEFINE:   FileCheck %s --input-file=%t/parts
; RUN: %{link} | FileCheck %s --check-prefix=X64
; RUN: FileCheck %s --check-prefix=MAP --implicit-check-not=dead < %t/a.map
; RUN: %{parts} --check-prefixes=PARTS,PARTS-X64
; RUN: %{parts} --check-prefixes=ICF,ICF-X64
; REDEFINE: %{arch} = aarch64
; RUN: %{link} | FileCheck %s --check-prefix=A64
; RUN: FileCheck %s --check-prefix=MAP --implicit-check-not=dead < %t/a.map
; RUN: %{parts} --check-prefixes=PARTS,PARTS-A64
; RUN: %{parts} --check-prefixes=ICF,ICF-A64

; Called cleanups lie in .text$split, after the hot code, kept or dropped with
; their function; the cleanup table's base is the cold start from the field.
; MAP:      .text  {{ *}}CODE
; MAP-NEXT: .text$hot
; MAP-NEXT: .text$split
; MAP:      0001:[[COLD:[0-9a-f]+]] live.cold
; MAP-NEXT: 0001:[[COLD]] ?dtor$2@?0?live@4HA
; X64:      140002010 010b1b8e f0ffff01
; A64:      140002010 010b1bb1 f0ffff01

; ICF folds cold parts by code and unwind info: fold1's and fold2's, not
; fold3's, whose equal code has another frame.
; ICF:             0001:[[#%.8x,FOLD:]] fold1.cold
; ICF-NEXT:        0001:[[#%.8x,FOLD]] fold2.cold
; ICF-X64:         0001:[[#%.8x,FOLD+17]] fold3.cold
; ICF-A64:         0001:[[#%.8x,FOLD+20]] fold3.cold

; A handler's cold part has unwind info of its own from its start: no prologue,
; the function's codes (after end_c on AArch64) and the personality.
; PARTS:           0001:{{[0-9a-f]+}} caught.cold 00000001[[#%x,COLD:]]
; PARTS-X64:       StartAddress: (0x1[[#%X,COLD]])
; PARTS-X64:         Version: 2
; PARTS-X64:         PrologSize: 0
; PARTS-X64:           0x00: ALLOC_SMALL size=40
; PARTS-X64-NEXT:    ]
; PARTS-X64-NEXT:    Handler: (0x1400010{{[0-9A-F]+}})
; PARTS-A64:       Function: 0x1[[#%X,COLD]]
; PARTS-A64:         Prologue [
; PARTS-A64-NEXT:      end_c
; PARTS-A64-NEXT:      str x30, [sp, #-16]!
; PARTS-A64-NEXT:      end
; PARTS-A64:           Routine: 0x1400010{{[0-9A-F]+}}

;--- a.ll
declare void @may_throw()
declare void @dtor(ptr) nounwind
declare void @use(i32)
declare void @abort() noreturn nounwind
declare i32 @__gxx_personality_seh0(...)

define void @live() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  call void @dtor(ptr %a)
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

define i32 @caught() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %cont unwind label %cs
cont:
  ret i32 0
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  invoke void @may_throw() [ "funclet"(token %cp) ]
    to label %out unwind label %cs2
out:
  catchret from %cp to label %body
cs2:
  %s2 = catchswitch within %cp [label %catch2] unwind to caller
catch2:
  %cp2 = catchpad within %s2 [ptr null]
  catchret from %cp2 to label %body
body:
  ret i32 1
}

define i32 @fold1(i32 %x) personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %die
die:
  call void @use(i32 1)
  call void @abort()
  unreachable
ok:
  call void @use(i32 1)
  ret i32 %x
}

define i32 @fold2(i32 %x) personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %die
die:
  call void @use(i32 1)
  call void @abort()
  unreachable
ok:
  call void @use(i32 2)
  ret i32 %x
}

define i32 @fold3(i32 %x, i32 %y) personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw() to label %ok unwind label %cs
cs:
  %s = catchswitch within none [label %catch] unwind to caller
catch:
  %cp = catchpad within %s [ptr null]
  catchret from %cp to label %die
die:
  call void @use(i32 1)
  call void @abort()
  unreachable
ok:
  call void @use(i32 %y)
  call void @use(i32 %x)
  %s2 = add i32 %x, %y
  ret i32 %s2
}

define void @dead() personality ptr @__gxx_personality_seh0 {
entry:
  %a = alloca i32
  invoke void @may_throw() to label %cont unwind label %ehcleanup
cont:
  call void @dtor(ptr %a)
  ret void
ehcleanup:
  %pad = cleanuppad within none []
  call void @dtor(ptr %a) [ "funclet"(token %pad) ]
  cleanupret from %pad unwind to caller
}

define void @hot() section ".text$hot" {
  ret void
}

;--- b.s
.globl may_throw, dtor, use, abort, __gxx_personality_seh0
.text
may_throw:
dtor:
use:
abort:
__gxx_personality_seh0:
  ret
