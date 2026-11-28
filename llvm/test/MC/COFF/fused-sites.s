// DEFINE: %{x64} = llvm-mc -triple x86_64-unknown-windows-itanium
// DEFINE: %{a64} = llvm-mc -triple aarch64-unknown-windows-itanium -defsym=A64=1
// RUN: %{x64} -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s --check-prefix=X64
// RUN: llvm-objdump -d -j '.text$f' %t.o | FileCheck %s --check-prefix=FLAGS
// RUN: not %{x64} -filetype=obj -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=X64-ERR --implicit-check-not=error:
// RUN: llvm-objdump -d -j '.text$b' %t.o | FileCheck %s --check-prefix=NEXT
// RUN: %{x64} -filetype=obj -x86-align-branch-boundary=32 \
// RUN:   -x86-align-branch=fused+jcc %s -o %t.b.o
// RUN: llvm-objdump -d -j '.text$b' %t.b.o | FileCheck %s --check-prefix=PADDED
// RUN: %{x64} -filetype=obj -defsym=BIT=1 %s -o %t.1.o
// RUN: llvm-readobj --coff-link-records %t.1.o | FileCheck %s --check-prefix=BIT
// RUN: %{x64} -filetype=obj -defsym=BIT=2 %s -o %t.2.o
// RUN: llvm-readobj --coff-link-records %t.2.o | FileCheck %s --check-prefix=BIT
// RUN: not %{x64} -filetype=obj -defsym=END=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=X64-END --implicit-check-not=error:
// RUN: %if aarch64-registered-target %{ %{a64} -filetype=obj %s -o %t.a.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -r --coff-link-records %t.a.o | FileCheck %s --check-prefix=A64 %}
// RUN: %if aarch64-registered-target %{ not %{a64} -filetype=obj -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=A64-ERR --implicit-check-not=error: %}

// On Windows Itanium and NT-POSIX a .reloc marks the next instruction as a
// fused site of the pointer whose load wrote the register it reads, which the
// assembler checks against its machine's description of the site; the checks
// every kind shares are pinned at the calls, in reloc-types-fused.s.

.ifndef A64
.ifdef BIT
// A fused add alone, and the compare of a fused test alone, are checked
// sites, which the object's link records say.
// BIT: Flags: 0x8
.if BIT == 1
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rax
.else
  cmpq $0, __imp_v@FUSED(%rip)
  je .
.endif
.else
// X64:      Section ({{[0-9]+}}) .text {
// X64-NEXT:   0x1 IMAGE_REL_LLVM_JUMP8
// X64-NEXT:   0x5 IMAGE_REL_LLVM_MEM_LOAD __imp_v
// X64-NEXT:   0x9 IMAGE_REL_LLVM_FUSED_ADD - (516)
// X64-NEXT:   0x10 IMAGE_REL_LLVM_MEM_LOAD __imp_v
// X64-NEXT:   0x14 IMAGE_REL_LLVM_FUSED_ADD - (4120)
// X64-NEXT:   0x1F IMAGE_REL_AMD64_SREL32
// X64-NEXT:   0x26 IMAGE_REL_LLVM_FUSED_READ __imp_v
// X64-NEXT:   0x2A IMAGE_REL_LLVM_FUSED_ACCESS __imp_v
// X64-NEXT:   0x2D IMAGE_REL_LLVM_FUSED_ACCESS __imp_v
// X64-NEXT:   0x34 IMAGE_REL_LLVM_FUSED_ACCESS __imp_v
// X64-NEXT:   0x3C IMAGE_REL_LLVM_FUSED_ACCESS __imp_v
// X64-NEXT:   0x41 IMAGE_REL_LLVM_MEM_LOAD __imp_v
// X64-NEXT:   0x45 IMAGE_REL_LLVM_FUSED_TEST_REG __imp_v
// X64-NEXT:   0x49 IMAGE_REL_LLVM_JUMP8
// X64-NEXT: }
// X64:      ResizableSection: .text (
// X64:      Flags: 0x8

