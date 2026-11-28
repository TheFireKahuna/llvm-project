; REQUIRES: x86, aarch64
; RUN: rm -rf %t && split-file %s %t && cd %t
; DEFINE: %{triple} = x86_64-unknown-windows-itanium
; DEFINE: %{asm} = llc -mtriple=%{triple} use.ll -o -
; DEFINE: %{link} = llc -mtriple=%{triple} -filetype=obj use.ll -o use.obj && \
; DEFINE:   llvm-mc -triple=%{triple} -filetype=obj rt.s -o rt.obj && \
; DEFINE:   lld-link -dll -noentry -out:rt.dll -implib:rt.lib rt.obj \
; DEFINE:     -export:_ZTIi,data -export:thrower \
; DEFINE:     -export:__gxx_personality_seh0 && \
; DEFINE:   lld-link -entry:main -subsystem:console -out:use.exe use.obj \
; DEFINE:     rt.lib 2>&1 | count 0 && \
; DEFINE:   llvm-readobj --coff-imports --coff-basereloc use.exe
; RUN: %{asm} | FileCheck %s --check-prefixes=ASM,COMDAT
; RUN: %{asm} -data-sections=0 | FileCheck %s --check-prefixes=ASM,PLAIN
; RUN: %{link} | FileCheck %s --check-prefix=IMAGE
; REDEFINE: %{triple} = aarch64-unknown-windows-itanium
; RUN: %{asm} | FileCheck %s --check-prefixes=ASM,COMDAT
; RUN: %{link} | FileCheck %s --check-prefix=IMAGE

; A catch-type entry is a 4-byte distance to a type_info pointer: __imp_ for an
; externally named one, the object's own otherwise; catch-all stays null.

; ASM:        @TType Encoding = indirect pcrel sdata4
; ASM:        {{\.long|\.word}} 0 {{.*}} TypeInfo 4
; COMDAT:     {{\.long|\.word}} _ZTIN12_GLOBAL__N_11XE.DW.stub-
; PLAIN:      .long .L_ZTIN12_GLOBAL__N_11XE.DW.stub-
; ASM:        {{\.long|\.word}} __imp__ZTI1L-
; ASM:        {{\.long|\.word}} __imp__ZTIi-

; The object's own pointer is a COMDAT with data sections, else a local label.

; COMDAT:      .section .rdata,"dr",one_only,_ZTIN12_GLOBAL__N_11XE.DW.stub
; COMDAT-NEXT: .p2align 3
; COMDAT-NEXT: _ZTIN12_GLOBAL__N_11XE.DW.stub:
; PLAIN:       .L_ZTIN12_GLOBAL__N_11XE.DW.stub:
; ASM-NEXT:    {{\.quad|\.xword}} _ZTIN12_GLOBAL__N_11XE

; The link is silent; _ZTIi is read from the IAT and only the two pointers, not
; the exception table, take base relocations.

; IMAGE:      Symbol: _ZTIi
; IMAGE:      BaseReloc [
; IMAGE-NEXT:   Entry {
; IMAGE-NEXT:     Type: DIR64
; IMAGE-NEXT:     Address: 0x2
; IMAGE-NEXT:   }
; IMAGE-NEXT:   Entry {
; IMAGE-NEXT:     Type: DIR64
; IMAGE-NEXT:     Address: 0x2
; IMAGE-NEXT:   }
; IMAGE-NEXT: ]

;--- use.ll
@_ZTIi = external constant ptr
@_ZTI1L = constant ptr null
@_ZTIN12_GLOBAL__N_11XE = internal constant ptr null

declare void @thrower()
declare i32 @__gxx_personality_seh0(...)

define void @main() personality ptr @__gxx_personality_seh0 {
entry:
  invoke void @thrower()
          to label %done unwind label %dispatch

dispatch:
  %cs = catchswitch within none [label %imported, label %local, label %internal, label %all] unwind to caller

imported:
  %c1 = catchpad within %cs [ptr @_ZTIi]
  catchret from %c1 to label %done

local:
  %c2 = catchpad within %cs [ptr @_ZTI1L]
  catchret from %c2 to label %done

internal:
  %c3 = catchpad within %cs [ptr @_ZTIN12_GLOBAL__N_11XE]
  catchret from %c3 to label %done

all:
  %c4 = catchpad within %cs [ptr null]
  catchret from %c4 to label %done

done:
  ret void
}

;--- rt.s
  .globl _ZTIi, thrower, __gxx_personality_seh0
  .data
_ZTIi:
  .quad 0
  .text
thrower:
__gxx_personality_seh0:
  ret
