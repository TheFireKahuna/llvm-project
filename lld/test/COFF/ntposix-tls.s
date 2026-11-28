# REQUIRES: x86, aarch64

## The image's template is read-only. A TPOFF adds the place the link knows
## (0x140, 48 KiB or __tp_place). Each TLS_BASE site, which the
## compiler emits as a wide DLL's, takes the image's form: a wide DLL's as
## compiled; a direct DLL's place loaded and the thread pointer added; a
## direct executable's or the runtime's link-time place added; a wide
## executable's word at tp + H decoded with its key. The bytes a form leaves
## dead are segment overrides on the site's last instruction or nops in place,
## and deleted where the section is resizable.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{x64} = llvm-mc -filetype=obj -triple=x86_64-windows-ntposix
# DEFINE: %{a64} = llvm-mc -filetype=obj -triple=aarch64-windows-ntposix
# DEFINE: %{exe} = lld-link -entry:main -subsystem:console
# DEFINE: %{dis} = llvm-objdump -d --no-show-raw-insn
# RUN: %{x64} x64.s -o x64.obj
# RUN: %{x64} x64-site.s -o x64-site.obj --defsym FIXED=1
# RUN: %{x64} x64-site.s -o x64-site-r.obj --defsym RESIZE=1
# RUN: %{x64} x64-bad.s -o x64-bad.obj
# RUN: %{x64} x64-read.s -o x64-read.obj
# RUN: %{x64} small.s -o small.obj
# RUN: %{x64} more.s -o plane.obj --defsym SIZE=800
# RUN: %{x64} more.s -o wide.obj --defsym SIZE=3000
# RUN: %{x64} place.s -o place.obj
# RUN: %{x64} end.s -o end.obj
# RUN: %{a64} a64.s -o a64.obj
# RUN: %{a64} a64-site.s -o a64-site.obj --defsym FIXED=1
# RUN: %{a64} a64-site.s -o a64-site-r.obj
# RUN: %{a64} a64-bad.s -o a64-bad.obj
# RUN: %{a64} small.s -o small-a64.obj
# RUN: %{a64} more.s -o plane-a64.obj --defsym SIZE=800
# RUN: %{a64} more.s -o wide-a64.obj --defsym SIZE=3000
# RUN: %{a64} end.s -o end-a64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64-site.s -o site-wi.obj \
# RUN:   --defsym FIXED=1
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium place.s -o place-wi.obj
# RUN: llvm-objcopy --remove-section=.drectve site-wi.obj site-pe.obj
# RUN: llvm-objcopy --remove-section=.drectve place-wi.obj place-pe.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc rev.s -o rev.obj
# RUN: llvm-objcopy --redefine-sym @comp.id=@comp.xx x64-site.obj foreign.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc pw.s -o pw.obj
## A DLL registers its thread-locals through its fill.
# RUN: lld-link -machine:x64 -def:np.def -out:np.lib
# RUN: lld-link -machine:arm64 -def:np.def -out:np-a64.lib
# RUN: %{x64} %S/Inputs/fill-entry.s -o fill.obj && llvm-ar rcs fill.lib fill.obj
# RUN: %{a64} %S/Inputs/fill-entry.s -o fill-a64.obj
# RUN: llvm-ar rcs fill-a64.lib fill-a64.obj
# DEFINE: %{dll} = lld-link -dll -noentry np.lib fill.lib
# DEFINE: %{dlla64} = lld-link -machine:arm64 -dll -noentry np-a64.lib \
# DEFINE:   fill-a64.lib

## The block: 16 bytes at 0x140, past the wide area; a site is the thread
## pointer plus the place, an lea with a SIB byte where the thread pointer is
## r12, and the site's 15 dead bytes or more no-operations before it.
# RUN: %{exe} x64.obj x64-site.obj small.obj end.obj -out:block.exe
# RUN: llvm-readobj --sections block.exe | FileCheck --check-prefix=HEADER %s
# RUN: llvm-objdump -d block.exe | FileCheck --check-prefixes=X64,X64-BLOCK %s

# HEADER:      Name: .tls
# HEADER-NEXT: VirtualSize: 0x10
# HEADER:      Characteristics [
# HEADER-NEXT:   IMAGE_SCN_ALIGN_32BYTES
# HEADER-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# HEADER-NEXT:   IMAGE_SCN_MEM_READ
# HEADER-NEXT: ]

