; RUN: llc -mtriple=aarch64-unknown-windows-itanium < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-unknown-windows-itanium -filetype=obj < %s | \
; RUN:   llvm-readobj -r - | FileCheck --check-prefix=RELOC %s

; On Windows Itanium a thread-local variable's address truncated to 32 bits in
; static data is its offset in the image's TLS template.

@var = thread_local global i32 0, align 4

@record = constant { ptr, i32 } { ptr @var, i32 ptrtoint (ptr @var to i32) }, align 8

; CHECK-LABEL: record:
; CHECK-NEXT:  .xword var
; CHECK-NEXT:  .secrel32 var

; RELOC:      IMAGE_REL_ARM64_ADDR64 var
; RELOC-NEXT: IMAGE_REL_ARM64_SECREL var
