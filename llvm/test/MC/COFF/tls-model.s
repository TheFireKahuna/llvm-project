## An object for NT-POSIX states the owned thread-local model in a
## /FAILIFMISMATCH key, and one for Windows Itanium states none. In one, @TPOFF in a 32-bit absolute field is
## TPOFF32; on AArch64 :tprel_hi12: is TPOFF_HIGH12A, and :tprel_lo12: and
## :tprel_lo12_nc: are TPOFF_LOW12A on an add and TPOFF_LOW12L on a load or a
## store. @GOTTPOFF in a field relative to rip at the end of its instruction
## is GOTTPOFF32, its addend in the field, and on AArch64 :gottprel: on an adrp
## and :gottprel_lo12: on a 64-bit load are GOTTPOFF_PAGE21 and
## GOTTPOFF_LOW12L, each a relocation against the thread-local whose offset the
## word the linker makes holds, as on ELF. .reloc marks the instructions that form an image's thread-local base
## with TLS_BASE, which names no symbol. Each error is one row under ERR.

# REQUIRES: aarch64-registered-target
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-ntposix %s -o %t.obj
# RUN: llvm-readobj -r --string-dump=.drectve %t.obj | \
# RUN:   FileCheck --check-prefixes=X64,NTPOSIX %s
# RUN: llvm-mc -triple=x86_64-windows-ntposix %s | FileCheck --check-prefix=X64-ASM %s
# RUN: llvm-mc -triple=aarch64-windows-ntposix --defsym A64=1 %s | \
# RUN:   FileCheck --check-prefix=A64-ASM %s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium %s -o %t.obj
# RUN: llvm-readobj --string-dump=.drectve %t.obj | FileCheck --check-prefix=ITANIUM %s
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-ntposix --defsym A64=1 %s -o %t.obj
# RUN: llvm-readobj -r %t.obj | FileCheck --check-prefix=A64 %s
# RUN: not llvm-mc -filetype=obj -triple=x86_64-windows-ntposix --defsym ERR=1 \
# RUN:   %s -o /dev/null 2>&1 | FileCheck --check-prefix=ERR %s
# RUN: not llvm-mc -filetype=obj -triple=aarch64-windows-ntposix --defsym ERR=1 \
# RUN:   --defsym A64=1 %s -o /dev/null 2>&1 | FileCheck --check-prefix=A64-ERR %s
# RUN: not llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o /dev/null \
# RUN:   2>&1 | FileCheck --check-prefix=MSVC %s
# RUN: not llvm-mc -filetype=obj -triple=aarch64-windows-msvc --defsym A64=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck --check-prefixes=MSVC,A64-MSVC %s

# X64:      0x3 IMAGE_REL_LLVM_TPOFF32 tv
# X64-NEXT: 0xA IMAGE_REL_LLVM_TPOFF32 tv
# X64-NEXT: 0x11 IMAGE_REL_LLVM_TPOFF32 tv
# X64-NEXT: 0x15 IMAGE_REL_LLVM_TLS_BASE - (0)
# X64-NEXT: 0x1F IMAGE_REL_LLVM_REL32_INSN place
# X64-NEXT: 0x23 IMAGE_REL_LLVM_TPOFF32 tv
# X64-NEXT: 0x2A IMAGE_REL_LLVM_GOTTPOFF32 tv
# X64-NEXT: 0x35 IMAGE_REL_LLVM_GOTTPOFF32 tv
# X64-NEXT: 0x3C IMAGE_REL_LLVM_GOTTPOFF32 tv
# NTPOSIX: /INCLUDE:__llvm_link_records_v1 /FAILIFMISMATCH:_TLS_MODEL=ntposix.1{{$}}
# ITANIUM: /INCLUDE:__llvm_link_records_v1{{$}}