# X64:            <.text>:
# X64-BLOCK-NEXT:   movl 0x140(%r14), %eax
# X64-BLOCK-NEXT:   leaq 0x154(%r14), %rcx
# X64-BLOCK-NEXT:   retq
# X64-BLOCK:        nopw %cs:(%rax,%rax)
# X64-BLOCK-NEXT:   49 8d 96 40 01 00 00 leaq 0x140(%r14), %rdx
# X64-BLOCK-NEXT:   nopw %cs:(%rax,%rax)
# X64-BLOCK-NEXT:   4d 8d 8c 24 40 01 00 00 leaq 0x140(%r12), %r9
# X64-BLOCK-NEXT:   nopw %cs:(%rax,%rax)
# X64-BLOCK-NEXT:   49 8d 8d 40 01 00 00 leaq 0x140(%r13), %rcx
# X64-BLOCK-NEXT:   nopw %cs:(%rax,%rax)
# X64-BLOCK-NEXT:   nop
# X64-BLOCK-NEXT:   49 8d ad 40 01 00 00 leaq 0x140(%r13), %rbp

## A dead fused read before a site is one no-operation.
# RUN: %{exe} x64.obj x64-read.obj small.obj end.obj -out:read.exe
# RUN: llvm-objdump -d read.exe | FileCheck --check-prefix=X64-READ %s

# X64-READ:      0f 1f 80 00 00 00 00 nopl
# X64-READ-NEXT: nopw %cs:(%rax,%rax)
# X64-READ-NEXT: 49 8d 96 40 01 00 00 leaq 0x140(%r14), %rdx

## The first plane: 816 bytes.
# RUN: %{exe} x64.obj small.obj plane.obj end.obj -out:plane.exe
# RUN: %{dis} plane.exe | FileCheck --check-prefixes=X64,X64-PLANE %s

# X64-PLANE-NEXT:   movl 0xc000(%r14), %eax
# X64-PLANE-NEXT:   leaq 0xc334(%r14), %rcx

## The place an image states, here a DLL.
# RUN: lld-link -dll -noentry x64.obj small.obj end.obj place.obj \
# RUN:   -out:stated.dll
# RUN: %{dis} stated.dll | FileCheck --check-prefixes=X64,X64-STATED %s

# X64-STATED-NEXT:   movl 0x30000(%r14), %eax
# X64-STATED-NEXT:   leaq 0x30014(%r14), %rcx

## No place: a DLL, at each relocation, and a wide executable, once; the
## runtime's thread-locals fit a block.
# RUN: not %{dll} x64.obj small.obj end.obj -out:none.dll \
# RUN:   2>&1 | FileCheck --check-prefix=NONE %s
# RUN: not %{exe} x64.obj small.obj wide.obj \
# RUN:   end.obj -out:none.exe 2>&1 | FileCheck --check-prefix=OVER %s
# RUN: not lld-link -dll -noentry wide.obj place.obj -out:big.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BIG %s

# NONE:      error: x64.obj: IMAGE_REL_LLVM_TPOFF32 relocation against tv needs an image whose thread-local place the link knows
# NONE-NEXT: error: x64.obj: IMAGE_REL_LLVM_TPOFF32 relocation against tend needs an image whose thread-local place the link knows
# NONE-NOT:  error:
# OVER:      error: TPOFF relocations need a thread-local place, which an executable has only for thread-locals of up to 1024 bytes; it has 3016
# OVER-NOT:  error:
# BIG:       error: __tp_place: the runtime's thread-locals exceed 1024 bytes; it has 3000

## A direct DLL loads its place and adds the thread pointer, the dead bytes
## overrides on the add, or a no-operation where they and it pass 15 bytes; a
## wide DLL's sites are as compiled, reading its place and its key.
# RUN: %{dll} x64-site.obj small.obj -out:x1.dll
# RUN: llvm-objdump -d x1.dll | FileCheck --check-prefix=X64-DIRECT %s
# RUN: %{dll} x64-site.obj small.obj wide.obj -out:x3.dll
# RUN: llvm-objdump -d x3.dll | FileCheck --check-prefix=X64-WIDE %s

