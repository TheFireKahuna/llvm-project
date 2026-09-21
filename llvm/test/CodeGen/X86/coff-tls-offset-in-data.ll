; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -filetype=obj < %s | llvm-readobj -r - | FileCheck %s --check-prefix=OBJ

; On COFF, the only value of a thread-local variable fixed at link time is its
; offset in the image's TLS template, so a 32-bit slot holding its address is
; that offset.

@tls = thread_local global i32 1, align 4
@guard = internal thread_local global i8 0, align 1
@_tls_index = external global i32
@record = constant { ptr, i32, i32 } { ptr @_tls_index, i32 ptrtoint (ptr @tls to i32), i32 ptrtoint (ptr @guard to i32) }, align 8

; ASM-LABEL: record:
; ASM-NEXT:    .quad _tls_index
; ASM-NEXT:    .secrel32 tls
; ASM-NEXT:    .secrel32 guard

; OBJ:      Section ({{[0-9]+}}) .rdata {
; OBJ-NEXT:   0x0 IMAGE_REL_AMD64_ADDR64 _tls_index
; OBJ-NEXT:   0x8 IMAGE_REL_AMD64_SECREL tls
; OBJ-NEXT:   0xC IMAGE_REL_AMD64_SECREL guard
; OBJ-NEXT: }

