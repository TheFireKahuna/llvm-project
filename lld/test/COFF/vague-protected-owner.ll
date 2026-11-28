; REQUIRES: x86

;; A copy that a DLL keeps protected owns its entity: an executable that keeps
;; the entity too takes the DLL's copy as it takes an offer, though no import
;; path joins them, and lists the import in its offer table. So does one that
;; reads another DLL's offer first.
;;
;; An executable's copy of a function that only its calls reach keeps no
;; instance of its own beside a protected one, and a fill hands out the
;; address of each function an executable's offer table names: the loader
;; grants an import's through its entry, and the executable's own instance is
;; a call target.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium \
; RUN:   %S/Inputs/fill-entry.s -o fill.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj p.ll -o p.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj d.ll -o d.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj q.ll -o q.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj main.ll -o main.obj
; RUN: lld-link -dll -noentry fill.obj p.obj -out:p.dll -implib:p.lib
; RUN: lld-link -dll -noentry fill.obj d.obj -out:d.dll -implib:d.lib
; RUN: lld-link -dll -noentry q.obj p.lib -out:q.dll -implib:q.lib
; RUN: lld-link -entry:main -subsystem:console main.obj q.lib -out:main.exe \
; RUN:   -debug:symtab
; RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=IMP %s
; RUN: llvm-objdump -d --no-show-raw-insn main.exe | FileCheck --check-prefix=CODE %s
; RUN: lld-link -entry:main -subsystem:console main.obj d.lib q.lib -out:main2.exe
; RUN: llvm-readobj --coff-imports main2.exe | FileCheck --check-prefix=IMP %s
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium \
; RUN:   %S/Inputs/loadconfig-cfg-iat.s -o config.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj s.ll -o s.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj r.ll -o r.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj main3.ll -o main3.obj
; RUN: lld-link -dll -noentry -guard:cf fill.obj p.obj config.obj -out:p.dll \
; RUN:   -implib:p.lib
; RUN: llvm-readobj --coff-load-config p.dll | FileCheck --check-prefix=SUPP %s
; RUN: lld-link -dll -noentry fill.obj s.obj -out:s.dll -implib:s.lib
; RUN: lld-link -dll -noentry r.obj s.lib -out:r.dll -implib:r.lib
; RUN: lld-link -entry:main -subsystem:console -guard:cf main3.obj q.lib r.lib \
; RUN:   config.obj -out:main3.exe -debug:symtab
; RUN: llvm-nm main3.exe > guard.txt
; RUN: llvm-readobj --coff-imports --coff-load-config main3.exe >> guard.txt
; RUN: FileCheck --check-prefix=GUARD --input-file=guard.txt %s

; IMP-NOT:  Name: d.dll
; IMP:      Name: p.dll
; IMP-NEXT: ImportLookupTableRVA:
; IMP-NEXT: ImportAddressTableRVA:
; IMP-NEXT: Symbol: _ZGV2iv (0)
; IMP-NEXT: Symbol: fn (0)
; IMP-NEXT: Symbol: iv (0)
; IMP-NEXT: }
; IMP-NOT:  Name: d.dll

;; Its reads and its address-take of fn are import loads; its call of fn
;; stays its own.
; CODE:      <main>:
; CODE:      movq {{.*}}(%rip), %rax # {{.*}} <__imp__ZGV2iv>
; CODE:      movq {{.*}}(%rip), %rax # {{.*}} <__imp_iv>
; CODE-NEXT: movl (%rax), %edi
; CODE-NEXT: movq {{.*}}(%rip), %rax # 0x140002018
; CODE-NEXT: movq %rax, (%rcx)
; CODE-NEXT: callq {{.*}} <fn>

;; p.dll exports ef but never takes its address.
; SUPP:      GuardFidTable [
; SUPP:        0x{{[0-9A-F]+}} flags 2
; SUPP:      ]

;; main3.exe imports p.dll's ef, since s.dll keeps a copy too, and the
;; loader grants it through the import's entry; it owns eo, whose address
;; s.dll takes.
; GUARD:      [[EO:[0-9a-f]+]] T eo
; GUARD:      Name: p.dll
; GUARD-NEXT: ImportLookupTableRVA:
; GUARD-NEXT: ImportAddressTableRVA: 0x[[IAT:[0-9A-F]+]]
; GUARD-NEXT: Symbol: ef (0)
; GUARD-NEXT: }
; GUARD:      GuardFidTable [
; GUARD:        0x[[EO]]
; GUARD:      GuardIatTable [
; GUARD-NEXT:   0x14000[[IAT]]
; GUARD-NEXT: ]

;--- p.ll
$iv = comdat any
$fn = comdat any

@iv = linkonce_odr protected global i32 0, comdat #0
@_ZGV2iv = linkonce_odr protected global i64 0, comdat($iv)

define linkonce_odr protected void @fn() comdat {
  ret void
}

$ef = comdat any

define weak_odr protected dllexport void @ef() comdat {
  ret void
}

define dllexport i32 @readp(ptr %out) {
  %g = load atomic i8, ptr @_ZGV2iv acquire, align 8
  %i = load i32, ptr @iv
  store volatile ptr @fn, ptr %out
  call void @fn()
  ret i32 %i
}

attributes #0 = { "dynamic-init"="guarded" }

;--- d.ll
$iv = comdat any

@iv = linkonce_odr global i32 0, comdat #0
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)

define dllexport i32 @readd() {
  %i = load i32, ptr @iv
  ret i32 %i
}

attributes #0 = { "dynamic-init"="guarded" }

;--- q.ll
declare dllimport i32 @readp(ptr)

define dllexport i32 @readq(ptr %out) {
  %i = call i32 @readp(ptr %out)
  ret i32 %i
}

;--- main.ll
$iv = comdat any
$fn = comdat any

@iv = linkonce_odr global i32 0, comdat #0
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)

define linkonce_odr void @fn() comdat {
  ret void
}

declare dllimport i32 @readq(ptr)
declare dllimport i32 @readd()

define i32 @main(ptr %out) {
  %g = load atomic i8, ptr @_ZGV2iv acquire, align 8
  %i = load i32, ptr @iv
  store volatile ptr @fn, ptr %out
  call void @fn()
  %q = call i32 @readq(ptr %out)
  %s = add i32 %i, %q
  ret i32 %s
}

attributes #0 = { "dynamic-init"="guarded" }
;--- s.ll
$ef = comdat any
$eo = comdat any

define linkonce_odr void @ef() comdat {
  ret void
}

define linkonce_odr void @eo() comdat {
  ret void
}

define dllexport ptr @taked(i1 %c) {
  %p = select i1 %c, ptr @ef, ptr @eo
  ret ptr %p
}

;--- r.ll
declare dllimport ptr @taked(i1)

define dllexport ptr @taker(i1 %c) {
  %p = call ptr @taked(i1 %c)
  ret ptr %p
}

;--- main3.ll
$ef = comdat any
$eo = comdat any

define weak_odr void @ef() comdat {
  ret void
}

define weak_odr void @eo() comdat {
  ret void
}

declare dllimport i32 @readq(ptr)
declare dllimport ptr @taker(i1)

define i32 @main() {
  call void @ef()
  call void @eo()
  %p = call ptr @taker(i1 true)
  %q = call i32 @readq(ptr %p)
  ret i32 %q
}
