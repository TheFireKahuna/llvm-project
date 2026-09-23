; RUN: llc -mtriple=x86_64-pc-windows-itanium -filetype=obj %s -o %t.obj
; RUN: llvm-readobj --sections --section-data --relocations %t.obj | FileCheck %s
; RUN: sed 's/i32 3/i32 8/' %s > %t.flags.ll
; RUN: not llc -mtriple=x86_64-pc-windows-itanium -filetype=obj %t.flags.ll -o %t.bad.obj 2>&1 | FileCheck %s --check-prefix=FLAGS
; RUN: sed 's/!"coff.rtti_abi", i32 2/!"coff.rtti_abi", i32 1/' %s > %t.abi.ll
; RUN: not llc -mtriple=x86_64-pc-windows-itanium -filetype=obj %t.abi.ll -o %t.bad.obj 2>&1 | FileCheck %s --check-prefix=ABI

; A record is a symbol-table index plus one flag byte, with no relocations or
; padding. Unused declarations and available_externally definitions contribute
; no records: they need not have symbols in the object.
@descriptor = constant [2 x ptr] zeroinitializer, !coff.binding !1
@unused = external constant [2 x ptr], !coff.binding !1
@optimized = available_externally constant [2 x ptr] zeroinitializer, !coff.binding !1

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}

; CHECK: Name: .llvm.bind
; CHECK: RawDataSize: 13
; CHECK: RelocationCount: 0
; CHECK: IMAGE_SCN_LNK_INFO
; CHECK: IMAGE_SCN_LNK_REMOVE
; CHECK: 0000: 01000000 02000000 {{([0-9A-F]{8})}} 03
; FLAGS: invalid COFF binding metadata for descriptor
; ABI: unsupported COFF RTTI ABI module flag
