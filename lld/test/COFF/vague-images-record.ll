; REQUIRES: x86, aarch64

;; The import library of a DLL with an object for Windows Itanium or NT-POSIX
;; has a member that no link loads, an object whose link records hold an
;; Images group: the DLL and every image that is loaded with it, each with the
;; images it imports from and the vague entities it keeps. An export that
;; forwards an offer has a member that imports from the DLL which keeps the
;; entity, so an image that reads only this library binds that DLL.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{t} = x86_64
; DEFINE: %{m} = x64
; DEFINE: %{dll} = lld-link -dll -noentry
; DEFINE: %{records} = llvm-readobj --coff-link-records
; DEFINE: %{run} = \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj a.ll -o a.obj && \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj b.ll -o b.obj && \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj d.ll -o d.obj && \
; DEFINE:   %{dll} a.obj -out:a.dll -implib:a.lib && \
; DEFINE:   llvm-nm a.lib | FileCheck --check-prefix=MEMBER %s && \
; DEFINE:   %{records} a.lib | FileCheck --check-prefix=A %s && \
; DEFINE:   %{dll} b.obj a.lib -out:b.dll -implib:b.lib && \
; DEFINE:   %{records} b.lib | FileCheck --check-prefixes=B,A %s && \
; DEFINE:   %{dll} b.obj b.lib -out:c.dll -implib:c.lib && \
; DEFINE:   llvm-readobj --coff-imports c.dll | FileCheck --check-prefix=IMPORTS %s && \
; DEFINE:   llvm-readobj c.lib | FileCheck --check-prefix=FORWARDED %s && \
; DEFINE:   %{records} c.lib | FileCheck --check-prefixes=C,A,C2 %s && \
; DEFINE:   %{dll} d.obj a.lib -out:d.dll -implib:d.lib -delayload:a.dll && \
; DEFINE:   %{records} d.lib | FileCheck --check-prefix=DELAY %s && \
; DEFINE:   llc -mtriple=%{t}-windows-msvc -filetype=obj d.ll -o msvc.obj && \
; DEFINE:   %{dll} msvc.obj a.lib -out:msvc.dll -implib:msvc.lib && \
; DEFINE:   llvm-nm msvc.lib | FileCheck --check-prefix=NONE %s \
; DEFINE:     --implicit-check-not=__llvm_link_records

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; REDEFINE: %{m} = arm64
; RUN: %{run}

; MEMBER: A __llvm_link_records$lib
; NONE: __imp_d

