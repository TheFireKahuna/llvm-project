# REQUIRES: x86, aarch64

## The ntposix model: .tls is read-only, TPOFF adds the place the link knows
## (0x100, 48 KiB, or __tp_place), and each TLS_SCALE site takes its
## cheapest form for the image's size, dead bytes overridden, nopped or deleted.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{x64} = llvm-mc -filetype=obj -triple=x86_64-windows-ntposix
# DEFINE: %{a64} = llvm-mc -filetype=obj -triple=aarch64-windows-ntposix
# DEFINE: %{exe} = lld-link -entry:main -subsystem:console
# DEFINE: %{dis} = llvm-objdump -d --no-show-raw-insn
# RUN: %{x64} x64.s -o x64.obj
# RUN: %{x64} x64-site.s -o x64-site.obj --defsym FIXED=1
# RUN: %{x64} x64-site.s -o x64-site-r.obj --defsym RESIZE=1
# RUN: %{x64} x64-bad.s -o x64-bad.obj
# RUN: %{x64} small.s -o small.obj
# RUN: %{x64} more.s -o plane.obj --defsym SIZE=800
# RUN: %{x64} more.s -o wide.obj --defsym SIZE=3000
# RUN: %{x64} more.s -o four.obj --defsym SIZE=3100
# RUN: %{x64} place.s -o place.obj
# RUN: %{x64} end.s -o end.obj
# RUN: %{a64} a64.s -o a64.obj
# RUN: %{a64} a64-site.s -o a64-site.obj --defsym FIXED=1
# RUN: %{a64} a64-site.s -o a64-site-r.obj
# RUN: %{a64} cell.s -o cell.obj
# RUN: %{a64} a64-bad.s -o a64-bad.obj
# RUN: %{a64} small.s -o small-a64.obj
# RUN: %{a64} more.s -o plane-a64.obj --defsym SIZE=800
# RUN: %{a64} more.s -o wide-a64.obj --defsym SIZE=3000
# RUN: %{a64} more.s -o four-a64.obj --defsym SIZE=3100
# RUN: %{a64} end.s -o end-a64.obj
# RUN: %{a64} more.s -o big-a64.obj --defsym SIZE=67107841
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64-site.s -o site-pe.obj \
# RUN:   --defsym FIXED=1
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium place.s -o place-pe.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc rev.s -o rev.obj
# RUN: llvm-objcopy --redefine-sym @comp.id=@comp.xx x64-site.obj foreign.obj
## A DLL registers its thread-locals through its fill.
# RUN: lld-link -machine:x64 -def:np.def -out:np.lib
# RUN: lld-link -machine:arm64 -def:np.def -out:np-a64.lib
# RUN: %{x64} %S/Inputs/fill-entry.s -o fill.obj && llvm-ar rcs fill.lib fill.obj
# RUN: %{a64} %S/Inputs/fill-entry.s -o fill-a64.obj
# RUN: llvm-ar rcs fill-a64.lib fill-a64.obj
# DEFINE: %{dll} = lld-link -dll -noentry np.lib fill.lib
# DEFINE: %{dlla64} = lld-link -machine:arm64 -dll -noentry np-a64.lib \
# DEFINE:   fill-a64.lib
# DEFINE: %{x64dll} = %{dll} x64-site.obj small.obj
# DEFINE: %{a64dll} = %{dlla64} a64-site.obj cell.obj small-a64.obj

## The block: 16 bytes at 0x100; a scale site is the thread pointer plus the
## place, an lea with a SIB byte where the thread pointer is r12.
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
# X64-BLOCK-NEXT:   movl 0x100(%r14), %eax
# X64-BLOCK-NEXT:   leaq 0x114(%r14), %rcx
# X64-BLOCK-NEXT:   retq
# X64-BLOCK:        2e 2e 2e 2e 2e 2e 2e 49 8d 96 00 01 00 00 leaq %cs:0x100(%r14), %rdx
# X64-BLOCK-NEXT:   2e 2e 2e 2e 2e 2e 2e 4d 8d 89 00 01 00 00 leaq %cs:0x100(%r9), %r9
# X64-BLOCK-NEXT:   2e 2e 2e 2e 2e 2e 2e 49 8d 8d 00 01 00 00 leaq %cs:0x100(%r13), %rcx
# X64-BLOCK-NEXT:   2e 2e 2e 2e 2e 2e 49 8d 84 24 00 01 00 00 leaq %cs:0x100(%r12), %rax

