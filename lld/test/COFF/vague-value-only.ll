; REQUIRES: x86, aarch64

;; A constant that every image defines alike holds the same bytes in each, so
;; an image that takes a DLL's instance of one still reads its value in its
;; own copy: a site that its object states to only read the value is bound to
;; the copy, as a site of the image's own data, and the copy is kept, with no
;; name, while such a site is live. Every other site reaches the DLL's
;; instance, so the constant has one address. A copy serves where its group is
;; one read-only section with the checksum that the DLL states, whose words
;; hold what the DLL's hold: the address of an import, which is an in-place
;; slot, but not that of a string of its own.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: cat defs.ll dll.ll > d.ll
; RUN: cat defs.ll get.ll > unread.ll
; RUN: cat unread.ll exe.ll > e.ll

; DEFINE: %{t} = x86_64
; DEFINE: %{llc} = llc -mtriple=%{t}-windows-itanium -filetype=obj
; DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab \
; DEFINE:   -opt:ref -out:exe.exe -lldmap:exe.map
; DEFINE: %{dump} = llvm-objdump -d --no-show-raw-insn exe.exe
; DEFINE: %{run} = \
; DEFINE:   %{llc} d.ll -o d.obj && %{llc} e.ll -o e.obj && \
; DEFINE:   %{llc} unread.ll -o unread.obj && %{llc} main.ll -o main.obj && \
; DEFINE:   %{llc} taking.ll -o taking.obj && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium e.ll -o e.bc && \
; DEFINE:   lld-link -dll -noentry d.obj -out:a.dll -implib:a.lib && \
; DEFINE:   %{link} main.obj e.obj a.lib && \
; DEFINE:   %{dump} | FileCheck --check-prefix=%{t} %s && \
; DEFINE:   llvm-readobj --coff-imports exe.exe | \
; DEFINE:     FileCheck --check-prefix=NONE %s --implicit-check-not=Symbol && \
; DEFINE:   FileCheck --check-prefix=COPIES --input-file=exe.map %s && \
; DEFINE:   %{link} -opt:noref main.obj e.obj a.lib && \
; DEFINE:   %{dump} | FileCheck --check-prefix=%{t} %s && \
; DEFINE:   %{link} main.obj e.bc a.lib -include:element && \
; DEFINE:   %{dump} | FileCheck --check-prefix=%{t}-LTO %s && \
; DEFINE:   %{link} main.obj e.obj taking.obj a.lib && \
; DEFINE:   %{dump} | FileCheck --check-prefixes=%{t},%{t}-TAKEN %s && \
; DEFINE:   llvm-readobj --coff-imports exe.exe | \
; DEFINE:     FileCheck --check-prefix=TAKEN %s --implicit-check-not=Symbol && \
; DEFINE:   sed "s/i32 7, comdat/i32 8, comdat/" e.ll > differs.ll && \
; DEFINE:   %{llc} differs.ll -o differs.obj && \
; DEFINE:   %{link} main.obj differs.obj a.lib && \
; DEFINE:   %{dump} | FileCheck --check-prefix=%{t}-DIFFERS %s && \
; DEFINE:   %{link} -opt:noref main.obj unread.obj a.lib && \
; DEFINE:   FileCheck --check-prefix=UNREAD --input-file=exe.map %s \
; DEFINE:     --implicit-check-not=unread.obj:(.rdata)

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

;; A value is read where the image's own constant would be, and an element
;; through the copy's address. A word of the copy that holds an import's
;; address is the import's slot. The copy with a string's address and the
;; one with a guard do not serve, and their reads go through the DLL's.
; x86_64-LABEL: <value>:
; x86_64-NEXT:    movl {{.*}}(%rip), %eax # 0x[[#%x,K:]]
; x86_64-NEXT:    retq
; x86_64-LABEL: <element>:
; x86_64-NEXT:    leaq {{.*}}(%rip), %rax # 0x[[#%x,TBL:]]
; x86_64-NEXT:    movl (%rax,%rcx,4), %eax
; x86_64-LABEL: <word>:
; x86_64-NEXT:    leaq {{.*}}(%rip), %rax # {{.*}} <__imp_get>
; x86_64-NEXT:    movq (%rax,%rcx,8), %rax
; x86_64-LABEL: <string>:
; x86_64-NEXT:    movq {{.*}}(%rip), %rax # {{.*}} <__imp_names>
; x86_64-NEXT:    movq (%rax), %rax
; x86_64-LABEL: <guard>:
; x86_64-NEXT:    movq {{.*}}(%rip), %rax # {{.*}} <__imp_guarded>
; x86_64-NEXT:    movl (%rax), %eax

