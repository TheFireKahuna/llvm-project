# REQUIRES: x86, aarch64

## The record x$tls is written from x's guard fact (_tls_index's address, then
## x's and the guard's offsets) only where an export or live code names it;
## __imp_x$tls in x's own image is a local import pointer to it.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{t} = x86_64
# DEFINE: %{as} = llvm-mc -triple=%{t}-windows-itanium -filetype=obj
# DEFINE: %{exe} = lld-link -entry:main -subsystem:console
# DEFINE: %{dll} = lld-link -dll -noentry def.obj sup.obj \
# DEFINE:   -export:x$tls,data -export:y$tls,data
# DEFINE: %{run} = \
# DEFINE:   %{as} def.s -o def.obj && %{as} %S/Inputs/tls-support.s -o sup.obj && \
# DEFINE:   %{as} use-%{t}.s -o use.obj && %{as} bad.s -o bad.obj && \
# DEFINE:   %{as} guard1.s -o guard1.obj && %{as} guard2.s -o guard2.obj && \
# DEFINE:   %{dll} -out:def.dll -implib:def.lib && \
# DEFINE:   llvm-readobj --coff-exports -x .rdata def.dll | FileCheck %s \
# DEFINE:     --check-prefixes=RECORDS,EXPORTS --implicit-check-not=Name: && \
# DEFINE:   %{dll} -out:merge.dll -merge:.tls=.data && \
# DEFINE:   llvm-readobj -x .rdata merge.dll | FileCheck %s --check-prefix=RECORDS && \
# DEFINE:   llvm-nm def.lib | FileCheck %s --check-prefix=LIB && \
# DEFINE:   %{exe} use.obj def.lib -out:import.exe && \
# DEFINE:   llvm-readobj --coff-imports import.exe | FileCheck %s --check-prefix=IMPORT && \
# DEFINE:   %{exe} use.obj def.obj sup.obj -out:local.exe && \
# DEFINE:   llvm-objdump -d local.exe | FileCheck %s --check-prefix=LOCAL-%{t} && \
# DEFINE:   llvm-readobj -x .rdata local.exe | FileCheck %s --check-prefix=ONE && \
# DEFINE:   %{exe} use.obj def.lib def.obj sup.obj -out:both.exe && \
# DEFINE:   llvm-objdump -d both.exe | FileCheck %s --check-prefix=LOCAL-%{t} && \
# DEFINE:   llvm-readobj --coff-imports both.exe | FileCheck %s --check-prefix=BOTH \
# DEFINE:     --implicit-check-not=Symbol: && \
# DEFINE:   rm -f def.a && llvm-ar rcs def.a def.obj sup.obj && \
# DEFINE:   %{exe} use.obj def.a -out:lazy.exe -verbose 2>&1 | FileCheck %s --check-prefix=LAZY && \
# DEFINE:   opt -mtriple=%{t}-windows-itanium lto.ll -o lto.bc && \
# DEFINE:   %{exe} use.obj lto.bc sup.obj -out:lto.exe && \
# DEFINE:   llvm-readobj --coff-exports -x .rdata -x .tls lto.exe | FileCheck %s --check-prefix=LTO \
# DEFINE:     --implicit-check-not=Name: && \
# DEFINE:   lld-link -dll -noentry def.obj -out:none.dll -export:y && \
# DEFINE:   not lld-link -dll -noentry def.obj -out:err.dll -export:y$tls,data 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=NOINDEX && \
# DEFINE:   not lld-link -dll -noentry def.obj bad.obj sup.obj -out:err.dll \
# DEFINE:     -export:p$tls,data -export:q$tls,data 2>&1 | FileCheck %s --check-prefix=BAD && \
# DEFINE:   not lld-link -dll -noentry guard1.obj guard2.obj sup.obj -out:err.dll 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=GUARDS && \
# DEFINE:   env LLD_IN_TEST=1 not %{exe} use.obj def.lib -out:err.exe -delayload:def.dll \
# DEFINE:     -alternatename:__delayLoadHelper2=main 2>&1 | FileCheck %s --check-prefix=DELAY && \
# DEFINE:   not lld-link -dll -noentry bad.obj def.lib -out:err.dll 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=DIRECT && \
# DEFINE:   %{as} norecord.s -o norecord.obj && \
# DEFINE:   not lld-link -dll -noentry norecord.obj sup.obj -out:err.dll 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=NORECORD

