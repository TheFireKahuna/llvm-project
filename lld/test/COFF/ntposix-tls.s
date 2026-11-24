# REQUIRES: x86, aarch64

## An image whose objects state the ntposix thread-local model keeps .tls as
## its own read-only section, whose header states the template's alignment. An
## executable's thread-locals of at most a kilobyte have a place the link
## knows: 0x100 from the thread pointer where they fit 768 bytes, else 48 KiB.
## An image that defines __tp_place has that place, and any other image
## has none. A TPOFF relocation writes the target's offset in the TLS groups,
## their first byte and their end included, plus the place. A TLS_SCALE
## relocation checks the instructions that form the image's thread-local base
## and writes the image's scale into its immediate: 1 up to a kilobyte of
## thread-locals, else their size in kilobytes rounded up. An image of the pe
## model has neither a place nor a scale, and an ntposix image's .tls merges
## with nothing.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix x64-site.s -o x64-site.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix x64-bad.s -o x64-bad.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix small.s -o small.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix more.s -o plane.obj --defsym SIZE=800
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix more.s -o wide.obj --defsym SIZE=3000
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix place.s -o place.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix end.s -o end.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix a64.s -o a64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix a64-bad.s -o a64-bad.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix small.s -o small-a64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix more.s -o plane-a64.obj --defsym SIZE=800
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix end.s -o end-a64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix more.s -o big-a64.obj --defsym SIZE=67107841
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64-site.s -o site-pe.obj \
# RUN:   --defsym FIXED=1
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium place.s -o place-pe.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc rev.s -o rev.obj

## The block: 16 bytes of thread-locals at 0x100.
# RUN: lld-link -entry:main -subsystem:console x64.obj x64-site.obj small.obj \
# RUN:   end.obj -out:block.exe
# RUN: llvm-readobj --sections block.exe | FileCheck --check-prefix=HEADER %s
# RUN: llvm-objdump -d --no-show-raw-insn block.exe | \
# RUN:   FileCheck --check-prefixes=X64,X64-BLOCK %s

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
# X64-BLOCK:        imulq $0x1, %r14, %rdx
# X64-BLOCK-NEXT:   addq {{.*}}(%rip), %rdx

## The first plane: 816 bytes.
# RUN: lld-link -entry:main -subsystem:console x64.obj small.obj plane.obj \
# RUN:   end.obj -out:plane.exe
# RUN: llvm-objdump -d --no-show-raw-insn plane.exe | \
# RUN:   FileCheck --check-prefixes=X64,X64-PLANE %s

# X64-PLANE-NEXT:   movl 0xc000(%r14), %eax
# X64-PLANE-NEXT:   leaq 0xc334(%r14), %rcx

## The place an image states, here a DLL.
# RUN: lld-link -dll -noentry x64.obj small.obj end.obj place.obj \
# RUN:   -out:stated.dll
# RUN: llvm-objdump -d --no-show-raw-insn stated.dll | \
# RUN:   FileCheck --check-prefixes=X64,X64-STATED %s

# X64-STATED-NEXT:   movl 0x30000(%r14), %eax
# X64-STATED-NEXT:   leaq 0x30014(%r14), %rcx

## No place: a DLL, at each relocation, and an executable of more than a
## kilobyte, once. A wide image's scale site takes its scale, 3 for 3016 bytes.
# RUN: not lld-link -dll -noentry x64.obj small.obj end.obj -out:none.dll \
# RUN:   2>&1 | FileCheck --check-prefix=NONE %s
# RUN: not lld-link -entry:main -subsystem:console x64.obj small.obj wide.obj \
# RUN:   end.obj -out:none.exe 2>&1 | FileCheck --check-prefix=OVER %s
# RUN: lld-link -dll -noentry x64-site.obj small.obj wide.obj -out:wide.dll
# RUN: llvm-objdump -d --no-show-raw-insn wide.dll | \
# RUN:   FileCheck --check-prefix=X64-WIDE %s

# NONE:      error: x64.obj: IMAGE_REL_LLVM_TPOFF32 relocation against tv needs an image whose thread-local place the link knows
# NONE-NEXT: error: x64.obj: IMAGE_REL_LLVM_TPOFF32 relocation against tend needs an image whose thread-local place the link knows
# NONE-NOT:  error:
# OVER:      error: TPOFF relocations need a thread-local place, which an executable has only for thread-locals of up to 1024 bytes; it has 3016
# OVER-NOT:  error:

## An unknown revision of the model, /merge of .tls or into it, and in an
## image of the pe model __tp_place and a scale site.
# RUN: not lld-link -dll -noentry rev.obj -out:rev.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=REV %s
# RUN: not lld-link -entry:main -subsystem:console -merge:.tls=.rdata \
# RUN:   -merge:.data=.tls x64.obj small.obj end.obj -out:merge.exe 2>&1 | \
# RUN:   FileCheck --check-prefix=MERGE %s
# RUN: not lld-link -dll -noentry site-pe.obj place-pe.obj -out:pe.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=PE %s

# REV:          error: rev.obj: unknown revision of the ntposix thread-local model: ntposix.2
# MERGE-COUNT-2: error: /merge: cannot merge '.tls' with any section in an image of the ntposix thread-local model
# MERGE-NOT:    error:
# PE:           error: __tp_place is defined in an image of the pe thread-local model, which has no place
# PE-NEXT:      error: site-pe.obj: the instructions at the TLS_SCALE relocation at offset 0x0 of .text are in an image of the pe thread-local model, which has no scale
# PE-NOT:       error:

# X64-WIDE:      imulq $0x3, %r14, %rdx
# X64-WIDE-NEXT: addq {{.*}}(%rip), %rdx

## A target outside the TLS groups, and instructions that are no scale site.
# RUN: not lld-link -entry:main -subsystem:console x64-bad.obj small.obj \
# RUN:   -out:bad.exe 2>&1 | FileCheck --check-prefix=X64-BAD %s

# X64-BAD:      error: x64-bad.obj: IMAGE_REL_LLVM_TPOFF32 relocation against main, which is not a thread-local
# X64-BAD-NEXT: error: x64-bad.obj: the instructions at the TLS_SCALE relocation at offset 0x7 of .text do not form a thread-local base
# X64-BAD-NEXT: error: x64-bad.obj: the instructions at the TLS_SCALE relocation at offset 0x15 of .text do not form a thread-local base
# X64-BAD-NOT:  error:

# RUN: lld-link -machine:arm64 -entry:main -subsystem:console a64.obj \
# RUN:   small-a64.obj end-a64.obj -out:block-a64.exe
# RUN: llvm-readobj --sections block-a64.exe | FileCheck --check-prefix=HEADER %s
# RUN: llvm-objdump -d --no-show-raw-insn block-a64.exe | \
# RUN:   FileCheck --check-prefixes=A64,A64-BLOCK %s
# RUN: lld-link -machine:arm64 -entry:main -subsystem:console a64.obj \
# RUN:   small-a64.obj plane-a64.obj end-a64.obj -out:plane-a64.exe
# RUN: llvm-objdump -d --no-show-raw-insn plane-a64.exe | \
# RUN:   FileCheck --check-prefixes=A64,A64-PLANE %s

# A64:            <.text>:
# A64-BLOCK-NEXT:   add x0, x28, #0x0, lsl #12
# A64-BLOCK-NEXT:   add x0, x0, #0x110
# A64-BLOCK-NEXT:   ldr w1, [x28, #0x108]
# A64-PLANE-NEXT:   add x0, x28, #0xc, lsl #12
# A64-PLANE-NEXT:   add x0, x0, #0x330
# A64-PLANE-NEXT:   ldr w1, [x28, #0x8]
# A64-NEXT:         adrp x9, 0x140002000
# A64-NEXT:         ldr x9, [x9]
# A64-NEXT:         mov x10, #0x1
# A64-NEXT:         madd x9, x28, x10, x9

## A scale of more than 16 bits, 65536 for 64 MiB, at the second site.
# RUN: not lld-link -machine:arm64 -dll -noentry a64-bad.obj small-a64.obj \
# RUN:   big-a64.obj -out:bad-a64.dll 2>&1 | FileCheck --check-prefix=A64-BAD %s

# A64-BAD:      error: a64-bad.obj: IMAGE_REL_LLVM_TPOFF_LOW12L relocation against tv needs an image whose thread-local place the link knows
# A64-BAD-NEXT: error: a64-bad.obj: the instructions at the TLS_SCALE relocation at offset 0x4 of .text do not form a thread-local base
# A64-BAD-NEXT: error: a64-bad.obj: the instructions at the TLS_SCALE relocation at offset 0x14 of .text cannot hold the image's scale, 65536, in 16 bits
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
  retq
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
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x9, cell
  ldr x9, [x9, :lo12:cell]
  movz x10, #0xffff
  madd x9, x28, x10, x9
  ret
  .data
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

#--- rev.s
  .section .drectve,"yn"
  .ascii " /FAILIFMISMATCH:_TLS_MODEL=ntposix.2"