// A fused add is the lea of a constant to the register the load directly
// before it wrote. Its index is the lea's length and, from bit 4, twice the
// constant, or twice its complement plus one where negative. The linker may
// delete the lea, so a jump or a lea of a label across it has a relocation.
  jmp 1f
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rax
  movq __imp_v(%rip), %r12
2:
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq -129(%r12), %r12
  leaq 2b(%rip), %rcx
1:

// A fused access loads or stores through the register that a load of the
// pointer wrote, or through a copy of it, with no index, no segment and a
// number as its displacement, and reads the register nowhere else. A copy of
// what the run did not write may hold the pointer.
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl 4(%rax), %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  vmovups %ymm0, (%rax)
  movq %rax, %rdx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  addl $1, 8(%rdx)
  retq
  movq %rsi, %rdx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rdx), %ecx

// A fused test is the test for zero of the register that a load of the
// pointer wrote, directly before the je or jne that alone reads its result,
// where the pointer has another reader; the compare of the pointer in memory
// is one where it has none, and is pinned in reloc-types-fused.s.
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rax, %rax
  je 1b
  movl (%rax), %ecx

// Bit 5 of a bound load's field says the flags are dead at it: the instruction
// directly after writes them all and reads none (inc keeps the carry, adc reads
// it), with nothing that lies or may come to lie between them. A load that the
// assembler resolves holds its distance.
// FLAGS:      48 8b 05 20 00 00 00   movq
// FLAGS-NEXT:                        testq
// FLAGS-NEXT: 48 8b 05 27 00 00 00   movq
// FLAGS-NEXT:                        cmpl
// FLAGS-NEXT: 48 8b 05 00 00 00 00   movq
// FLAGS-NEXT:                        incl
// FLAGS-NEXT: 48 8b 05 00 00 00 00   movq
// FLAGS-NEXT:                        adcl
// FLAGS-NEXT: 48 8b 05 00 00 00 00   movq
// FLAGS-NEXT:                        nop
// FLAGS:      48 8b 05 00 00 00 00   movq
// FLAGS-NEXT: 90                     nop
// FLAGS-NEXT:                        testq
// FLAGS-NEXT: 48 8b 05 03 00 00 00   movq
// FLAGS-NEXT:                        subl
  .section .text$f,"xr"
  movq __imp_v(%rip), %rax
  testq %rax, %rax
  movq __imp_v@FUSED(%rip), %rax
  cmpl $1, %ecx
  movq __imp_v(%rip), %rax
  incl %ecx
  movq __imp_v(%rip), %rax
  adcl $1, %ecx
  movq __imp_v(%rip), %rax
  .p2align 4
  testq %rax, %rax
  movq __imp_v(%rip), %rax
  .byte 0x90
  testq %rax, %rax
  movq __imp_l(%rip), %rax
  subl %ecx, %edx
  retq
__imp_l:
  .quad 0
// NEXT:        48 8b 05 20 00 00 00   movq
// NEXT-NEXT:                          testq
// PADDED:      48 8b 05 00 00 00 00   movq
// PADDED-NEXT:                        nop
  .section .text$b,"xr"
  .skip 22, 0x90
  movq __imp_v(%rip), %rax
  testq %rax, %rax
  je 3f
3:
  retq
  .text

.ifdef ERR
// An add writes the flags, and any other lea, or one that does not follow
// the load of its register, adds to no pointer that the linker can find.
// X64-ERR-COUNT-9: error: fused add is not at a lea of a constant directly after the load of a pointer
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  addq $16, %rax
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rcx
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax,%rcx), %rax
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq v(%rax), %rax
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 0x8000000(%rax), %rax
  movq __imp_v(%rip), %rax
  nop
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rax
  movq __imp_v(%rip), %rax
  .byte 0x90
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rax
  movq __imp_v(%rip), %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rax
  movq __imp_v@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ADD
  leaq 16(%rax), %rax