# X64-DIRECT:      <.text>:
# X64-DIRECT:      48 8b 15 {{.*}} movq 0x{{.*}}(%rip), %rdx # 0x180003008
# X64-DIRECT-NEXT: 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 49 03 d6 addq %r14, %rdx
# X64-DIRECT-NEXT: movq 0x{{.*}}(%rip), %r9
# X64-DIRECT-NEXT: 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 4d 03 cc addq %r12, %r9
# X64-DIRECT-NEXT: movq 0x{{.*}}(%rip), %rcx
# X64-DIRECT-NEXT: 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 2e 49 03 cd addq %r13, %rcx
# X64-DIRECT-NEXT: movq 0x{{.*}}(%rip), %rbp
# X64-DIRECT-NEXT: nopw %cs:(%rax,%rax)
# X64-DIRECT-NEXT: 49 03 ed addq %r13, %rbp

## Its fill writes its place, kind 2 from __tp_register's entry at 0x2008,
## and then its key at place + 16, kind 1 from __tp_key's at 0x2000; with
## no import of the key the link fails.
# RUN: llvm-objdump -s -j .rdata x3.dll | FileCheck --check-prefix=X64-KEY %s
# RUN: llvm-lib -machine:x64 -def:nokey.def -out:nokey.lib
# RUN: not lld-link -dll -noentry nokey.lib fill.lib x64-site.obj small.obj \
# RUN:   wide.obj -out:nk.dll 2>&1 | FileCheck --check-prefix=NOKEY %s

# X64-KEY:      180002020 02000000 00000000 00300000 20000000
# X64-KEY-NEXT: 180002030 00000000 00000000 08300000 0a200000
# X64-KEY-NEXT: 180002040 00000000 00000000 18300000 01200000
# NOKEY: error: the image's wide thread-locals need its key, __imp___tp_key$val, which ntposix.dll's __tp_key gives; link ntposix.lib

# X64-WIDE:      <.text>:
# X64-WIDE:      movq 0x{{.*}}(%rip), %rdx # 0x180003008
# X64-WIDE-NEXT: movq (%r14,%rdx), %rdx
# X64-WIDE-NEXT: rorq $0x11, %rdx
# X64-WIDE-NEXT: xorq 0x{{.*}}(%rip), %rdx # 0x180003018

## A wide executable decodes its word at tp + H with the key its place word
## holds, the first of .sealed after the done word, {H, wide, UNFILLED}.
# RUN: %{exe} -entry:site x64-site.obj small.obj wide.obj -out:wide.exe
# RUN: llvm-objdump -s -j .sealed wide.exe > wide.txt
# RUN: llvm-objdump -d wide.exe >> wide.txt
# RUN: FileCheck --check-prefix=X64-EXE %s < wide.txt

# X64-EXE:      Contents of section .sealed:
# X64-EXE-NEXT: 140003000 00000000 00000000 00010000 00000000
# X64-EXE-NEXT: 140003010 01000000 00000000 f1f1f1f1 f1f1f1f1
# X64-EXE:      c4 c3 fb f0 96 00 01 00 00 11 rorxq $0x11, 0x100(%r14), %rdx
# X64-EXE-NEXT: 2e 2e 2e 2e 2e 48 33 15 02 20 00 00 xorq %cs:0x2002(%rip), %rdx
# X64-EXE-NEXT: c4 43 fb f0 8c 24 00 01 00 00 11 rorxq $0x11, 0x100(%r12), %r9
# X64-EXE-NEXT: 2e 2e 2e 2e 4c 33 0d ec 1f 00 00 xorq %cs:0x1fec(%rip), %r9
# X64-EXE-NEXT: rorxq $0x11, 0x100(%r13), %rcx
# X64-EXE-NEXT: xorq %cs:0x{{.*}}(%rip), %rcx
# X64-EXE-NEXT: rorxq $0x11, 0x100(%r13), %rbp
# X64-EXE-NEXT: 2e 2e 2e 2e 2e 2e 48 33 2d {{.*}} xorq %cs:0x{{.*}}(%rip), %rbp

