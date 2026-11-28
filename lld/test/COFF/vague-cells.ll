; REQUIRES: x86, aarch64

;; A DLL reaches each vague entity that it keeps preemptible through a cell,
;; its import pointer in .sealed, which its fill binds to the instance that the
;; executable's offer table names: a variable and its guard, and the address
;; of a constant and of a function, while the constant's value reads its own
;; copy and the function's calls stay direct. A word of data that holds the
;; function's address is computed from its cell. The fill's list names each
;; cell as a word of kind 4 and use 3, 0x1c, with the top 24 bits of its
;; entity's key above that, the RVA of its own entry in the DLL's offer table
;; and above it that of its instance, and its header ends with the key of the
;; DLL's name; the table names each entity by its key and the string of its
;; export at its cell, a guard with its variable's size, and the DLL by its
;; name, flagged 0x800, with the identity of its record as its size.
;; A DLL that imports such an entity binds its entry the same way, and its
;; table names the entry by its import's name. Such a DLL starts with the fill
;; that searches. The executable picks the owner of an entity that two DLLs
;; keep, the first breadth-first, imports each of its names and lists it at
;; the import's entry; it names each DLL that its link sees loaded with it,
;; with the identity of the record that it read, a.dll's its own.
;; -Bsymbolic binds the DLL's own.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{a} = x64
; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{dll} = lld-link -machine:%{a} -dll -noentry fill.obj
; DEFINE: %{run} = \
; DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-itanium \
; DEFINE:     %S/Inputs/fill-entry.s -o fill.obj && \
; DEFINE:   %{llc} a.ll -o a.obj && %{llc} b.ll -o b.obj && \
; DEFINE:   %{llc} c.ll -o c.obj && %{llc} main.ll -o main.obj && \
; DEFINE:   %{dll} a.obj -out:a.dll -implib:a.lib -debug:symtab && \
; DEFINE:   %{dll} b.obj -out:b.dll -implib:b.lib && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn a.dll > a.txt && \
; DEFINE:   llvm-objdump -s -j .rdata -j .sealed a.dll >> a.txt && \
; DEFINE:   %{dll} c.obj a.lib -out:c.dll && \
; DEFINE:   llvm-objdump -s -j .rdata c.dll >> a.txt && \
; DEFINE:   lld-link -machine:%{a} -entry:main -subsystem:console main.obj \
; DEFINE:     a.lib b.lib -out:main.exe && \
; DEFINE:   llvm-readobj --coff-imports main.exe >> a.txt && \
; DEFINE:   llvm-objdump -s -j .rdata main.exe >> a.txt && \
; DEFINE:   FileCheck --check-prefixes=CHECK,%{a} %s < a.txt && \
; DEFINE:   %{dll} a.obj -out:sym.dll -Bsymbolic && \
; DEFINE:   llvm-readobj --sections sym.dll | FileCheck --check-prefix=SYM %s && \
; DEFINE:   llc -mtriple=%{t}-windows-gnu -filetype=obj -addrsig gnu.ll -o gnu.obj && \
; DEFINE:   %{dll} a.obj gnu.obj -export:gaddr -export:gcall -out:d.dll && \
; DEFINE:   llvm-objdump -d --no-show-raw-insn d.dll | \
; DEFINE:     FileCheck --check-prefix=GNU-%{a} %s && \
; DEFINE:   llvm-objdump -s -j .rdata d.dll | FileCheck --check-prefix=GNUW %s

; RUN: %{run}
; REDEFINE: %{a} = arm64
; REDEFINE: %{t} = aarch64
; RUN: %{run}

;; A call or jump through the cell of a function that the DLL keeps
;; preemptible, from a unit that declares it an import, reaches the DLL's own
;; copy; an address-take, alone or sharing the call's load, by a hidden alias
;; or in the function itself, reads the cell. One by an internal alias takes
;; the DLL's own copy, as a local symbol does on ELF.
; RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium \
; RUN:   %S/Inputs/fill-entry.s -o fill-x.obj
; RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium \
; RUN:   %S/Inputs/fill-entry.s -o fill-a.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj ef.ll -o ef-x.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj eu.ll -o eu-x.obj
; RUN: llc -mtriple=aarch64-windows-itanium -filetype=obj ef.ll -o ef-a.obj
; RUN: llc -mtriple=aarch64-windows-itanium -filetype=obj eu.ll -o eu-a.obj
; RUN: lld-link -machine:x64 -dll -noentry fill-x.obj ef-x.obj eu-x.obj -out:e-x.dll
; RUN: lld-link -machine:arm64 -dll -noentry fill-a.obj ef-a.obj eu-a.obj -out:e-a.dll
; RUN: llvm-objdump -d --no-show-raw-insn e-x.dll | FileCheck --check-prefix=EX %s
; RUN: llvm-objdump -d --no-show-raw-insn e-a.dll | FileCheck --check-prefix=EA %s