; aarch64-LABEL: <value>:
; aarch64-NEXT:    ldr w0, 0x[[#%x,K:]]
; aarch64-NEXT:    ret
; aarch64-LABEL: <element>:
; aarch64-NEXT:    adr x8, 0x[[#%x,TBL:]]
; aarch64-NEXT:    ldr w0, [x8, x0, lsl #2]
; aarch64-LABEL: <word>:
; aarch64-NEXT:    adr x8, {{.*}} <__imp_get>
; aarch64-NEXT:    ldr x0, [x8, x0, lsl #3]
; aarch64-LABEL: <string>:
; aarch64-NEXT:    ldr x8, {{.*}} <__imp_names>
; aarch64-NEXT:    ldr x0, [x8]
; aarch64-LABEL: <guard>:
; aarch64-NEXT:    ldr x8, {{.*}} <__imp_guarded>
; aarch64-NEXT:    ldr w0, [x8]

;; No site but a value read leaves the constant no import.
; NONE: Symbol: guarded (
; NONE: Symbol: names (
; NONE: Symbol: get (

; COPIES:     e.obj:(.rdata)
; COPIES:     e.obj:(.rdata)
; COPIES:     e.obj:(.rdata)

;; LTO compiles the copy, and the object it makes states its reads.
; x86_64-LTO-LABEL: <element>:
; x86_64-LTO-NEXT:    leaq {{.*}}(%rip), %rax
; x86_64-LTO-NEXT:    movl (%rax,%rcx,4), %eax
; aarch64-LTO-LABEL: <element>:
; aarch64-LTO-NEXT:    adr x8,
; aarch64-LTO-NEXT:    ldr w0, [x8, x0, lsl #2]

;; A site that takes the address reaches the DLL's instance, in the same
;; image and the same function, and the value beside it is read in the copy.
; x86_64-TAKEN-LABEL: <taken>:
; x86_64-TAKEN:         movq {{.*}}(%rip), %rcx # {{.*}} <__imp_k>
; x86_64-TAKEN-NEXT:    movl {{.*}}(%rip), %esi # 0x[[#K]]
; aarch64-TAKEN-LABEL: <taken>:
; aarch64-TAKEN:         ldr x0, {{.*}} <__imp_k>
; aarch64-TAKEN-NEXT:    ldr w19, 0x[[#K]]
; TAKEN: Symbol: guarded (
; TAKEN: Symbol: k (
; TAKEN: Symbol: names (
; TAKEN: Symbol: get (

;; A copy whose bytes are not those that the DLL states the checksum of
;; reads as the DLL's instance does only in the DLL.
; x86_64-DIFFERS-LABEL: <value>:
; x86_64-DIFFERS-NEXT:    movq {{.*}}(%rip), %rax # {{.*}} <__imp_k>
; aarch64-DIFFERS-LABEL: <value>:
; aarch64-DIFFERS-NEXT:    ldr x8, {{.*}} <__imp_k>

;; A copy that nothing reads leaves the image, which keeps the string alone.
; UNREAD:      unread.obj:(.rdata)
; UNREAD-NEXT: .str

;; An ARM64 adrp that states the same as the loads that it gives the
;; pointer's page is given the copy's pointer with them.
; RUN: llvm-mc -triple=aarch64-windows-itanium -filetype=obj shared.s -o shared.obj
; RUN: lld-link -entry:main -subsystem:console -debug:symtab -opt:ref \
; RUN:   -out:exe.exe main.obj shared.obj a.lib -include:shared
; RUN: llvm-objdump -d --no-show-raw-insn exe.exe | FileCheck --check-prefix=SHARED %s
; SHARED-LABEL: <shared>:
; SHARED-NEXT:    adrp x9, 0x[[#%x,]]
; SHARED-NEXT:    add x8, x9, #0x[[#%x,OFF:]]
; SHARED-NEXT:    add x10, x9, #0x[[#OFF]]

;--- defs.ll
$k = comdat any
$tbl = comdat any
$fns = comdat any
$names = comdat any
$guarded = comdat any

@.str = private unnamed_addr constant [3 x i8] c"hi\00"
@k = linkonce_odr constant i32 7, comdat
@tbl = weak_odr constant [4 x i32] [i32 1, i32 2, i32 3, i32 4], comdat
@fns = linkonce_odr constant [2 x ptr] [ptr @get, ptr null], comdat
@names = linkonce_odr constant [1 x ptr] [ptr @.str], comdat
@guarded = linkonce_odr constant i32 3, comdat
@_ZGV7guarded = linkonce_odr global i64 0, comdat($guarded)

;--- get.ll
declare ptr @get()

;--- dll.ll
define dllexport ptr @get() {
  %a = load volatile i32, ptr @k
  %b = load volatile i32, ptr @tbl
  %c = load volatile ptr, ptr @fns
  %d = load volatile ptr, ptr @names
  %e = load volatile i32, ptr @guarded
  %f = load volatile i64, ptr @_ZGV7guarded
  ret ptr null
}

;--- exe.ll
define i32 @value() {
  %x = load i32, ptr @k
  ret i32 %x
}

define i32 @element(i64 %i) {
  %p = getelementptr [4 x i32], ptr @tbl, i64 0, i64 %i
  %x = load i32, ptr %p
  ret i32 %x
}

define ptr @word(i64 %i) {
  %p = getelementptr [2 x ptr], ptr @fns, i64 0, i64 %i
  %x = load ptr, ptr %p
  ret ptr %x
}

define ptr @string() {
  %x = load ptr, ptr @names
  ret ptr %x
}

define i32 @guard() {
  %x = load i32, ptr @guarded
  ret i32 %x
}

;--- taking.ll
$k = comdat any

@k = linkonce_odr constant i32 7, comdat

declare void @main(ptr)

define i32 @taken() {
  %x = load i32, ptr @k
  call void @main(ptr @k)
  ret i32 %x
}

;--- main.ll
define void @main(ptr %p) {
  ret void
}

;--- shared.s
	.text
	.globl	shared
shared:
	.site_fact	7, 0
	adrp	x9, __imp_k
	.site_fact	7, 0
	ldr	x8, [x9, :lo12:__imp_k]
	.site_fact	7, 0
	ldr	x10, [x9, :lo12:__imp_k]
	ldr	w0, [x8]
	ldr	w1, [x10]
	ret

	.section	.rdata,"dr",discard,k
	.globl	k
k:
	.word	7
	.link_fact	k, 1, 1
	.link_fact	k, 2, 0