## In a resizable section the forms leave no bytes dead.
# RUN: %{exe} -entry:site x64-site-r.obj small.obj -out:block-r.exe
# RUN: llvm-objdump -d block-r.exe | FileCheck --check-prefix=X64-R-BLOCK %s
# RUN: %{dll} x64-site-r.obj small.obj -out:x1-r.dll
# RUN: llvm-objdump -d x1-r.dll | FileCheck --check-prefix=X64-R-DIRECT %s
# RUN: %{exe} -entry:site x64-site-r.obj small.obj wide.obj -out:wide-r.exe
# RUN: llvm-objdump -d wide-r.exe | FileCheck --check-prefix=X64-R-EXE %s

# X64-R-BLOCK:      49 8d 96 40 01 00 00 leaq 0x140(%r14), %rdx
# X64-R-BLOCK-NEXT: 4d 8d 8c 24 40 01 00 00 leaq 0x140(%r12), %r9
# X64-R-BLOCK-NEXT: 49 8d 8d 40 01 00 00 leaq 0x140(%r13), %rcx
# X64-R-BLOCK-NEXT: 49 8d ad 40 01 00 00 leaq 0x140(%r13), %rbp
# X64-R-BLOCK-NEXT: eb 00 jmp

# X64-R-DIRECT:      movq 0x{{.*}}(%rip), %rdx # 0x180003008
# X64-R-DIRECT-NEXT: 49 03 d6 addq %r14, %rdx
# X64-R-DIRECT-NEXT: movq 0x{{.*}}(%rip), %r9 # 0x180003008
# X64-R-DIRECT-NEXT: 4d 03 cc addq %r12, %r9

# X64-R-EXE:      rorxq $0x11, 0x100(%r14), %rdx
# X64-R-EXE-NEXT: 48 33 15 {{.*}} xorq 0x{{.*}}(%rip), %rdx # 0x140003018
# X64-R-EXE-NEXT: rorxq $0x11, 0x100(%r12), %r9

## A site in an object that LLVM did not make for these targets.
# RUN: not %{dll} foreign.obj small.obj pw.obj -out:foreign.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=FOREIGN %s

# FOREIGN-COUNT-4: error: foreign.obj: IMAGE_REL_LLVM_TLS_BASE relocation in .text of an object that LLVM did not make for Windows Itanium or NT-POSIX
# FOREIGN-NOT:     error:

## An unknown revision of the model, /merge of .tls or into it, and in an
## image where no object states the model, as a stale one does not,
## __tp_place and a site.
# RUN: not lld-link -dll -noentry rev.obj -out:rev.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=REV %s
# RUN: not %{exe} -merge:.tls=.rdata \
# RUN:   -merge:.data=.tls x64.obj small.obj end.obj -out:merge.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=MERGE %s
# RUN: not lld-link -dll -noentry site-pe.obj place-pe.obj pw.obj -out:pe.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=PE %s

# REV:          error: rev.obj: unknown revision of the ntposix thread-local model: ntposix.2
# MERGE-COUNT-2: error: /merge: cannot merge '.tls' with any section: it is the image's thread-local template
# MERGE-NOT:    error:
# PE:           error: __tp_place is defined in an image where no object states the owned thread-local model, which has no place
# PE-COUNT-4:   error: site-pe.obj: .text+0x{{.*}}: IMAGE_REL_LLVM_TLS_BASE in an object that does not state the owned thread-local model
# PE-NOT:       error:

## A runtime's own image, which states its place and has no template, adds
## the place at each site and needs no place word.
# RUN: lld-link -dll -noentry x64-site.obj place.obj -out:rt.dll
# RUN: %{dis} rt.dll | FileCheck --check-prefix=RT %s
# RT:      leaq 0x30000(%r14), %rdx
# RT:      leaq 0x30000(%r12), %r9

## A target outside the template, and instructions that are no site.
# RUN: not %{exe} x64-bad.obj small.obj \
# RUN:   -out:bad.exe 2>&1 | FileCheck --check-prefix=X64-BAD %s

# X64-BAD:      error: x64-bad.obj: .text+0x7: IMAGE_REL_LLVM_TLS_BASE does not have the instructions its type states
# X64-BAD-NEXT: error: x64-bad.obj: .text+0x1d: IMAGE_REL_LLVM_TLS_BASE does not have the instructions its type states
# X64-BAD-NEXT: error: x64-bad.obj: IMAGE_REL_LLVM_TPOFF32 relocation against main, which is not a thread-local
# X64-BAD-NOT:  error:

## On ARM64 the link-time place is one add and a wide executable's word one
## load, and a direct DLL's place a literal load where in reach. A TPOFF's
## high part that is zero goes, the low part's add taking its base.
# RUN: %{exe} -machine:arm64 a64.obj a64-site.obj small-a64.obj end-a64.obj \
# RUN:   -out:block-a64.exe
# RUN: llvm-readobj --sections block-a64.exe | FileCheck --check-prefix=HEADER %s
# RUN: %{dis} block-a64.exe | FileCheck --check-prefixes=A64,A64-BLOCK %s
# RUN: %{exe} -machine:arm64 a64.obj a64-site.obj small-a64.obj \
# RUN:   plane-a64.obj end-a64.obj -out:plane-a64.exe
# RUN: %{dis} plane-a64.exe | FileCheck --check-prefixes=A64,A64-PLANE %s
# RUN: %{exe} -entry:site -machine:arm64 a64-site.obj small-a64.obj \
# RUN:   wide-a64.obj -out:wide-a64.exe
# RUN: %{dis} wide-a64.exe | FileCheck --check-prefix=A64-EXE %s
# RUN: %{dlla64} a64-site.obj small-a64.obj -out:a1.dll
# RUN: %{dis} a1.dll | FileCheck --check-prefix=A64-DIRECT %s
# RUN: %{dlla64} a64-site.obj small-a64.obj wide-a64.obj -out:a3.dll
# RUN: %{dis} a3.dll | FileCheck --check-prefix=A64-WIDE %s
# RUN: %{dlla64} a64-site.obj small-a64.obj -align:0x200000 -out:far.dll
# RUN: %{dis} far.dll | FileCheck --check-prefix=A64-FAR %s

# A64:            <.text>:
# A64-BLOCK-NEXT:   add x0, x28, #0x150
# A64-BLOCK-NEXT:   ldr w1, [x28, #0x148]
# A64-PLANE-NEXT:   add x0, x28, #0xc, lsl #12
# A64-PLANE-NEXT:   add x0, x0, #0x330
# A64-PLANE-NEXT:   ldr w1, [x28, #0x8]
# A64-NEXT:         ret
# A64-BLOCK-NEXT:   add x9, x28, #0x140
# A64-PLANE-NEXT:   add x9, x28, #0xc, lsl #12
# A64-NEXT:         nop
# A64-NEXT:         nop
# A64-NEXT:         nop
# A64-NEXT:         nop
# A64-NEXT:         nop
# A64-BLOCK-NEXT:   add x0, x1, #0x140
# A64-PLANE-NEXT:   add x0, x1, #0xc, lsl #12

# A64-EXE:      ldr x9, [x28, #0x100]
# A64-EXE-NEXT: nop
# A64-EXE-NEXT: nop
# A64-EXE-NEXT: nop
# A64-EXE-NEXT: ldr x10, 0x140003018
# A64-EXE-NEXT: eor x9, x10, x9, ror #17
# A64-EXE-NEXT: ldr x0, [x1, #0x100]

# A64-DIRECT:      nop
# A64-DIRECT-NEXT: ldr x9, 0x180003008
# A64-DIRECT-NEXT: add x9, x28, x9
# A64-DIRECT-NEXT: nop
# A64-DIRECT-NEXT: nop
# A64-DIRECT-NEXT: nop
# A64-DIRECT-NEXT: nop
# A64-DIRECT-NEXT: ldr x0, 0x180003008
# A64-DIRECT-NEXT: add x0, x1, x0

# A64-WIDE:      adrp x9, 0x180003000
# A64-WIDE-NEXT: ldr x9, [x9, #0x8]
# A64-WIDE-NEXT: ldr x9, [x28, x9]
# A64-WIDE-NEXT: adrp x10, 0x180003000
# A64-WIDE-NEXT: ldr x10, [x10, #0x18]
# A64-WIDE-NEXT: eor x9, x10, x9, ror #17

# A64-FAR:      adrp x9, 0x180600000
# A64-FAR-NEXT: ldr x9, [x9, #0x8]
# A64-FAR-NEXT: add x9, x28, x9
# A64-FAR-NEXT: nop