; EX:      <own>:
; EX-NEXT: leaq {{.*}} <ef>
; EX:      <pgo>:
; EX-NEXT: movq {{.*}}(%rip), %rax # 0x[[CELL:[0-9a-f]+]]
; EX:      <ef>:
; EX-NEXT: movq {{.*}}(%rip), %rax # 0x[[CELL]]
; EX:      <calls>:
; EX:      callq {{.*}} <ef>
; EX:      <jumps>:
; EX-NEXT: jmp {{.*}} <ef>
; EX:      <both>:
; EX:      callq {{.*}} <ef>
; EX-NEXT: movq {{.*}}(%rip), %rax # 0x[[CELL]]
; EX:      <addr>:
; EX-NEXT: movq {{.*}}(%rip), %rax # 0x[[CELL]]

; EA:      <own>:
; EA-NEXT: adr x0, {{.*}} <ef>
; EA:      <pgo>:
; EA-NEXT: ldr x0, 0x[[ECELL:[0-9a-f]+]]
; EA:      <ef>:
; EA-NEXT: ldr x0, 0x[[ECELL]]
; EA:      <calls>:
; EA:      bl {{.*}} <ef>
; EA:      <jumps>:
; EA-NEXT: b {{.*}} <ef>
; EA:      <both>:
; EA:      ldr x19, 0x[[CELL:[0-9a-f]+]]
; EA-NEXT: bl {{.*}} <ef>
; EA:      <addr>:
; EA-NEXT: ldr x0, 0x[[CELL]]

;; An executable names what it keeps, which it does not export, by a string
;; that its table holds after the buckets. Two names with one key in one
;; table are an error.
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj own.ll -o own.obj
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj key.ll -o key.obj
; RUN: lld-link -entry:own -subsystem:console own.obj -out:own.exe
; RUN: llvm-objdump -s -j .rdata own.exe | FileCheck --check-prefix=OWN %s
; RUN: not lld-link -entry:own -subsystem:console own.obj key.obj -include:key \
; RUN:   -out:key.exe 2>&1 | FileCheck --check-prefix=KEY %s

; OWN:      140002000 2b1d7a4f 00000000 01000000 00000000
; OWN-NEXT: 140002010 9d8486f6 62eb9fb8 30200000 00300000
; OWN-NEXT: 140002020 04000000 05000200 00000000 01000000
; OWN-NEXT: 140002030 6b306231 63646238 38336132 33366336  k0b1cdb883a236c6
; OWN-NEXT: 140002040 3600                                 6.

; KEY: error: k35204f1f268f1afc and k0b1cdb883a236c66 have one key in the offer table; rename one of the two

; CHECK:      <__llvm_fill_dll_imports>:
; x64:      <reads>:
; x64:      movq {{.*}}(%rip), %rax # 0x180003018 <__imp__ZGV2iv>
; x64:      movq {{.*}}(%rip), %rax # 0x180003010 <__imp_iv>
; x64-NEXT: movl (%rax), %esi
; x64-NEXT: addl {{.*}}(%rip), %esi # 0x180002000 <c>
; x64:      callq {{.*}} <fn>
; x64:      <addrs>:
; x64-NEXT: movq {{.*}}(%rip), %rax # 0x180003010 <__imp_iv>
; x64:      movq {{.*}}(%rip), %rax # 0x180003020 <__imp_c>
; x64:      movq {{.*}}(%rip), %rax # 0x180003008
; x64:      leaq {{.*}}(%rip), %rax # 0x180003028 <pw>

