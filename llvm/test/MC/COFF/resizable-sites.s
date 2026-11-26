// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -mattr=+egpr -filetype=obj \
// RUN:   --defsym X64=1 %s -o %t.x.o
// RUN: llvm-readobj -r --coff-link-records %t.x.o | FileCheck %s --check-prefix=X64
// RUN: %if aarch64-registered-target %{ llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %s -o %t.a.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -r --coff-link-records %t.a.o | FileCheck %s --check-prefix=A64 %}

// On Windows Itanium and NT-POSIX each section holds one site between a label
// and an lea or adr of it: across a site the linker may resize, the address
// names a label and the section is resizable; across any other the distance is
// written. A trailing fused read makes an x86-64 section resizable. An ARM64
// adrp is resizable with the instruction after it, which the linker may fold
// with it where it is an add, or a load of a word, signed word or doubleword
// into an integer register, of the address into the adrp's register, its
// field the low bits of the adrp's addend, and not alone.

// X64: Section ({{[0-9]+}}) .text$jump {
// X64-NEXT:   0x5 IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0xC IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$jcc {
// X64-NEXT:   0x5 IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0xC IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$tail {
// X64-NEXT:   0x1 IMAGE_REL_LLVM_BRANCH_TAIL ext
// X64-NEXT:   0x8 IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0xF IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$memcall {
// X64-NEXT:   0x2 IMAGE_REL_LLVM_MEM_CALL __imp_f
// X64-NEXT:   0x9 IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0x10 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$memjump {
// X64-NEXT:   0x2 IMAGE_REL_LLVM_MEM_JUMP_TAIL __imp_f
// X64-NEXT:   0x9 IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0x10 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$memjumpp1 {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_MEM_JUMP_P1 __imp_f
// X64-NEXT:   0xA IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0x11 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$load {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_MEM_LOAD __imp_f
// X64-NEXT:   0xA IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0x11 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$loadrex2 {
// X64-NEXT:   0x4 IMAGE_REL_LLVM_MEM_LOAD_REX2 __imp_f
// X64-NEXT:   0xB IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0x12 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$read {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT:   0xA IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$readrex2 {
// X64-NEXT:   0x4 IMAGE_REL_LLVM_FUSED_READ_REX2 __imp_f
// X64-NEXT:   0xB IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$test {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_FUSED_TEST __imp_f
// X64-NEXT:   0x9 IMAGE_REL_LLVM_JUMP8 $L
// X64-NEXT:   0xD IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$scale {
// X64-NEXT:   0x0 IMAGE_REL_LLVM_TLS_SCALE -
// X64-NEXT:   0xA IMAGE_REL_LLVM_REL32_INSN place
// X64-NEXT:   0x11 IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$align {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT:   0x7 IMAGE_REL_LLVM_ALIGN -
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$call {
// X64-NEXT:   0x1 IMAGE_REL_LLVM_BRANCH32 ext
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$lea {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_LEA_OFFSET ext
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$tpoff {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_TPOFF32 tv
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$cell {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_MEM_LOAD .refptr.f
// X64-NEXT:   0xA IMAGE_REL_AMD64_SREL32 $L
// X64-NEXT:   0x11 IMAGE_REL_LLVM_FUSED_READ __imp_f
// X64-NEXT: }
// X64-NEXT: Section ({{[0-9]+}}) .text$var {
// X64-NEXT:   0x3 IMAGE_REL_LLVM_MEM_LOAD var
// X64-NEXT: }
// X64:      Kind: Sections
// X64-NEXT: ResizableSection: .text$jump (
// X64-NEXT: ResizableSection: .text$jcc (
// X64-NEXT: ResizableSection: .text$tail (
// X64-NEXT: ResizableSection: .text$memcall (
// X64-NEXT: ResizableSection: .text$memjump (
// X64-NEXT: ResizableSection: .text$memjumpp1 (
// X64-NEXT: ResizableSection: .text$load (
// X64-NEXT: ResizableSection: .text$loadrex2 (
// X64-NEXT: ResizableSection: .text$read (
// X64-NEXT: ResizableSection: .text$readrex2 (
// X64-NEXT: ResizableSection: .text$test (
// X64-NEXT: ResizableSection: .text$scale (
// X64-NEXT: ResizableSection: .text$align (
// X64-NEXT: ResizableSection: .text$cell (
// X64-NEXT: }

// A64: Section ({{[0-9]+}}) .text$add {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12A ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldr {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldrw {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldrsw {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$addk {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12A ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldrk {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT:   0x8 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$read {
// A64-NEXT:   0x0 IMAGE_REL_LLVM_FUSED_READ __imp_f
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$tail {
// A64-NEXT:   0x0 IMAGE_REL_LLVM_BRANCH_TAIL ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$test {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_BRANCH19 ext
// A64-NEXT:   0x0 IMAGE_REL_LLVM_FUSED_TEST __imp_f
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$scale {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 place
// A64-NEXT:   0x0 IMAGE_REL_LLVM_TLS_SCALE -
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L place
// A64-NEXT:   0x10 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$call {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_BRANCH26 ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$fcall {
// A64-NEXT:   0x0 IMAGE_REL_LLVM_FUSED_CALL __imp_f
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$adr {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_REL21 ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$cbz {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_BRANCH19 ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$fjump {
// A64-NEXT:   0x0 IMAGE_REL_LLVM_FUSED_JUMP_TAIL __imp_f
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$adrp {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$str {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldrh {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldrd {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$prfm {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$other {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_PAGEOFFSET_12L ext
// A64-NEXT: }
// A64:      Kind: Sections
// A64-NEXT: ResizableSection: .text$add (
// A64-NEXT: ResizableSection: .text$ldr (
// A64-NEXT: ResizableSection: .text$ldrw (
// A64-NEXT: ResizableSection: .text$ldrsw (
// A64-NEXT: ResizableSection: .text$addk (
// A64-NEXT: ResizableSection: .text$ldrk (
// A64-NEXT: ResizableSection: .text$read (
// A64-NEXT: ResizableSection: .text$tail (
// A64-NEXT: ResizableSection: .text$test (
// A64-NEXT: ResizableSection: .text$scale (
// A64-NEXT: ResizableSection: .text$fjump (
// A64-NEXT: }

.ifdef X64
// A section with one site, the lea of its label and, for fsite, a fused read.
.macro site name, insn:vararg
  .section .text$\name,"xr"
.L\name:
  \insn
  leaq .L\name(%rip), %rcx
.endm
.macro fsite name, insn:vararg
  site \name, \insn
  movq __imp_f@FUSED(%rip), %rdx
.endm

  fsite jump, jmp .Ljump
  fsite jcc, je .Ljcc
  fsite tail, jmp ext
  fsite memcall, callq *__imp_f(%rip)
  fsite memjump, jmpq *__imp_f(%rip)
  fsite memjumpp1, rex64 jmpq *__imp_f(%rip)
  fsite load, movq __imp_f(%rip), %rax
  fsite loadrex2, movq __imp_f(%rip), %r16
  site read, movq __imp_f@FUSED(%rip), %rax
  site readrex2, movq __imp_f@FUSED(%rip), %r16

  .section .text$test,"xr"
.Ltest:
  cmpq $0, __imp_f@FUSED(%rip)
  je .Ltest
  leaq .Ltest(%rip), %rcx

  .section .text$scale,"xr"
.Lscale:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  imulq $128, %r14, %rdx
  addq place(%rip), %rdx
  leaq .Lscale(%rip), %rcx

  .section .text$align,"xr"
  movq __imp_f@FUSED(%rip), %rdx
.Lalign:
  .reloc ., IMAGE_REL_LLVM_ALIGN, 100
  nop
  leaq .Lalign(%rip), %rcx

  site call, callq ext
  site lea, leaq ext+8(%rip), %rax
  site tpoff, movl tv@TPOFF(%r14), %eax
  fsite cell, movq .refptr.f(%rip), %rax
  site var, movq var(%rip), %rax

.else
// A section with one site, which rsite marks, and the adr of its label. The
// site of pair is an adrp of to and the instruction after it.
.macro site name, insn:vararg
  .section .text$\name,"xr"
.L\name:
  \insn
  adr x2, .L\name
.endm
.macro rsite name, type, insn:vararg
  .section .text$\name,"xr"
.L\name:
  .reloc ., \type, __imp_f
  \insn
  adr x2, .L\name
.endm

.macro pair name, to, low:vararg
  .section .text$\name,"xr"
.L\name:
  adrp x0, \to
  \low
  adr x2, .L\name
.endm

  pair add, ext, add x0, x0, :lo12:ext
  pair ldr, ext, ldr x0, [x0, :lo12:ext]
  pair ldrw, ext, ldr w0, [x0, :lo12:ext]
  pair ldrsw, ext, ldrsw x0, [x0, :lo12:ext]
  pair addk, ext+8, add x0, x0, :lo12:ext+8
  pair ldrk, ext-8, ldr x0, [x0, :lo12:ext-8]
  site read, ldr x0, [x0, :fused_lo12:__imp_f]
  site tail, b ext
  rsite test, IMAGE_REL_LLVM_FUSED_TEST, cbz x8, ext

  .section .text$scale,"xr"
.Lscale:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x8, place
  ldr x8, [x8, :lo12:place]
.Lb2:
  movz x9, #128
  madd x0, x1, x9, x8
  adr x2, .Lb2

  site call, bl ext
  rsite fcall, IMAGE_REL_LLVM_FUSED_CALL, blr x8
  site adr, adr x1, ext
  site cbz, cbz x0, ext
  rsite fjump, IMAGE_REL_LLVM_FUSED_JUMP, br x8
  site adrp, adrp x0, ext
  pair str, ext, str x1, [x0, :lo12:ext]
  pair ldrh, ext, ldrh w0, [x0, :lo12:ext]
  pair ldrd, ext, ldr d0, [x0, :lo12:ext]
  pair prfm, ext, prfm pldl1keep, [x0, :lo12:ext]
  pair other, ext, ldr x1, [x0, :lo12:ext]

.endif
