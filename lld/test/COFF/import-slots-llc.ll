; REQUIRES: x86, aarch64

;; Compiled static data holding another image's addresses (a table, pointers,
;; a derived vtable and type_info) links as in-place slots with no base
;; relocation; the support vtable's address point takes its $ap16 export.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{a} = x64
; DEFINE: %{t} = x86_64
; DEFINE: %{run} = \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj -data-sections \
; DEFINE:     d.ll -o d.obj && \
; DEFINE:   lld-link -machine:%{a} -def:rt.def -out:rt.lib && \
; DEFINE:   lld-link -machine:%{a} -dll -noentry -opt:noref rt.lib d.obj \
; DEFINE:     -out:d.dll && \
; DEFINE:   llvm-readobj --file-headers --coff-imports --coff-basereloc \
; DEFINE:     d.dll | FileCheck %s

; RUN: %{run}
; REDEFINE: %{a} = arm64
; REDEFINE: %{t} = aarch64
; RUN: %{run}

; CHECK:      IATRVA: 0x[[#%X,IAT:]]
;; writable_fn, in .data; every import is in slots, so the DLL has no table and
;; the directory starts with the first read-only slot.
; CHECK:      Name: rt.dll
; CHECK:      ImportAddressTableRVA: 0x{{[0-9A-F]+}}000
; CHECK-NEXT: Symbol: imported_fn
; CHECK-NEXT: }
;; fn_tab and var_ptr, one run across two sections.
; CHECK:      Name: rt.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA: 0x[[#IAT]]
; CHECK-NEXT: Symbol: imported_fn
; CHECK-NEXT: Symbol: imported_fn
; CHECK-NEXT: Symbol: imported_var
; CHECK-NEXT: }
;; The vtable: the base's function and its destructor.
; CHECK:      Name: rt.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA: 0x[[#IAT+48]]
; CHECK-NEXT: Symbol: _ZN1B1fEv
; CHECK-NEXT: Symbol: _ZN1BD2Ev
; CHECK-NEXT: }
;; The type_info: the support vtable's address point, then the base's.
; CHECK:      Name: rt.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA: 0x[[#IAT+80]]
; CHECK-NEXT: Symbol: _ZTVN10__cxxabiv120__si_class_type_infoE$ap16
; CHECK-NEXT: }
; CHECK:      Name: rt.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA: 0x[[#IAT+96]]
; CHECK-NEXT: Symbol: _ZTI1B
; CHECK-NEXT: }
; CHECK-NOT:  Name: rt.dll
; CHECK-NOT:  Symbol: _ZTVN10__cxxabiv120__si_class_type_infoE (
;; Only the words that hold the image's own addresses are relocated.
; CHECK:      BaseReloc [
; CHECK-COUNT-4: Type: DIR64
; CHECK-NOT:  Type: DIR64

;--- rt.def
LIBRARY rt.dll
EXPORTS
  _ZN1B1fEv _ZN1BD2Ev _ZTI1B DATA imported_fn imported_var DATA
  _ZTVN10__cxxabiv120__si_class_type_infoE DATA
  _ZTVN10__cxxabiv120__si_class_type_infoE$ap16 DATA

;--- d.ll
@fn_tab = constant [2 x ptr] [ptr @imported_fn, ptr @imported_fn]
@imported_var = external global i32
@var_ptr = constant ptr @imported_var
@writable_fn = global ptr @imported_fn
@_ZTV1D = constant { [6 x ptr] } { [6 x ptr] [ptr null, ptr @_ZTI1D, ptr @_ZN1B1fEv, ptr @_ZN1BD2Ev, ptr @_ZN1DD0Ev, ptr @_ZN1D1hEv] }
@_ZTI1D = constant { ptr, ptr, ptr } { ptr getelementptr inbounds (i8, ptr @_ZTVN10__cxxabiv120__si_class_type_infoE, i64 16), ptr @_ZTS1D, ptr @_ZTI1B }
@_ZTVN10__cxxabiv120__si_class_type_infoE = external global [0 x ptr]
@_ZTS1D = constant [3 x i8] c"1D\00"
@_ZTI1B = external constant ptr

define void @_ZN1D1hEv(ptr %this) {
  ret void
}

define void @_ZN1DD0Ev(ptr %this) {
  call void @_ZN1BD2Ev(ptr %this)
  ret void
}

declare void @imported_fn()
declare void @_ZN1B1fEv(ptr)
declare void @_ZN1BD2Ev(ptr)