;; b.dll imports from a.dll, whose instance of iv stands for its own, and
;; keeps what a.dll does not offer. Its library repeats a.dll.
; B:      Image {
; B-NEXT:   Name: b.dll
; B-NEXT:   Imports: a.dll
; B-NEXT:   Entity {
; B-NEXT:     Name: fn
; B:        Entity {
; B-NEXT:     Name: own
; B:        }
; B-NEXT: }

;; c.dll reads b.dll's library alone. It imports iv from a.dll, which b.dll
;; forwards to, and its own library imports iv from a.dll and own from b.dll.
;; Its record states the three images: a.dll as b.dll's library repeats it,
;; and b.dll as that library states it.
; IMPORTS:      Name: a.dll
; IMPORTS:      Symbol: iv
; IMPORTS:      Name: b.dll
; IMPORTS:      Symbol: own

; FORWARDED:      File: a.dll
; FORWARDED:      Symbol: __imp_iv
; FORWARDED:      File: b.dll
; FORWARDED:      Symbol: __imp_own

; C:      Image {
; C-NEXT:   Name: c.dll
; C-NEXT:   Imports: a.dll
; C-NEXT:   Imports: b.dll
; C-NEXT:   Entity {
; C-NEXT:     Name: fn
; C:        }
; C-NEXT: }
;; What a.dll keeps, by name: the constant with its section's checksum, the
;; function, the hidden, default, protected and unique variables. Neither the
;; type_info object nor a guard or another name of an entity is listed.
; A:      Image {
; A-NEXT:   Name: a.dll
; A-NEXT:   Entity {
; A-NEXT:     Name: ci
; A-NEXT:     Visibility: Default
; A-NEXT:     Init: 0
; A-NEXT:     Size: 4
; A-NEXT:     Alignment: 4
; A-NEXT:     CheckSum: 0x{{[0-9A-F]+}}
; A-NEXT:   }
; A-NEXT:   Entity {
; A-NEXT:     Name: fn
; A-NEXT:     Visibility: Default
; A-NEXT:     Kind: Function
; A-NEXT:     Size: 0
; A-NEXT:     Alignment: {{16|4}}
; A-NEXT:   }
; A-NEXT:   Entity {
; A-NEXT:     Name: hid
; A-NEXT:     Visibility: Hidden
; A-NEXT:     Init: 1
; A-NEXT:     Size: 4
; A-NEXT:     Alignment: 4
; A-NEXT:   }
; A-NEXT:   Entity {
; A-NEXT:     Name: iv
; A-NEXT:     Visibility: Default
; A-NEXT:     Init: 3
; A-NEXT:     Size: 8
; A-NEXT:     Alignment: 8
; A-NEXT:   }
; A-NEXT:   Entity {
; A-NEXT:     Name: prot
; A-NEXT:     Visibility: Protected
; A-NEXT:     Init: 1
; A-NEXT:     Size: 4
; A-NEXT:     Alignment: 4
; A-NEXT:   }
; A-NEXT:   Entity {
; A-NEXT:     Name: uniq
; A-NEXT:     Visibility: Unique
; A-NEXT:     Init: 2
; A-NEXT:     Size: 4
; A-NEXT:     Alignment: 4
; A-NEXT:   }
; A-NEXT: }

; C2:      Image {
; C2-NEXT:   Name: b.dll
; C2-NEXT:   Imports: a.dll
; C2:          Name: own

;; A DLL that the image loads late is stated as such.
; DELAY:      Name: d.dll
; DELAY-NEXT: DelayLoads: a.dll

;; A library's records are read with bounds: a count that the group does not
;; hold, an import from an image it does not have, a critical field that is
;; not known, an entity whose alignment is not one, two images of one name, a
;; critical kind that is not known and a name that does not end.
; RUN: llvm-lib -machine:x64 -def:a.def -out:base.lib
; DEFINE: %{data} =
; DEFINE: %{bad} = yaml2obj -DDATA=4C4C52430100%{data} member.yaml -o member.obj && \
; DEFINE:   cp base.lib bad.lib && llvm-ar rs bad.lib member.obj && \
; DEFINE:   not %{dll} b.obj bad.lib -out:x.dll 2>&1 | FileCheck %s
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj b.ll -o b.obj
; REDEFINE: %{data} = 04017F
; RUN: %{bad} --check-prefix=COUNT
; REDEFINE: %{data} = 040A0105612E646C6C010200
; RUN: %{bad} --check-prefix=IMPORT
; REDEFINE: %{data} = 040B0105612E646C6C00010300
; RUN: %{bad} --check-prefix=FIELD
; REDEFINE: %{data} = 04110105612E646C6C0001010601017800040E
; RUN: %{bad} --check-prefix=ENTITY
; REDEFINE: %{data} = 04110205612E646C6C000005412E444C4C0000
; RUN: %{bad} --check-prefix=TWICE
; REDEFINE: %{data} = 0500
; RUN: %{bad} --check-prefix=KIND
; REDEFINE: %{data} = 04090109612E646C6C0000
; RUN: %{bad} --check-prefix=NAME
; COUNT:  error: bad.lib: link records: 127 images, which the group does not hold
; IMPORT: error: bad.lib: link records: import from image 1, which the group does not have
; FIELD:  error: bad.lib: link records: unknown critical field 3
; ENTITY: error: bad.lib: link records: entity x of a.dll is out of range
; TWICE:  error: bad.lib: link records: two images named A.DLL
; KIND:   error: bad.lib: link records: unknown critical kind 5
; NAME:   error: bad.lib: link records: unexpected end of data

;--- a.ll
$iv = comdat any
$ci = comdat any
$uniq = comdat any
$hid = comdat any
$prot = comdat any
$fn = comdat any
$_ZTI1A = comdat any

@iv = linkonce_odr global i64 1, comdat #0
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)
@iva = alias i64, ptr @iv
@ci = linkonce_odr constant i32 2, comdat
@uniq = linkonce_odr global i32 3, comdat #1
@hid = linkonce_odr hidden global i32 4, comdat
@prot = weak_odr protected global i32 5, comdat
@_ZTI1A = linkonce_odr constant ptr null, comdat #2

define linkonce_odr void @fn() comdat {
  ret void
}

define dllexport ptr @use() {
  %a = load volatile i64, ptr @iv
  %b = load volatile i64, ptr @_ZGV2iv
  %c = load volatile i32, ptr @ci
  %d = load volatile i32, ptr @uniq
  %e = load volatile i32, ptr @hid
  %f = load volatile i32, ptr @prot
  %g = load volatile ptr, ptr @_ZTI1A
  ret ptr @fn
}

attributes #0 = { "dynamic-init"="unordered" }
attributes #1 = { "dynamic-init"="guarded" "gnu-unique" }
attributes #2 = { "type-info" }

;--- b.ll
$iv = comdat any
$own = comdat any
$fn = comdat any

@iv = linkonce_odr global i64 1, comdat #0
@own = linkonce_odr global i32 7, comdat

define linkonce_odr void @fn() comdat {
  ret void
}

define dllexport ptr @useb() {
  %a = load volatile i64, ptr @iv
  %b = load volatile i32, ptr @own
  ret ptr @fn
}

attributes #0 = { "dynamic-init"="unordered" }

;--- d.ll
declare ptr @use()

define dllexport ptr @d() {
  %p = call ptr @use()
  ret ptr %p
}

define void @__delayLoadHelper2() {
  ret void
}

;--- a.def
LIBRARY a.dll
EXPORTS
  iv DATA

;--- member.yaml
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: []
sections:
  - Name:            .llvm_link_records
    Characteristics: [ IMAGE_SCN_LNK_INFO ]
    Alignment:       1
    SectionData:     '[[DATA]]'
symbols:
  - { Name: '__llvm_link_records$lib', Value: 0, SectionNumber: -1, SimpleType: IMAGE_SYM_TYPE_NULL, ComplexType: IMAGE_SYM_DTYPE_NULL, StorageClass: IMAGE_SYM_CLASS_EXTERNAL }