# RUN: %{run}
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## LTO never takes a record for final: bitcode that reads x through its record
## leaves the record's pointer, which the link makes the direct access where
## the image defines x, under ThinLTO too.
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj %S/Inputs/tls-support.s \
# RUN:   -o sup64.obj
# RUN: opt -mtriple=x86_64-windows-itanium lto.ll -o lto.bc
# RUN: opt -mtriple=x86_64-windows-itanium read.ll -o read.bc
# RUN: lld-link -entry:readx -include:readxy -subsystem:console read.bc lto.bc sup64.obj \
# RUN:   -out:read.exe -debug:symtab
# RUN: llvm-objdump -d --no-show-raw-insn read.exe | FileCheck %s --check-prefixes=READ,FULL
# RUN: opt -module-summary -mtriple=x86_64-windows-itanium lto.ll -o lto.bc
# RUN: opt -module-summary -mtriple=x86_64-windows-itanium read.ll -o read.bc
# RUN: lld-link -entry:readx -subsystem:console read.bc lto.bc sup64.obj \
# RUN:   -out:read.exe -debug:symtab
# RUN: llvm-objdump -d --no-show-raw-insn read.exe | FileCheck %s --check-prefix=READ
# READ:      <readx>:
# READ-NEXT: movl {{.*}}(%rip), %e[[I:[a-z]+]] # {{.*}} <_tls_index>
# READ-NEXT: movq %gs:0x58, %r[[A:[a-z]+]]
# READ-NEXT: movq (%r[[A]],%r[[I]],8), %r[[B:[a-z]+]]
# READ-NEXT: movl 0x8(%r[[B]]), %eax
# READ-NEXT: retq
## Full LTO has every record of the module's final definitions read the
## image's TLS index directly, so x and y share the index and the block.
# FULL:      <readxy>:
# FULL-NEXT: movl {{.*}}(%rip), %e[[I:[a-z]+]] # {{.*}} <_tls_index>
# FULL-NEXT: movq %gs:0x58, %r[[A:[a-z]+]]
# FULL-NEXT: movq (%r[[A]],%r[[I]],8), %r[[B:[a-z]+]]
# FULL-NEXT: movl 0x8(%r[[B]]), %eax
# FULL-NEXT: addl 0xc(%r[[B]]), %eax
# FULL-NEXT: retq

## Bitcode that names a thread-local variable that another image defines
## reaches it through the record that the import library offers.
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj def.s -o def64.obj
# RUN: lld-link -dll -noentry def64.obj sup64.obj -export:x$tls,data \
# RUN:   -out:def64.dll -implib:def64.lib
# RUN: opt -mtriple=x86_64-windows-itanium sym.ll -o sym.bc
# RUN: lld-link -entry:readx -subsystem:console sym.bc def64.lib -out:sym.exe \
# RUN:   -debug:symtab
# RUN: llvm-objdump -d --no-show-raw-insn sym.exe | FileCheck %s --check-prefix=SYM
# SYM:      <readx>:
# SYM-NEXT: movq {{.*}}(%rip), %rax # {{.*}} <__imp_x$tls>

## An export keeps a record, not the variable: _tls_index at 0x2000, x 12 and
## its guard 21 bytes into the groups, y 16 and its guard 20.
# RECORDS:      00001000 00200080 01000000
# RECORDS-NEXT: 0c000000 15000000 00200080 01000000
# RECORDS-NEXT: 10000000 14000000
# EXPORTS:      Name: x$tls
# EXPORTS:      Name: y$tls
# LIB:          D __imp_x$tls
# LIB:          D __imp_y$tls