## In a resizable section: a direct DLL's place a literal load and its add, a
## wide executable's word, its key's literal load and the eor, and a wide
## DLL's loads of its place and key literal loads where in reach.
# RUN: %{exe} -entry:site -machine:arm64 a64-site-r.obj small-a64.obj \
# RUN:   -out:block-a64-r.exe
# RUN: %{dis} block-a64-r.exe | FileCheck --check-prefix=A64-R-BLOCK %s
# RUN: %{dlla64} a64-site-r.obj small-a64.obj -out:a1-r.dll
# RUN: %{dis} a1-r.dll | FileCheck --check-prefix=A64-R-DIRECT %s
# RUN: %{exe} -entry:site -machine:arm64 a64-site-r.obj small-a64.obj \
# RUN:   wide-a64.obj -out:wide-a64-r.exe
# RUN: %{dis} wide-a64-r.exe | FileCheck --check-prefix=A64-R-EXE %s
# RUN: %{dlla64} a64-site-r.obj small-a64.obj wide-a64.obj -out:a3-r.dll
# RUN: %{dis} a3-r.dll | FileCheck --check-prefix=A64-R-WIDE %s

# A64-R-BLOCK:      <.text>:
# A64-R-BLOCK-NEXT: add x9, x28, #0x140
# A64-R-BLOCK-NEXT: add x0, x1, #0x140
# A64-R-BLOCK-NEXT: ret

# A64-R-DIRECT:      ldr x9, 0x180003008
# A64-R-DIRECT-NEXT: add x9, x28, x9
# A64-R-DIRECT-NEXT: ldr x0, 0x180003008
# A64-R-DIRECT-NEXT: add x0, x1, x0
# A64-R-DIRECT-NEXT: ret

# A64-R-EXE:      <.text>:
# A64-R-EXE-NEXT: ldr x9, [x28, #0x100]
# A64-R-EXE-NEXT: ldr x10, 0x140003018
# A64-R-EXE-NEXT: eor x9, x10, x9, ror #17
# A64-R-EXE-NEXT: ldr x0, [x1, #0x100]

# A64-R-WIDE:      ldr x9, 0x180003008
# A64-R-WIDE-NEXT: ldr x9, [x28, x9]
# A64-R-WIDE-NEXT: ldr x10, 0x180003018
# A64-R-WIDE-NEXT: eor x9, x10, x9, ror #17

## Instructions that are no site.
# RUN: not %{dlla64} a64-bad.obj small-a64.obj -out:bad-a64.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=A64-BAD %s

# A64-BAD:      error: a64-bad.obj: .text+0x4: IMAGE_REL_LLVM_TLS_BASE does not have the instructions its type states
# A64-BAD-NEXT: error: a64-bad.obj: .text+0x1c: IMAGE_REL_LLVM_TLS_BASE does not have the instructions its type states
# A64-BAD-NEXT: error: a64-bad.obj: IMAGE_REL_LLVM_TPOFF_LOW12L relocation against tv needs an image whose thread-local place the link knows
# A64-BAD-NOT:  error:

## :tprel_lo12: and :dtprel_lo12:, which -mtls-size=12 writes, are checked to
## fit, the offset from the thread pointer or from the image's base plus the
## field's addend, as ELF's TPREL_LO12 and DTPREL_LO12 are.
# RUN: %{a64} a64-small.s -o a64-small.obj
# RUN: %{exe} -machine:arm64 a64-small.obj small-a64.obj end-a64.obj \
# RUN:   -out:small-a64.exe
# RUN: %{dis} small-a64.exe | FileCheck --check-prefix=A64-SMALL %s
# RUN: not %{exe} -machine:arm64 a64-small.obj small-a64.obj plane-a64.obj \
# RUN:   end-a64.obj -out:small-plane.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=A64-SMALL-ERR %s

# A64-SMALL:      <.text>:
# A64-SMALL-NEXT: ldr w1, [x28, #0x140]
# A64-SMALL-NEXT: add x0, x9, #0xdbc

# A64-SMALL-ERR:      error: a64-small.obj: IMAGE_REL_LLVM_TPOFF_LOW12L relocation against tv is out of range
# A64-SMALL-ERR-NEXT: error: a64-small.obj: IMAGE_REL_LLVM_DTPOFF_LOW12A relocation against tend is out of range

#--- x64.s
  .text
  .globl main
main:
  movl tv@TPOFF(%r14), %eax
  leaq tend@TPOFF+4(%r14), %rcx
  retq

