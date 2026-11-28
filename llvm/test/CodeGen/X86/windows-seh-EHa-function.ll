; RUN: llc -mtriple=x86_64-windows-msvc < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-itanium < %s | FileCheck %s

; A function that opens a __try block with llvm.seh.try.begin covers each
; instruction of the block (-EHa), with no eh-asynch module flag, as on
; Windows Itanium, where the mode is each function's. One that does not covers
; its calls only.

; CHECK-LABEL: async:
; CHECK:       [[BEGIN:.Ltmp[0-9]+]]: # EH_LABEL
; CHECK-NEXT:  nop
; CHECK-NEXT:  movl $1, (%rcx)
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]: # EH_LABEL
; CHECK:       .long [[BEGIN]]@IMGREL # LabelStart
; CHECK-NEXT:  .long [[END]]@IMGREL # LabelEnd
; CHECK-NEXT:  .long 1 # CatchAll

; CHECK-LABEL: sync:
; CHECK:       .seh_endprologue
; CHECK-NEXT:  movl $1, (%rcx)
; CHECK-NEXT:  [[BEGIN:.Ltmp[0-9]+]]: # EH_LABEL
; CHECK-NEXT:  callq f
; CHECK-NEXT:  nop
; CHECK-NEXT:  [[END:.Ltmp[0-9]+]]: # EH_LABEL
; CHECK:       .long [[BEGIN]]@IMGREL # LabelStart
; CHECK-NEXT:  .long [[END]]@IMGREL # LabelEnd
; CHECK-NEXT:  .long 1 # CatchAll
; CHECK-NEXT:  .long
; CHECK-NEXT:  .Llsda_end

define i32 @async(ptr %p) personality ptr @__C_specific_handler {
entry:
  invoke void @llvm.seh.try.begin()
          to label %body unwind label %dispatch

body:
  store volatile i32 1, ptr %p
  invoke void @llvm.seh.try.end()
          to label %done unwind label %dispatch

dispatch:
  %cs = catchswitch within none [label %pad] unwind to caller

pad:
  %cp = catchpad within %cs [ptr null]
  catchret from %cp to label %except

except:
  ret i32 1

done:
  ret i32 0
}

define i32 @sync(ptr %p) personality ptr @__C_specific_handler {
entry:
  store volatile i32 1, ptr %p
  invoke void @f()
          to label %done unwind label %dispatch

dispatch:
  %cs = catchswitch within none [label %pad] unwind to caller

pad:
  %cp = catchpad within %cs [ptr null]
  catchret from %cp to label %except

except:
  ret i32 1

done:
  ret i32 0
}

declare void @f()
declare void @llvm.seh.try.begin()
declare void @llvm.seh.try.end()
declare i32 @__C_specific_handler(...)
