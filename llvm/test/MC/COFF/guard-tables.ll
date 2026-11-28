; RUN: sed 's/ASM/leaq in_asm(%rip), %rax/' %s | \
; RUN:   llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj -o %t.x64.o
; RUN: llvm-objdump -t %t.x64.o > %t.x64.txt
; RUN: llvm-readobj -x '.gfids$y' -x '.giats$y' %t.x64.o >> %t.x64.txt
; RUN: FileCheck %s --input-file=%t.x64.txt
; RUN: %if aarch64-registered-target %{ sed 's/ASM/adrp x0, in_asm/' %s | \
; RUN:   llc -mtriple=aarch64-pc-windows-ntposix -filetype=obj -o %t.a64.o %}
; RUN: %if aarch64-registered-target %{ llvm-objdump -t %t.a64.o > %t.a64.txt %}
; RUN: %if aarch64-registered-target %{ llvm-readobj -x '.gfids$y' -x '.giats$y' %t.a64.o >> %t.a64.txt %}
; RUN: %if aarch64-registered-target %{ FileCheck %s --input-file=%t.a64.txt %}

; Without Control Flow Guard's checks, a compiled object for Windows Itanium
; or NT-POSIX lists the functions and the import pointers whose address its
; relocations take, also where inline assembly or a declaration's use takes it. A function that is
; only called is not listed, also where it is called or tail-called through
; its import pointer or through a register that only holds it. The address of
; a declaration that is not dso_local is taken from its import pointer.

; CHECK-DAG: [{{ *}}[[#%d,ASM:]]]{{.*}} in_asm{{$}}
; CHECK-DAG: [{{ *}}[[#%d,DECL:]]]{{.*}} declared{{$}}
; CHECK-DAG: [{{ *}}[[#%d,IMP:]]]{{.*}} __imp_imported{{$}}
; CHECK-DAG: [{{ *}}[[#%d,ANY:]]]{{.*}} __imp_anywhere{{$}}
; CHECK-DAG: (sec -1){{.*}}(scl 3) (nx 0) 0x00000800 @feat.00
; CHECK:      Hex dump of section '.gfids$y':
; CHECK-NEXT: 0x00000000 [[#%.2x,ASM]]000000 [[#%.2x,DECL]]000000 {{[^ ]+$}}
; CHECK-EMPTY:
; CHECK-NEXT: Hex dump of section '.giats$y':
; CHECK-NEXT: 0x00000000 [[#%.2x,IMP]]000000 [[#%.2x,ANY]]000000 {{[^ ]+$}}
; CHECK-EMPTY:

module asm "ASM"

declare dso_local void @declared()
declare void @anywhere()
declare dllimport void @imported()
declare dllimport void @imported_called()
declare dllimport void @imported_jumped()

define void @in_asm() {
  ret void
}

define void @called() {
  ret void
}

define void @user(ptr %q, ptr %r) {
  call void @called()
  call void @imported_called()
  store ptr @declared, ptr %q
  store ptr @imported, ptr %r
  store ptr @anywhere, ptr %q
  ret void
}

define void @jumper() {
  tail call void @imported_jumped()
  ret void
}

define void @twice() {
  call void @imported_called()
  call void @imported_called()
  ret void
}