## A dead fused read before a site is one no-operation; before any other
## instruction it is overrides on it.
# RUN: %{x64} x64-read.s -o x64-read.obj
# RUN: %{exe} x64.obj x64-read.obj small.obj end.obj -out:read.exe
# RUN: llvm-objdump -d read.exe | FileCheck --check-prefix=X64-READ %s

# X64-READ:      0f 1f 80 00 00 00 00 nopl
# X64-READ-NEXT: 2e 2e 2e 2e 2e 2e 2e 49 8d 96 00 01 00 00 leaq %cs:0x100(%r14), %rdx
# X64-READ-NEXT: 2e 2e 2e 2e 2e 2e 2e c3 retq

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

## No place: a DLL, at each relocation, and an executable of more than a
## kilobyte, once.
# RUN: not %{dll} x64.obj small.obj end.obj -out:none.dll \
# RUN:   2>&1 | FileCheck --check-prefix=NONE %s
# RUN: not %{exe} x64.obj small.obj wide.obj \
# RUN:   end.obj -out:none.exe 2>&1 | FileCheck --check-prefix=OVER %s

# NONE:      error: x64.obj: IMAGE_REL_LLVM_TPOFF32 relocation against tv needs an image whose thread-local place the link knows
# NONE-NEXT: error: x64.obj: IMAGE_REL_LLVM_TPOFF32 relocation against tend needs an image whose thread-local place the link knows
# NONE-NOT:  error:
# OVER:      error: TPOFF relocations need a thread-local place, which an executable has only for thread-locals of up to 1024 bytes; it has 3016
# OVER-NOT:  error:

## Without a place: at scale 1 the add alone where the registers are one; at 3
## (3016 bytes) an lea where it can; at 4 (3116 bytes) a shift; else the imul.
# RUN: %{x64dll} -out:x1.dll
# RUN: llvm-objdump -d x1.dll | FileCheck --check-prefix=X64-1 %s
# RUN: %{x64dll} wide.obj -out:x3.dll
# RUN: llvm-objdump -d x3.dll | FileCheck --check-prefix=X64-3 %s
# RUN: %{x64dll} four.obj -out:x4.dll
# RUN: llvm-objdump -d x4.dll | FileCheck --check-prefix=X64-4 %s

# X64-1:      <.text>:
# X64-1-NEXT: imulq $0x1, %r14, %rdx
# X64-1-NEXT: addq 0x{{.*}}(%rip), %rdx
# X64-1-NEXT: 2e 2e 2e 2e 2e 2e 2e 4c 03 0d {{.*}} addq %cs:0x{{.*}}(%rip), %r9
# X64-1-NEXT: imulq $0x1, %r13, %rcx
# X64-1-NEXT: addq 0x{{.*}}(%rip), %rcx
# X64-1-NEXT: imulq $0x1, %r12, %rax
# X64-1-NEXT: addq 0x{{.*}}(%rip), %rax

# X64-3:      <.text>:
# X64-3-NEXT: 4b 8d 14 76 leaq (%r14,%r14,2), %rdx
# X64-3-NEXT: 2e 2e 2e 48 03 15 {{.*}} addq %cs:0x{{.*}}(%rip), %rdx
# X64-3-NEXT: 4f 8d 0c 49 leaq (%r9,%r9,2), %r9
# X64-3-NEXT: 2e 2e 2e 4c 03 0d {{.*}} addq %cs:0x{{.*}}(%rip), %r9
# X64-3-NEXT: imulq $0x3, %r13, %rcx
# X64-3-NEXT: addq 0x{{.*}}(%rip), %rcx
# X64-3-NEXT: 4b 8d 04 64 leaq (%r12,%r12,2), %rax
# X64-3-NEXT: 2e 2e 2e 48 03 05 {{.*}} addq %cs:0x{{.*}}(%rip), %rax

# X64-4:      <.text>:
# X64-4-NEXT: imulq $0x4, %r14, %rdx
# X64-4-NEXT: addq 0x{{.*}}(%rip), %rdx
# X64-4-NEXT: 49 c1 e1 02 shlq $0x2, %r9
# X64-4-NEXT: 2e 2e 2e 4c 03 0d {{.*}} addq %cs:0x{{.*}}(%rip), %r9
# X64-4-NEXT: imulq $0x4, %r13, %rcx
# X64-4-NEXT: addq 0x{{.*}}(%rip), %rcx
# X64-4-NEXT: imulq $0x4, %r12, %rax
# X64-4-NEXT: addq 0x{{.*}}(%rip), %rax

