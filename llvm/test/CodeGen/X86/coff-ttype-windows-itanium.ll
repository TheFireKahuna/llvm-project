; On Windows Itanium and NT-POSIX a catch-type entry is a 32-bit offset to a
; pointer to the descriptor: the import pointer of a descriptor the linker can
; see, an extern_weak one included, which the object keeps a weak external,
; and a pointer private to the object for a local one, which has a section of
; its own, as every global does by default on these triples. catch (...)
; stays a null entry.
;
; RUN: llc -mtriple=x86_64-unknown-windows-itanium < %s | FileCheck %s --check-prefixes=CHECK,OWN
; RUN: llc -mtriple=x86_64-pc-windows-ntposix < %s | FileCheck %s --check-prefixes=CHECK,OWN
; RUN: llc -mtriple=x86_64-unknown-windows-itanium -data-sections=0 < %s | FileCheck %s --check-prefixes=CHECK,SHARED
; RUN: llc -mtriple=x86_64-w64-windows-gnu < %s | FileCheck %s --check-prefix=MINGW

@_ZTI8Imported = external constant ptr
@_ZTI5Local = dso_local constant { ptr, ptr } { ptr null, ptr null }
@_ZTIN12_GLOBAL__N_14AnonE = internal constant { ptr, ptr } { ptr null, ptr null }
@_ZTI4Weak = extern_weak constant ptr

declare void @may_throw()
declare i32 @__gxx_personality_seh0(...)

define i32 @f() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @may_throw()
          to label %ret unwind label %lpad

lpad:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTI8Imported
          catch ptr @_ZTI5Local
          catch ptr @_ZTIN12_GLOBAL__N_14AnonE
          catch ptr @_ZTI4Weak
          catch ptr null
  %sel = extractvalue { ptr, i32 } %lp, 1
  ret i32 %sel

ret:
  ret i32 0
}

; CHECK:       .byte 155 # @TType Encoding = indirect pcrel sdata4
; CHECK:       # >> Catch TypeInfos <<
; CHECK-NEXT:  [[T5:.Ltmp[0-9]+]]: # TypeInfo 5
; CHECK-NEXT:  .long __imp__ZTI8Imported-[[T5]]
; CHECK-NEXT:  [[T4:.Ltmp[0-9]+]]: # TypeInfo 4
; CHECK-NEXT:  .long __imp__ZTI5Local-[[T4]]
; CHECK-NEXT:  [[T3:.Ltmp[0-9]+]]: # TypeInfo 3
; SHARED-NEXT: .long .L_ZTIN12_GLOBAL__N_14AnonE.DW.stub-[[T3]]
; OWN-NEXT:    .long _ZTIN12_GLOBAL__N_14AnonE.DW.stub-[[T3]]
; CHECK-NEXT:  [[T2:.Ltmp[0-9]+]]: # TypeInfo 2
; CHECK-NEXT:  .long __imp__ZTI4Weak-[[T2]]
; CHECK-NEXT:  .long 0 # TypeInfo 1

; SHARED:      .section .rdata,"dr"
; SHARED-NOT:  .section
; SHARED:      .L_ZTIN12_GLOBAL__N_14AnonE.DW.stub:
; SHARED-NEXT: .quad _ZTIN12_GLOBAL__N_14AnonE

; OWN:         .section .rdata,"dr",one_only,_ZTIN12_GLOBAL__N_14AnonE.DW.stub
; OWN-NEXT:    .p2align 3, 0x0
; OWN-NEXT:    _ZTIN12_GLOBAL__N_14AnonE.DW.stub:
; OWN-NEXT:    .quad _ZTIN12_GLOBAL__N_14AnonE

; CHECK-NOT:   .globl {{.*}}DW.stub
; CHECK-NOT:   _ZTI4Weak.DW.stub
; CHECK:       .weak _ZTI4Weak
; CHECK-NOT:   .refptr

; MINGW:       .byte 0 # @TType Encoding = absptr
; MINGW:       .quad _ZTI8Imported
; MINGW-NOT:   DW.stub
