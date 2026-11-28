# REQUIRES: x86, aarch64

## The record x$tls is written from x's guard fact (the image's place word's
## address, then x's and the guard's offsets) only where an export or live code
## names it; __imp_x$tls in x's own image is a local import pointer to it.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{t} = x86_64
# DEFINE: %{a} = x64
# DEFINE: %{as} = llvm-mc -triple=%{t}-windows-itanium -filetype=obj
# DEFINE: %{exe} = lld-link -entry:main -subsystem:console
# DEFINE: %{dll} = lld-link -dll -noentry def.obj sup.obj rt.lib fill.obj \
# DEFINE:   -export:x$tls,data -export:y$tls,data
# DEFINE: %{run} = \
# DEFINE:   %{as} def.s -o def.obj && %{as} %S/Inputs/tls-support.s -o sup.obj && \
# DEFINE:   %{as} %S/Inputs/fill-entry.s -o fill.obj && \
# DEFINE:   lld-link -machine:%{a} -def:rt.def -out:rt.lib && \
# DEFINE:   %{as} use-%{t}.s -o use.obj && %{as} bad.s -o bad.obj && \
# DEFINE:   %{as} guard1.s -o guard1.obj && %{as} guard2.s -o guard2.obj && \
# DEFINE:   %{dll} -out:def.dll -implib:def.lib && \
# DEFINE:   llvm-readobj --coff-exports -x .rdata def.dll | FileCheck %s \
# DEFINE:     --check-prefixes=RECORDS,EXPORTS --implicit-check-not=Name: && \
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
# DEFINE:   lld-link -dll -noentry def.obj rt.lib fill.obj -out:none.dll -export:y && \
# DEFINE:   not lld-link -dll -noentry def.obj -out:err.dll -export:y$tls,data 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=NOPLACE && \
# DEFINE:   not lld-link -dll -noentry def.obj bad.obj sup.obj rt.lib fill.obj -out:err.dll \
# DEFINE:     -export:p$tls,data -export:q$tls,data 2>&1 | FileCheck %s --check-prefix=BAD && \
# DEFINE:   not lld-link -dll -noentry guard1.obj guard2.obj sup.obj rt.lib fill.obj \
# DEFINE:     -out:err.dll 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=GUARDS && \
# DEFINE:   env LLD_IN_TEST=1 not %{exe} use.obj def.lib -out:err.exe -delayload:def.dll \
# DEFINE:     -alternatename:__delayLoadHelper2=main 2>&1 | FileCheck %s --check-prefix=DELAY && \
# DEFINE:   not lld-link -dll -noentry bad.obj def.lib rt.lib fill.obj -out:err.dll 2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=DIRECT && \
# DEFINE:   %{as} norecord.s -o norecord.obj && \
# DEFINE:   not lld-link -dll -noentry norecord.obj sup.obj rt.lib fill.obj -out:err.dll \
# DEFINE:     2>&1 \
# DEFINE:     | FileCheck %s --check-prefix=NORECORD

# RUN: %{run}
# REDEFINE: %{t} = aarch64
# REDEFINE: %{a} = arm64
# RUN: %{run}

## Bitcode that names a thread-local variable that another image defines
## reaches it through the record that the import library offers, which the
## link makes the thread pointer plus the variable's $tpoff word: the thread
## pointer in its register where the function is passed it, and where the
## function derived it into rax, which only the chain's own wide word's load
## and select overwrite, in rax still.
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj %S/Inputs/tls-support.s \
# RUN:   -o sup64.obj
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj %S/Inputs/fill-entry.s \
# RUN:   -o fill64.obj
# RUN: lld-link -machine:x64 -def:rt.def -out:rt64.lib
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj def.s -o def64.obj
# RUN: lld-link -dll -noentry def64.obj sup64.obj rt64.lib fill64.obj -export:x$tls,data \
# RUN:   -out:def64.dll -implib:def64.lib
# RUN: opt -mtriple=x86_64-windows-itanium sym.ll -o sym.bc
# RUN: llvm-mc -triple=x86_64-windows-itanium -filetype=obj fill-exe.s -o fill-exe.obj
# RUN: lld-link -entry:readx -subsystem:console sym.bc def64.lib rt64.lib \
# RUN:   fill-exe.obj -out:sym.exe -debug:symtab
# RUN: llvm-objdump -d --no-show-raw-insn sym.exe | FileCheck %s --check-prefix=SYM
# SYM:      <readx>:
# SYM-NOT:  __imp_x$tls
# SYM:      addq {{.*}} <__imp___tp_base$val>
# SYM-NEXT: movq {{.*}}(%rip), %rcx # 0x140003010
# SYM-NEXT: movl (%rax,%rcx), %eax
# SYM:      <readtp$tp>:
# SYM-NEXT: movq {{.*}}(%rip), %rcx # 0x140003010
# SYM-NEXT: movl (%r14,%rcx), %eax
# SYM-NEXT: retq

## An export keeps a record, not the variable: the place word at 0x3008, x 4
## and its guard 13 bytes into the template, y 8 and its guard 12. The image,
## direct, offers its importers its unit.
# RECORDS:      08300080 01000000
# RECORDS-NEXT: 04000000 0d000000 08300080 01000000
# RECORDS-NEXT: 08000000 0c000000
# EXPORTS:      Name: __tp_unit
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
## neither y's pointer nor y's record. The executable's place word holds its
## place, 0x140.
# ONE:      38200040 01000000
# ONE-NEXT: 04000000 0d000000 40010000 00000000
# LOCAL-x86_64:       leaq {{.*}} # 0x140002028
# LOCAL-x86_64-NEXT:  movq (%rax), %rcx
# LOCAL-aarch64:      adr x8, 0x140002028
# LOCAL-aarch64-NEXT: ldr x9, [x8]

## __imp_x$tls asks for the archive member that defines x.
# LAZY: Loading lazy x from def.a for __imp_x$tls

## Bitcode keeps x and y, whose records an object or export names, and the LTO
## object states the guard.
# LTO:      48200040 01000000
# LTO-NEXT: 00000000 08000000 48200040 01000000
# LTO-NEXT: 04000000 08000000
# LTO:      Hex dump of section '.tls':
# LTO-NEXT: 05000000 06000000 01{{ }}
# LTO:      Name: __tp_unit
# LTO:      Name: y$tls

# NOPLACE: error: the image's thread-locals need its place, __imp___tp_register$init, which clang_rt.wincrt_dynamic.dll's __tp_register gives; link clang_rt.wincrt_dynamic.lib

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

define dllexport i32 @readtp() "thread-pointer-argument" {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @x)
  %v = load i32, ptr %a
  ret i32 %v
}

declare ptr @llvm.threadlocal.address.p0(ptr)

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

#--- rt.def
LIBRARY clang_rt.wincrt_dynamic.dll
EXPORTS
__tp_register
__tp_map DATA
__tp_base DATA
__tp_key DATA
__tp_slow
__tp_personality

#--- fill-exe.s
.macro entry name
.section .text,"xr",one_only,\name
.globl \name
\name:
  ret
  .rva __llvm_fill_list
  .rva __llvm_fill_target
.endm
  entry __llvm_fill_exe
  entry __llvm_fill_exe_imports
.section .text,"xr",one_only,__llvm_fill_true
.globl __llvm_fill_true
__llvm_fill_true:
  ret