## In a resizable section the forms leave no bytes dead.
# RUN: %{exe} x64.obj x64-site-r.obj small.obj end.obj -out:block-r.exe
# RUN: llvm-objdump -d block-r.exe | FileCheck --check-prefix=X64-R-BLOCK %s
# RUN: %{dll} x64-site-r.obj small.obj -out:x1-r.dll
# RUN: llvm-objdump -d x1-r.dll | FileCheck --check-prefix=X64-R1 %s
# RUN: %{dll} x64-site-r.obj small.obj wide.obj -out:x3-r.dll
# RUN: llvm-objdump -d x3-r.dll | FileCheck --check-prefix=X64-R3 %s

# X64-R-BLOCK:      49 8d 96 00 01 00 00 leaq 0x100(%r14), %rdx
# X64-R-BLOCK-NEXT: 4d 8d 89 00 01 00 00 leaq 0x100(%r9), %r9
# X64-R-BLOCK-NEXT: 49 8d 8d 00 01 00 00 leaq 0x100(%r13), %rcx
# X64-R-BLOCK-NEXT: 49 8d 84 24 00 01 00 00 leaq 0x100(%r12), %rax
# X64-R-BLOCK-NEXT: eb 00 jmp

# X64-R1:      <.text>:
# X64-R1-NEXT: imulq $0x1, %r14, %rdx
# X64-R1-NEXT: addq 0x{{.*}}(%rip), %rdx
# X64-R1-NEXT: 4c 03 0d {{.*}} addq 0x{{.*}}(%rip), %r9
# X64-R1-NEXT: imulq $0x1, %r13, %rcx

# X64-R3:      <.text>:
# X64-R3-NEXT: 4b 8d 14 76 leaq (%r14,%r14,2), %rdx
# X64-R3-NEXT: 48 03 15 {{.*}} addq 0x{{.*}}(%rip), %rdx
# X64-R3-NEXT: 4f 8d 0c 49 leaq (%r9,%r9,2), %r9
# X64-R3-NEXT: 4c 03 0d {{.*}} addq 0x{{.*}}(%rip), %r9
# X64-R3-NEXT: imulq $0x3, %r13, %rcx

## A scale site in an object that LLVM did not make for these targets.
# RUN: not %{dll} foreign.obj small.obj -out:foreign.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=FOREIGN %s

# FOREIGN-COUNT-4: error: foreign.obj: IMAGE_REL_LLVM_TLS_SCALE relocation in .text of an object that LLVM did not make for Windows Itanium or NT-POSIX
# FOREIGN-NOT:     error:

## An unknown revision of the model, /merge of .tls or into it, and in an
## image of the pe model __tp_place and a scale site.
# RUN: not lld-link -dll -noentry rev.obj -out:rev.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=REV %s
# RUN: not %{exe} -merge:.tls=.rdata \
# RUN:   -merge:.data=.tls x64.obj small.obj end.obj -out:merge.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=MERGE %s
# RUN: not lld-link -dll -noentry site-pe.obj place-pe.obj -out:pe.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=PE %s

# REV:          error: rev.obj: unknown revision of the ntposix thread-local model: ntposix.2
# MERGE-COUNT-2: error: /merge: cannot merge '.tls' with any section in an image of the ntposix thread-local model
# MERGE-NOT:    error:
# PE-COUNT-4:   error: site-pe.obj: .text+0x{{.*}}: IMAGE_REL_LLVM_TLS_SCALE
# PE-NEXT:      error: __tp_place is defined in an image of the pe thread-local model, which has no place
# PE-NOT:       error:

## A target outside the TLS groups, and instructions that are no scale site.
# RUN: not %{exe} x64-bad.obj small.obj \
# RUN:   -out:bad.exe 2>&1 | FileCheck --check-prefix=X64-BAD %s

# X64-BAD:      error: x64-bad.obj: .text+0x7: IMAGE_REL_LLVM_TLS_SCALE
# X64-BAD-NEXT: error: x64-bad.obj: .text+0x15: IMAGE_REL_LLVM_TLS_SCALE
# X64-BAD-NEXT: error: x64-bad.obj: IMAGE_REL_LLVM_TPOFF32 relocation against main, which is not a thread-local
# X64-BAD-NOT:  error:

## On ARM64 the thread pointer plus the place is one add, which cannot name
## the zero register, and the place's load is a literal load.
# RUN: %{exe} -machine:arm64 a64.obj \
# RUN:   a64-site.obj cell.obj small-a64.obj end-a64.obj -out:block-a64.exe
# RUN: llvm-readobj --sections block-a64.exe | FileCheck --check-prefix=HEADER %s
# RUN: %{dis} block-a64.exe | FileCheck --check-prefixes=A64,A64-BLOCK %s
# RUN: %{exe} -machine:arm64 a64.obj \
# RUN:   a64-site.obj cell.obj small-a64.obj plane-a64.obj end-a64.obj \
# RUN:   -out:plane-a64.exe
# RUN: %{dis} plane-a64.exe | FileCheck --check-prefixes=A64,A64-PLANE %s

# A64:            <.text>:
# A64-BLOCK-NEXT:   add x0, x28, #0x0, lsl #12
# A64-BLOCK-NEXT:   add x0, x0, #0x110
# A64-BLOCK-NEXT:   ldr w1, [x28, #0x108]
# A64-PLANE-NEXT:   add x0, x28, #0xc, lsl #12
# A64-PLANE-NEXT:   add x0, x0, #0x330
# A64-PLANE-NEXT:   ldr w1, [x28, #0x8]
# A64-NEXT:         ret
# A64-NEXT:         nop
# A64-NEXT:         nop
# A64-NEXT:         nop
# A64-BLOCK-NEXT:   add x9, x28, #0x100
# A64-PLANE-NEXT:   add x9, x28, #0xc, lsl #12
# A64-NEXT:         nop
# A64-NEXT:         ldr x9, 0x140003000
# A64-NEXT:         nop
# A64-NEXT:         add xzr, x9, x28

## Without a place: a shift, at scale 1 the add alone, else the multiply; the
## place's adrp and ldr where it is out of a literal load's reach.
# RUN: %{a64dll} -out:a1.dll
# RUN: %{dis} a1.dll | FileCheck --check-prefix=A64-1 %s
# RUN: %{a64dll} wide-a64.obj -out:a3.dll
# RUN: %{dis} a3.dll | FileCheck --check-prefix=A64-3 %s
# RUN: %{a64dll} four-a64.obj -out:a4.dll
# RUN: %{dis} a4.dll | FileCheck --check-prefix=A64-4 %s
# RUN: %{a64dll} -align:0x200000 -out:far.dll
# RUN: %{dis} far.dll | FileCheck --check-prefix=A64-FAR %s

# A64-1:        <.text>:
# A64-1-NEXT:   nop
# A64-1-NEXT:   ldr x9, 0x180004000
# A64-1-NEXT:   nop
# A64-1-NEXT:   add x9, x9, x28{{$}}
# A64-3:        <.text>:
# A64-3-NEXT:   nop
# A64-3-NEXT:   ldr x9, 0x180004000
# A64-3-NEXT:   mov x10, #0x3
# A64-3-NEXT:   madd x9, x28, x10, x9
# A64-4:        <.text>:
# A64-4-NEXT:   nop
# A64-4-NEXT:   ldr x9, 0x180004000
# A64-4-NEXT:   nop
# A64-4-NEXT:   add x9, x9, x28, lsl #2
# A64-FAR:      <.text>:
# A64-FAR-NEXT: adrp x9, 0x180800000
# A64-FAR-NEXT: ldr x9, [x9]
# A64-FAR-NEXT: nop
# A64-FAR-NEXT: add x9, x9, x28{{$}}

# RUN: %{exe} -machine:arm64 a64.obj \
# RUN:   a64-site-r.obj cell.obj small-a64.obj end-a64.obj -out:block-a64-r.exe
# RUN: %{dis} block-a64-r.exe | FileCheck --check-prefix=A64-R-BLOCK %s
# RUN: %{dlla64} a64-site-r.obj cell.obj \
# RUN:   small-a64.obj -out:a1-r.dll
# RUN: %{dis} a1-r.dll | FileCheck --check-prefix=A64-R1 %s
# RUN: %{dlla64} a64-site-r.obj cell.obj \
# RUN:   small-a64.obj wide-a64.obj -out:a3-r.dll
# RUN: %{dis} a3-r.dll | FileCheck --check-prefix=A64-R3 %s