; arm64:      <reads>:
; arm64:      ldr x8, 0x180003018 <__imp__ZGV2iv>
; arm64:      ldr x9, 0x180003010 <__imp_iv>
; arm64-NEXT: ldr w19, [x9]
; arm64-NEXT: adr x10, 0x180002000 <c>
; arm64:      bl {{.*}} <fn>
; arm64:      <addrs>:
; arm64-NEXT: ldr x8, 0x180003010 <__imp_iv>
; arm64-NEXT: ldr x9, 0x180003020 <__imp_c>
; arm64:      ldr x8, 0x180003008
; arm64:      adr x8, 0x180003028 <pw>

;; Four cells, fn's, iv's, the guard's and c's, and pw, a word of fn's cell;
;; the entry of a.dll's table that names a.dll, and no name word.
; CHECK:      Contents of section .rdata:
; CHECK-NEXT: 180002000 03000000 00000000 00000000 04000000
; CHECK-NEXT: 180002010 05000000 00000000 00300000 30000000
; x64-NEXT:   180002020 38210000 00000000 08300000 1ce53d8c
; arm64-NEXT: 180002020 30210000 00000000 08300000 1ce53d8c
; x64-NEXT:   180002030 50210000 80100000 10300000 1c45ad9f
; x64-NEXT:   180002040 80210000 04400000 18300000 1c497d2d
; x64-NEXT:   180002050 20210000 08400000 20300000 1c21408c
; x64-NEXT:   180002060 68210000 00200000 28300000 08300000
; arm64-NEXT: 180002030 48210000 74100000 10300000 1c45ad9f
; arm64-NEXT: 180002040 78210000 04400000 18300000 1c497d2d
; arm64-NEXT: 180002050 18210000 08400000 20300000 1c21408c
; arm64-NEXT: 180002060 60210000 00200000 28300000 08300000
; CHECK-NEXT: 180002070 00000000 00000000

;; The table: five entries, by key, in eight buckets; each with the RVA of
;; its name, here the export's, at its cell, with its size, the entity's
;; flags (default 1, its init kind above bit 2, a function 0x40, a checksum
;; 0x80, preemptible 0x200 and indirect 0x400) and the logarithm of its
;; alignment, which a function has none of; and a.dll's, by a string after the
;; buckets.
; x64:        180002110 2b1d7a4f 03000000 05000000 00000000
; x64-NEXT:   180002120 2d4726a8 77497d2d e2200000 18300000
; x64-NEXT:   180002130 04000000 09060200 43db7053 f4bda758
; x64-NEXT:   180002140 bc210000 00000000 aae6b131 00080000
; x64-NEXT:   180002150 4f85c1e2 e3e53d8c f2200000 08300000
; x64-NEXT:   180002160 00000000 41060000 1bf8b946 9a21408c
; x64-NEXT:   180002170 f0200000 20300000 04000000 81060200
; x64-NEXT:   180002180 50486587 2245ad9f f5200000 10300000
; x64-NEXT:   180002190 04000000 09060200 00000000 00000000
; x64-NEXT:   1800021a0 01000000 02000000 02000000 05000000
; x64-NEXT:   1800021b0 05000000 05000000 05000000 612e646c
; x64-NEXT:   1800021c0 6c00
; arm64:      2b1d7a4f 03000000
; arm64-NEXT: 180002110 05000000 00000000 2d4726a8 77497d2d
; arm64-NEXT: 180002120 e2200000 18300000 04000000 09060200
; arm64-NEXT: 180002130 43db7053 f4bda758 b4210000 00000000
; arm64-NEXT: 180002140 aae6b131 00080000 4f85c1e2 e3e53d8c
; arm64-NEXT: 180002150 f2200000 08300000 00000000 41060000
; arm64-NEXT: 180002160 1bf8b946 9a21408c f0200000 20300000
; arm64-NEXT: 180002170 04000000 81060200 50486587 2245ad9f
; arm64-NEXT: 180002180 f5200000 10300000 04000000 09060200
; arm64-NEXT: 180002190 00000000 00000000 01000000 02000000
; arm64-NEXT: 1800021a0 02000000 05000000 05000000 05000000
; arm64-NEXT: 1800021b0 05000000 612e646c 6c00
; CHECK:      Contents of section .sealed:
; CHECK-NEXT: 180003000 00000000 00000000 [[FNA:[0-9a-f]+]] 01000000
; CHECK-NEXT: 180003010 04400080 01000000 08400080 01000000
; CHECK-NEXT: 180003020 00200080 01000000 [[FNA]] 01000000