# IMPORT:      Name: def.dll
# IMPORT:      Symbol: x$tls
# IMPORT-NOT:  Symbol:

## The image's own record beats the import library's, read first.
# BOTH: Format:

## The pointer's load takes the record's address; the dead function keeps
## neither y's pointer nor y's record.
# ONE:      00001000 00300040 01000000
# ONE-NEXT: 0c000000 15000000 00000000 00000000 {{[^ ]+$}}
# LOCAL-x86_64:       leaq {{.*}} # 0x140002028
# LOCAL-x86_64-NEXT:  movq (%rax), %rcx
# LOCAL-aarch64:      adr x8, 0x140002028
# LOCAL-aarch64-NEXT: ldr x9, [x8]

## __imp_x$tls asks for the archive member that defines x.
# LAZY: Loading lazy x from def.a for __imp_x$tls

## Bitcode keeps x and y, whose records an object or export names, and the LTO
## object states the guard.
# LTO:      00003000 00300040 01000000
# LTO-NEXT: 08000000 10000000 00300040 01000000
# LTO-NEXT: 0c000000 10000000
# LTO:      Hex dump of section '.tls':
# LTO-NEXT: 00000000 00000000 05000000 06000000
# LTO-NEXT: 0x{{[0-9a-f]+}} 01000000 00000000 00{{ }}
# LTO:      Name: y$tls

# NOINDEX: error: thread-local record y$tls needs _tls_index, which the image does not define

# BAD-DAG: error: thread-local record p$tls: the guard of p, which no object states, is not in the thread-local data of the image
# BAD-DAG: error: thread-local record q$tls: q is not in the thread-local data of the image

## The copies of a variable state one guard: under another, a thread would
## initialize the variable again or never.
# GUARDS: error: thread-local variable v has the guard g1 and in guard2.obj the guard g2

# DELAY: error: cannot delay-load def.lib(def.dll) due to import of data: __declspec(dllimport) x$tls

## A reference to the variable itself, which no image exports.
# DIRECT:      error: undefined symbol: x
# DIRECT-NEXT: >>> referenced by bad.obj:(.rdata)
# DIRECT-NEXT: NOTE: x is a thread-local variable of another image, which is reached through its record x$tls: declare it dllimport or with default visibility.

## A reference to the record of a variable whose definition states none.
# NORECORD:      error: undefined symbol: __declspec(dllimport) h$tls
# NORECORD-NEXT: >>> referenced by norecord.obj:(.data)
# NORECORD-NEXT: NOTE: the definition of h in norecord.obj states no record: it is hidden there, or of another producer; give the declaration its visibility.

#--- def.s
        .section .tls$,"dw"
        .globl  x, y
        .long   0
x:      .long   5
y:      .long   6
guard:  .byte   0
        .section .tls$,"dw",discard,__tls_set
        .globl  __tls_set
__tls_set:
        .byte   1
        .link_fact x, 7, __tls_set
        .link_fact y, 7, guard

#--- use-x86_64.s
        .text
        .globl  main
main:
        movq    __imp_x$tls(%rip), %rax
        movq    (%rax), %rcx
        movl    (%rcx), %ecx
        movl    8(%rax), %eax
        movq    %gs:88, %rdx
        movq    (%rdx,%rcx,8), %rdx
        movl    (%rdx,%rax), %eax
        retq
        .section .text,"xr",one_only,dead
dead:
        movq    __imp_y$tls(%rip), %rax
        retq

#--- use-aarch64.s
        .text
        .globl  main
