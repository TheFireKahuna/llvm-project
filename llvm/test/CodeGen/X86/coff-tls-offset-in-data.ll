; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s | \
; RUN:   llvm-readobj -r - | FileCheck --check-prefix=RELOC %s
; RUN: llc -mtriple=x86_64-pc-windows-msvc < %s | FileCheck --check-prefix=MSVC %s

; On Windows Itanium a thread-local variable's address truncated to 32 bits in
; static data is its offset in the image's TLS template, the only value of it
; fixed at link time. A pointer-sized field still holds its address, and the
; MSVC target is unchanged.

@var = thread_local global i32 0, align 4
@guard = internal thread_local global i8 0, align 1

@record = constant { ptr, i32, i32 } { ptr @var, i32 ptrtoint (ptr @var to i32), i32 ptrtoint (ptr @guard to i32) }, align 8

; CHECK-LABEL: record:
; CHECK-NEXT:  .quad var
; CHECK-NEXT:  .secrel32 var
; CHECK-NEXT:  .secrel32 guard

; RELOC:      IMAGE_REL_AMD64_ADDR64 var
; RELOC-NEXT: IMAGE_REL_AMD64_SECREL var
; RELOC-NEXT: IMAGE_REL_AMD64_SECREL guard

; MSVC-LABEL: record:
; MSVC-NEXT:  .quad var
; MSVC-NEXT:  .long var
; MSVC-NEXT:  .long guard