;; c.dll's entry of iv, at 0x2000, is a cell in the one range of its list,
;; with no instance of its own, and its table's entries are c.dll's and iv's,
;; named by its import, with a.dll's record of iv.
; CHECK:      Contents of section .rdata:
; CHECK-NEXT: 180002000
; CHECK-NEXT: 180002010 01000000 01000000 01000000 00000000
; CHECK-NEXT: 180002020 00300000 08000000 e0200000 00000000
; CHECK-NEXT: 180002030 00200000 08000000 00200000 1c45ad9f
; CHECK-NEXT: 180002040 f8200000 00000000
; CHECK:      1800020d0 2b1d7a4f 01000000 02000000 00000000
; CHECK-NEXT: 1800020e0 1a57269d 0c767064 1c210000 00000000
; CHECK-NEXT: 1800020f0 7311b01e 00080000 50486587 2245ad9f
; CHECK-NEXT: 180002100 c2200000 00200000 04000000 09060200
; CHECK-NEXT: 180002110 00000000 01000000 02000000 632e646c
; CHECK-NEXT: 180002120 6c00

;; The executable takes a.dll's instance of each name, at its entries, which
;; its imports name, each with the hint that a.lib gives it.
; CHECK:      Name: a.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA: 0x2000
; CHECK-NEXT: Symbol: _ZGV2iv (0)
; CHECK-NEXT: Symbol: c (2)
; CHECK-NEXT: Symbol: fn (3)
; CHECK-NEXT: Symbol: iv (4)
; CHECK-NEXT: Symbol: reads (
; CHECK:      Name: b.dll
; CHECK-NEXT: ImportLookupTableRVA:
; CHECK-NEXT: ImportAddressTableRVA:
; CHECK-NEXT: Symbol: readsb (
; CHECK-NEXT: }
; x64:        2b1d7a4f 03000000
; x64-NEXT:   140002110 06000000 00000000 2d4726a8 77497d2d
; x64-NEXT:   140002120 c2200000 00200000 04000000 09060200
; x64-NEXT:   140002130 43db7053 f4bda758 cc210000 00000000
; x64-NEXT:   140002140 aae6b131 00080000 fb677095 8b24996f
; x64-NEXT:   140002150 d2210000 00000000 7f336f84 00080000
; x64-NEXT:   140002160 4f85c1e2 e3e53d8c d0200000 10200000
; x64-NEXT:   140002170 00000000 41060000 1bf8b946 9a21408c
; x64-NEXT:   140002180 cc200000 08200000 04000000 81060200
; x64-NEXT:   140002190 50486587 2245ad9f d6200000 18200000
; x64-NEXT:   1400021a0 04000000 09060200 00000000 00000000
; x64-NEXT:   1400021b0 01000000 02000000 03000000 06000000
; x64-NEXT:   1400021c0 06000000 06000000 06000000 612e646c
; x64-NEXT:   1400021d0 6c00622e 646c6c00
; arm64:      140002100 2b1d7a4f 03000000 06000000 00000000
; arm64-NEXT: 140002110 2d4726a8 77497d2d c2200000 00200000
; arm64-NEXT: 140002120 04000000 09060200 43db7053 f4bda758
; arm64-NEXT: 140002130 c4210000 00000000 aae6b131 00080000
; arm64-NEXT: 140002140 fb677095 8b24996f ca210000 00000000
; arm64-NEXT: 140002150 7f336f84 00080000 4f85c1e2 e3e53d8c
; arm64-NEXT: 140002160 d0200000 10200000 00000000 41060000
; arm64-NEXT: 140002170 1bf8b946 9a21408c cc200000 08200000
; arm64-NEXT: 140002180 04000000 81060200 50486587 2245ad9f
; arm64-NEXT: 140002190 d6200000 18200000 04000000 09060200
; arm64-NEXT: 1400021a0 00000000 00000000 01000000 02000000
; arm64-NEXT: 1400021b0 03000000 06000000 06000000 06000000
; arm64-NEXT: 1400021c0 06000000 612e646c 6c00622e 646c6c00

; SYM-NOT: Name: .sealed

;; A foreign object's address-take of fn, and its word of data that holds
;; it, read fn's cell; its call stays direct.
; GNU-x64:        <gaddr>:
; GNU-x64-NEXT:     movq {{.*}}(%rip), %rax # 0x180003008
; GNU-x64:        <gcall>:
; GNU-x64:          callq {{.*}} <fn>
; GNU-arm64:      <gaddr>:
; GNU-arm64-NEXT:   adrp x0, 0x180003000
; GNU-arm64-NEXT:   ldr x0, [x0, #0x8]
; GNU-arm64:      <gcall>:
; GNU-arm64:        bl {{.*}} <fn>
; GNUW:           180002070 00000000 00000000 08400000 08300000

;--- a.ll
$iv = comdat any
$c = comdat any
$fn = comdat any
$hid = comdat any

@iv = linkonce_odr global i32 0, comdat #0
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)
@c = linkonce_odr constant i32 3, comdat
@hid = linkonce_odr hidden global i32 1, comdat
@pw = constant ptr @fn

define linkonce_odr void @fn() comdat {
  ret void
}

define dllexport i32 @reads() {
  %g = load atomic i8, ptr @_ZGV2iv acquire, align 8
  %i = load i32, ptr @iv
  %c = load i32, ptr @c
  %h = load i32, ptr @hid
  call void @fn()
  %s = add i32 %i, %c
  %t = add i32 %s, %h
  ret i32 %t
}

define dllexport void @addrs(ptr %out) {
  store volatile ptr @iv, ptr %out
  store volatile ptr @c, ptr %out
  store volatile ptr @fn, ptr %out
  store volatile ptr @pw, ptr %out
  ret void
}

attributes #0 = { "dynamic-init"="guarded" }

;--- b.ll
$iv = comdat any
$c = comdat any
$fn = comdat any

@iv = linkonce_odr global i32 0, comdat #0
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)
@c = linkonce_odr constant i32 3, comdat