# A64-R-BLOCK:      ret
# A64-R-BLOCK-NEXT: add x9, x28, #0x100
# A64-R-BLOCK-NEXT: ldr x9, 0x140003000
# A64-R-BLOCK-NEXT: add xzr, x9, x28
# A64-R-BLOCK-NEXT: ret

# A64-R1:      <.text>:
# A64-R1-NEXT: ldr x9, 0x180004000
# A64-R1-NEXT: add x9, x9, x28{{$}}
# A64-R1-NEXT: ldr x9, 0x180004000
# A64-R1-NEXT: add xzr, x9, x28{{$}}
# A64-R1-NEXT: ret

# A64-R3:      <.text>:
# A64-R3-NEXT: ldr x9, 0x180004000
# A64-R3-NEXT: mov x10, #0x3
# A64-R3-NEXT: madd x9, x28, x10, x9

## A scale of more than 16 bits, 65536 for 64 MiB, at the second site.
# RUN: not %{dlla64} a64-bad.obj small-a64.obj \
# RUN:   big-a64.obj -out:bad-a64.dll 2>&1 | FileCheck --check-prefix=A64-BAD %s

# A64-BAD:      error: a64-bad.obj: .text+0x4: IMAGE_REL_LLVM_TLS_SCALE
# A64-BAD-NEXT: error: a64-bad.obj: IMAGE_REL_LLVM_TPOFF_LOW12L relocation against tv needs an image whose thread-local place the link knows
# A64-BAD-NEXT: error: a64-bad.obj: .text+0x14: IMAGE_REL_LLVM_TLS_SCALE
# A64-BAD-NOT:  error:

#--- x64.s
  .text
  .globl main
main:
  movl tv@TPOFF(%r14), %eax
  leaq tend@TPOFF+4(%r14), %rcx
  retq

#--- x64-site.s
  .text
  .globl site
site:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %rdx
  addq place(%rip), %rdx
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r9, %r9
  addq place(%rip), %r9
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r13, %rcx
  addq place(%rip), %rcx
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r12, %rax
  addq place(%rip), %rax
.ifdef RESIZE
  jmp .Lend
.Lend:
.endif
  retq
## The fill is data, so the link does not resize the section.
.ifdef FIXED
  .p2align 2, 0
.endif
  .data
place:
  .quad 0

## The fill is data, so the link does not resize this section.
#--- x64-read.s
  .text
  .globl read
read:
  movq __imp_main@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %rdx
  addq place(%rip), %rdx
  movq __imp_main@FUSED(%rip), %rax
  retq
  .p2align 2, 0
  .data
place:
  .quad 0

#--- x64-bad.s
  .text
  .globl main
main:
  movl main@TPOFF(%r14), %eax
## The sum goes to another register than the product.
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %rdx
  addq place(%rip), %rcx
## The site ends the section.
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %rdx
## Code that is not live is not checked.
  .section .text$dead,"xr",discard,dead
  .globl dead
dead:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %rdx
  .data
place:
  .quad 0

#--- a64.s
  .text
  .globl main
main:
  add x0, x28, :tprel_hi12:tend
  add x0, x0, :tprel_lo12_nc:tend
  ldr w1, [x28, :tprel_lo12:tv+8]
  ret

#--- a64-site.s
  .text
  .globl site
site:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x9, cell
  ldr x9, [x9, :lo12:cell]
  movz x10, #0xffff
  madd x9, x28, x10, x9
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x9, cell
  ldr x9, [x9, :lo12:cell]
  movz x10, #0xffff
  madd xzr, x28, x10, x9
  ret
## An .org keeps the section fixed.
.ifdef FIXED
  .org . + 4
.endif

#--- cell.s
  .data
  .globl cell
cell:
  .quad 0

#--- a64-bad.s
  .text
  .globl main
main:
  ldr w1, [x28, :tprel_lo12:tv]
## The multiply reads the place where the scale is.
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x9, main
  ldr x9, [x9, :lo12:main]
  movz x10, #0
  madd x9, x28, x9, x10
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x9, main
  ldr x9, [x9, :lo12:main]
  movz x10, #0
  madd x9, x28, x10, x9

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

#--- rev.s
  .section .drectve,"yn"
  .ascii " /FAILIFMISMATCH:_TLS_MODEL=ntposix.2"