#--- x64-site.s
## The wide DLL's form, as the compiler emits it: the base in t from the
## thread pointer tp, base and index either way and with a displacement where
## the base is rbp or r13.
.macro site t, b, i
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), \t
  movq (\b,\i), \t
  rorq $17, \t
  xorq __imp___tp_register$init+16(%rip), \t
.endm
  .text
  .globl site
site:
  site %rdx, %r14, %rdx
  site %r9, %r12, %r9
  site %rcx, %rcx, %r13
  site %rbp, %rbp, %r13
.ifdef RESIZE
  jmp .Lend
.Lend:
.endif
  retq
## An alignment the link does not redo keeps the section fixed.
.ifdef FIXED
  .p2align 2, 0
.endif

#--- x64-read.s
  .text
  .globl read
read:
  movq __imp_main@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), %rdx
  movq (%r14,%rdx), %rdx
  rorq $17, %rdx
  xorq __imp___tp_register$init+16(%rip), %rdx
  retq
  .p2align 2, 0

#--- x64-bad.s
  .text
  .globl main
main:
  movl main@TPOFF(%r14), %eax
## The rotate is of another register than the base.
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), %rdx
  movq (%r14,%rdx), %rdx
  rorq $17, %rcx
  xorq __imp___tp_register$init+16(%rip), %rdx
## The site ends the section.
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), %rdx
## Code that is not live is not checked.
  .section .text$dead,"xr",discard,dead
  .globl dead
dead:
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  movq __imp___tp_register$init(%rip), %rdx

#--- a64.s
  .text
  .globl main
main:
  add x0, x28, :tprel_hi12:tend
  add x0, x0, :tprel_lo12_nc:tend
  ldr w1, [x28, :tprel_lo12_nc:tv+8]
  ret

#--- a64-small.s
  .text
  .globl main
main:
  ldr w1, [x28, :tprel_lo12:tv]
  add x0, x9, :dtprel_lo12:tend+3500
  ret

#--- a64-site.s
## The base in t from the thread pointer tp, the key in k.
.macro site t, tp, k
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  adrp \t, __imp___tp_register$init
  ldr \t, [\t, :lo12:__imp___tp_register$init]
  ldr \t, [\tp, \t]
  adrp \k, __imp___tp_register$init+16
  ldr \k, [\k, :lo12:__imp___tp_register$init+16]
  eor \t, \k, \t, ror #17
.endm
  .text
  .globl site
site:
  site x9, x28, x10
  site x0, x1, x16
  ret
## An .org keeps the section fixed.
.ifdef FIXED
  .org . + 4
.endif

#--- a64-bad.s
  .text
  .globl main
main:
  ldr w1, [x28, :tprel_lo12:tv]
## The eor rotates the key, not the word.
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  adrp x9, __imp___tp_register$init
  ldr x9, [x9, :lo12:__imp___tp_register$init]
  ldr x9, [x28, x9]
  adrp x10, __imp___tp_register$init+16
  ldr x10, [x10, :lo12:__imp___tp_register$init+16]
  eor x9, x9, x10, ror #17
## The key's register is the base's.
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  adrp x9, __imp___tp_register$init
  ldr x9, [x9, :lo12:__imp___tp_register$init]
  ldr x9, [x28, x9]
  adrp x9, __imp___tp_register$init+16
  ldr x9, [x9, :lo12:__imp___tp_register$init+16]
  eor x9, x9, x9, ror #17

#--- small.s
  .section .tls$,"dw"
  .p2align 5
  .globl tv
tv:
  .long 7
  .zero 12

#--- more.s
  .section .tls$m,"bw"
  .zero SIZE

#--- end.s
  .section .tls$zz,"dw"
  .globl tend
tend:

#--- place.s
  .globl __tp_place
__tp_place = 0x30000

#--- np.def
LIBRARY ntposix.dll
EXPORTS
__tp_register
__tp_key

#--- pw.s
## A word of the name of the place word, for objects that have none.
  .data
  .globl __imp___tp_register$init
__imp___tp_register$init:
  .zero 24

#--- nokey.def
LIBRARY ntposix.dll
EXPORTS
__tp_register

#--- rev.s
  .section .drectve,"yn"
  .ascii " /FAILIFMISMATCH:_TLS_MODEL=ntposix.2"