main:
        adrp    x8, __imp_x$tls
        ldr     x8, [x8, :lo12:__imp_x$tls]
        ldr     x9, [x8]
        ldr     w9, [x9]
        ldr     w10, [x8, #8]
        ldr     x11, [x18, #0x58]
        ldr     x11, [x11, x9, lsl #3]
        ldr     w0, [x11, x10]
        ret
        .section .text,"xr",one_only,dead
dead:
        adrp    x8, __imp_y$tls
        ldr     x8, [x8, :lo12:__imp_y$tls]
        ret

#--- bad.s
        .section .tls$,"dw"
        .globl  p
p:      .long   0
qg:     .byte   0
        .data
        .globl  q
q:      .long   0
        .link_fact p, 7
        .link_fact q, 7, qg
        .section .rdata,"dr"
        .secrel32 x

#--- norecord.s
        .section .tls$,"dw"
        .globl  h
h:      .long   0
        .data
        .quad   __imp_h$tls

#--- guard1.s
        .section .tls$,"dw",discard,v
        .globl  v, g1
v:      .long   0
g1:     .byte   0
        .link_fact v, 7, g1

#--- guard2.s
        .section .tls$,"dw",discard,v
        .globl  v, g2
v:      .long   0
g2:     .byte   0
        .link_fact v, 7, g2

#--- sym.ll
@x = external thread_local global i32

define i32 @readx() {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @x)
  %v = load i32, ptr %a
  ret i32 %v
}

declare ptr @llvm.threadlocal.address.p0(ptr)

#--- read.ll
@"x$tls" = external dllimport constant { ptr, i32, i32 }

define i32 @readx() {
  %record = load ptr, ptr @"x$tls", !invariant.load !0
  %index = load i32, ptr %record, !invariant.load !0
  %tp = call ptr @llvm.thread.pointer.p0()
  %index64 = zext i32 %index to i64
  %slot = getelementptr [8 x i8], ptr %tp, i64 %index64
  %block = load ptr, ptr %slot
  %offset = load i32, ptr getelementptr (i8, ptr @"x$tls", i64 8), !invariant.load !0
  %offset64 = zext i32 %offset to i64
  %addr = getelementptr i8, ptr %block, i64 %offset64
  %v = load i32, ptr %addr
  ret i32 %v
}

define i32 @readxy() {
  %rx = load ptr, ptr @"x$tls", !invariant.load !0
  %ix = load i32, ptr %rx, !invariant.load !0
  %tx = call ptr @llvm.thread.pointer.p0()
  %ix64 = zext i32 %ix to i64
  %sx = getelementptr [8 x i8], ptr %tx, i64 %ix64
  %bx = load ptr, ptr %sx, !invariant.load !0
  %ox = load i32, ptr getelementptr (i8, ptr @"x$tls", i64 8), !invariant.load !0
  %ox64 = zext i32 %ox to i64
  %ax = getelementptr i8, ptr %bx, i64 %ox64
  %vx = load i32, ptr %ax
  %ry = load ptr, ptr @"y$tls", !invariant.load !0
  %iy = load i32, ptr %ry, !invariant.load !0
  %ty = call ptr @llvm.thread.pointer.p0()
  %iy64 = zext i32 %iy to i64
  %sy = getelementptr [8 x i8], ptr %ty, i64 %iy64
  %by = load ptr, ptr %sy, !invariant.load !0
  %oy = load i32, ptr getelementptr (i8, ptr @"y$tls", i64 8), !invariant.load !0
  %oy64 = zext i32 %oy to i64
  %ay = getelementptr i8, ptr %by, i64 %oy64
  %vy = load i32, ptr %ay
  %v = add i32 %vx, %vy
  ret i32 %v
}

@"y$tls" = external dllimport constant { ptr, i32, i32 }

declare ptr @llvm.thread.pointer.p0()

!0 = !{}

#--- lto.ll
$__tls_set = comdat any

@x = dso_local thread_local global i32 5, align 4, !tls_record !0
@y = dso_local thread_local global i32 6, align 4, !tls_record !0
@z = dso_local thread_local global i32 7, align 4, !tls_record !0
@__tls_set = linkonce_odr hidden thread_local constant i8 1, comdat, align 1
@llvm.compiler.used = appending global [1 x ptr] [ptr @__tls_set], section "llvm.metadata"

!llvm.linker.options = !{!1}
!0 = !{ptr @__tls_set}
!1 = !{!"-export:y$tls,data"}
