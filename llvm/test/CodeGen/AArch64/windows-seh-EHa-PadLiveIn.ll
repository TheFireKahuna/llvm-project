; RUN: llc -mtriple=aarch64-pc-windows-msvc < %s | FileCheck %s
; RUN: llc -mtriple=aarch64-windows-itanium < %s | FileCheck %s

; Under asynchronous EH a fault anywhere in the __try region reaches its
; __except block with only the callee-saved registers restored, so %p, which
; is live into the block, is held in one of them across the region.

; CHECK-LABEL: holds:
; CHECK:         mov [[P:x(19|2[0-8])]], x0
; CHECK:         str w{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK:         ldr w0, {{\[}}[[P]]{{\]}}
; CHECK:       $ehgcr_0_3:

@Null = global ptr null

define i32 @holds(ptr %p) personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin()
          to label %try unwind label %dispatch

try:
  %n = load volatile ptr, ptr @Null
  store volatile i32 1, ptr %n
  invoke void @llvm.seh.try.end()
          to label %done unwind label %dispatch

dispatch:
  %cs = catchswitch within none [label %except] unwind to caller

except:
  %cp = catchpad within %cs [ptr null]
  catchret from %cp to label %done

done:
  %v = load i32, ptr %p
  ret i32 %v
}

declare i32 @__C_specific_handler(...)
declare void @llvm.seh.try.begin()
declare void @llvm.seh.try.end()