define linkonce_odr void @fn() comdat {
  ret void
}

define dllexport i32 @readsb(ptr %out) {
  %g = load atomic i8, ptr @_ZGV2iv acquire, align 8
  %i = load i32, ptr @iv
  store volatile ptr @c, ptr %out
  store volatile ptr @fn, ptr %out
  ret i32 %i
}

attributes #0 = { "dynamic-init"="guarded" }

;--- c.ll
@iv = external dllimport global i32

define dllexport i32 @readc() {
  %i = load i32, ptr @iv
  ret i32 %i
}

;--- main.ll
declare dllimport i32 @reads()
declare dllimport i32 @readsb(ptr)

define i32 @main() {
  %a = call i32 @reads()
  %b = call i32 @readsb(ptr null)
  %s = add i32 %a, %b
  ret i32 %s
}

;--- own.ll
$k0b1cdb883a236c66 = comdat any
@k0b1cdb883a236c66 = linkonce_odr global i32 0, comdat

define ptr @own() {
  ret ptr @k0b1cdb883a236c66
}

;--- key.ll
$k35204f1f268f1afc = comdat any
@k35204f1f268f1afc = linkonce_odr global i32 0, comdat

define ptr @key() {
  ret ptr @k35204f1f268f1afc
}

;--- gnu.ll
$fn = comdat any

@gw = dso_local global ptr @fn

define linkonce_odr void @fn() comdat {
  ret void
}

define dso_local ptr @gaddr() {
  ret ptr @fn
}

define dso_local void @gcall() {
  call void @fn()
  ret void
}
;--- ef.ll
$ef = comdat any

@ef.own = internal alias ptr (), ptr @ef
@ef.pgo = weak_odr hidden alias ptr (), ptr @ef

define weak_odr dllexport ptr @ef() comdat {
  ret ptr @ef
}

define dllexport ptr @own() {
  ret ptr @ef.own
}

define dllexport ptr @pgo() {
  ret ptr @ef.pgo
}

;--- eu.ll
declare dllimport void @ef()

define dllexport void @calls() {
  call void @ef()
  ret void
}

define dllexport void @jumps() {
  tail call void @ef()
  ret void
}

define dllexport ptr @both() {
  call void @ef()
  ret ptr @ef
}

define dllexport ptr @addr() {
  ret ptr @ef
}