// X64-ERR-COUNT-7: error: fused access does not name a pointer at a load or store through a register alone
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax,%rcx), %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl %fs:(%rax), %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  leal 4(%rax), %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movq %rax, (%rax)
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  mulq (%rax)
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl v(%rax), %ecx
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  callq *(%rax)

// The register holds the marker's pointer, and only an access reads a copy.
  movq __imp_w(%rip), %rax
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused access reads RAX, which no load of '__imp_v' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rax), %ecx
  movq %rax, %rdx
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused access reads RDX, which no load of '__imp_v' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_v
  movl (%rdx), %ecx

// X64-ERR-COUNT-2: error: fused test does not name a pointer at a test of a 64-bit register against itself
  movq __imp_v(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rax, %rcx
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testl %eax, %eax
  movq %rax, %rdx
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused test reads RDX, which no load of '__imp_v' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rdx, %rdx
  je 1b
// Either form of a fused test is followed directly by its je or jne.
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST_REG, __imp_v
  testq %rax, %rax
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused test is not followed by a je or jne
  jb 1b
  cmpq $0, __imp_v@FUSED(%rip)
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused test is not followed by a je or jne
  nop
  cmpq $0, __imp_v@FUSED(%rip)
  .byte 0x90
// X64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused test is not followed by a je or jne
  je 1b
.endif

.ifdef END
// X64-END: error: fused test is not followed by a je or jne
  cmpq $0, __imp_v@FUSED(%rip)
.endif
.endif

.else
// A64:      Section ({{[0-9]+}}) .text {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_v
// A64-NEXT:   0x4 IMAGE_REL_LLVM_FUSED_READ __imp_v
// A64-NEXT:   0x8 IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL __imp_v
// A64-NEXT:   0xC IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL __imp_v
// A64-NEXT:   0x14 IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL __imp_v
// A64-NEXT:   0x18 IMAGE_REL_LLVM_FUSED_TEST __imp_v
// A64-NEXT: }
// A64:      Flags: 0x8

// A fused add is the add of an immediate to the register that a load of the
// pointer wrote, into which the linker may write the page offset of what the
// pointer names.
  adrp x8, __imp_v
  ldr x8, [x8, :fused_lo12:__imp_v]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  add x0, x8, #8

// A fused access is a load or a store at such a register, or at a copy of
// it, plus an offset that is a number, which takes that page offset too.
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  ldr w0, [x8, #4]
  mov x9, x8
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  stp x1, x2, [x9, #16]

// A fused test is the cbz or cbnz of such a register.
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  cbnz x8, .

.ifdef ERR
// A64-ERR-COUNT-3: error: fused add does not name a pointer at a 64-bit add of an unshifted immediate
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  add x0, x8, #8, lsl #12
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  add w0, w8, #8
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  sub x0, x8, #8
  ldr x8, [x8, :fused_lo12:__imp_w]
// A64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused add reads X8, which no load of '__imp_v' last wrote
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  add x0, x8, #8
  ldr x8, [x8, :fused_lo12:__imp_v]
  mov x9, x8
// A64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused add reads X9, which no load of '__imp_v' last wrote
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12A_IF_LOCAL, __imp_v
  add x0, x9, #8
// A64-ERR-COUNT-4: error: fused access does not name a pointer at a load or store with an offset that is a number
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  ldr w0, [x8, x1]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  ldr w0, [x8, #4]!
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  str x8, [x8]
  .reloc ., IMAGE_REL_LLVM_PAGEOFFSET_12L_IF_LOCAL, __imp_v
  ldr w0, [x8, :lo12:v]
// A64-ERR-COUNT-2: error: fused test does not name a pointer at a cbz or cbnz of a 64-bit register
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  cbz w8, .
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  tbz x8, #0, .
// A64-ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused test reads X9, which no load of '__imp_v' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_v
  cbz x9, .
.endif
.endif