# X64-ASM:      movq tv@GOTTPOFF(%rip), %rax
# X64-ASM-NEXT: movl (%r14,%rax), %eax
# X64-ASM-NEXT: addq tv@GOTTPOFF(%rip), %rcx
# X64-ASM-NEXT: movq tv@GOTTPOFF+8(%rip), %r9
# A64-ASM:      adrp x11, :gottprel:tv
# A64-ASM-NEXT: ldr x11, [x11, :gottprel_lo12:tv]
# A64-ASM-NEXT: ldr w1, [x28, x11]

# A64:      0x0 IMAGE_REL_LLVM_TPOFF_HIGH12A tv
# A64-NEXT: 0x4 IMAGE_REL_LLVM_TPOFF_LOW12A tv
# A64-NEXT: 0x8 IMAGE_REL_LLVM_TPOFF_LOW12A tv
# A64-NEXT: 0xC IMAGE_REL_LLVM_TPOFF_LOW12L tv
# A64-NEXT: 0x10 IMAGE_REL_LLVM_TPOFF_LOW12L tv
# A64-NEXT: 0x14 IMAGE_REL_ARM64_PAGEBASE_REL21 place
# A64-NEXT: 0x14 IMAGE_REL_LLVM_TLS_BASE - (0)
# A64-NEXT: 0x18 IMAGE_REL_ARM64_PAGEOFFSET_12L place
# A64-NEXT: 0x24 IMAGE_REL_LLVM_GOTTPOFF_PAGE21 tv
# A64-NEXT: 0x28 IMAGE_REL_LLVM_GOTTPOFF_LOW12L tv

# ERR-COUNT-2: error: @TPOFF is not a 32-bit absolute field
# ERR-COUNT-2: error: @GOTTPOFF is not a field relative to rip at the end of its instruction
# A64-ERR:     error: relocation specifier :tprel_g0: unsupported on COFF targets
# A64-ERR:     error: relocation specifier :gottprel_lo12: unsupported on COFF targets
# A64-ERR:     error: relocation specifier :gottprel: unsupported on COFF targets

# MSVC-DAG:     error: unknown relocation name
# MSVC-DAG:     error: relocation specifier {{@TPOFF|:tprel_hi12:}} unsupported on COFF targets
# MSVC-DAG:     error: relocation specifier {{@GOTTPOFF|:gottprel:}} unsupported on COFF targets
# A64-MSVC-DAG: error: relocation specifier :tprel_lo12: unsupported on COFF targets

  .text
f:
.ifndef A64
  movl tv@TPOFF(%r14), %eax
  leaq tv@TPOFF+4(%r14), %rcx
  addq $tv@TPOFF, %rcx
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  imulq $128, %r14, %rdx
  addq place(%rip), %rdx
  .long tv@TPOFF
  movq tv@GOTTPOFF(%rip), %rax
  movl (%r14,%rax), %eax
  addq tv@gottpoff(%rip), %rcx
  movq tv@gottpoff+8(%rip), %r9
.ifdef ERR
  .quad tv@TPOFF
  movl tv@TPOFF(%rip), %eax
  cmpq $1, tv@gottpoff(%rip)
  leaq tv@gottpoff(%rip), %rax
.endif
.else
  add x0, x28, :tprel_hi12:tv
  add x0, x0, :tprel_lo12_nc:tv
  add x0, x28, :tprel_lo12:tv
  ldr w1, [x28, :tprel_lo12:tv]
  str x1, [x28, :tprel_lo12_nc:tv+8]
  .reloc ., IMAGE_REL_LLVM_TLS_BASE
  adrp x9, place
  ldr x9, [x9, :lo12:place]
  movz x10, #0
  madd x9, x28, x10, x9
  adrp x11, :gottprel:tv
  ldr x11, [x11, :gottprel_lo12:tv]
  ldr w1, [x28, x11]
.ifdef ERR
  movz x0, #:tprel_g0:tv
  ldr w0, [x0, :gottprel_lo12:tv]
  ldr x0, :gottprel:tv
.endif
.endif
