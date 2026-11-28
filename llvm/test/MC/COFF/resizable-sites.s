// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -mattr=+egpr -filetype=obj \
// RUN:   --defsym X64=1 %s -o %t.x.o
// RUN: llvm-readobj -r --coff-link-records %t.x.o | FileCheck %s --check-prefix=X64
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %s -o %t.a.o
// RUN: llvm-readobj -r --coff-link-records %t.a.o | FileCheck %s --check-prefix=A64

// On Windows Itanium and NT-POSIX the linker may change the size of the code
// at a relocation of the types COFF::relocationMayResizeCode names, so the
// assembler resolves no distance across such code, whether an instruction or
// a .reloc marker states the type. Each section holds one site between a
// label and an lea, or an adr, of that label: across a site the linker may
// resize the address has a relocation against a label that moves with the
// code, and the section is listed as resizable; across any other site the
// loads a pointer the linker may bind, an import pointer or a .refptr cell. Padding that a marker states ends at the
// marker, so a label there lies after it.

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
// X64-NEXT:   0xA IMAGE_REL_AMD64_REL32 place
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

// A64: Section ({{[0-9]+}}) .text$adrp {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$add {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEOFFSET_12A ext
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
// A64-NEXT: }
// A64-NEXT: Section ({{[0-9]+}}) .text$ldr {
// A64-NEXT:   0x0 IMAGE_REL_ARM64_PAGEOFFSET_12L __imp_f
// A64-NEXT:   0x4 IMAGE_REL_ARM64_REL21 $L
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
// A64:      Kind: Sections
// A64-NEXT: ResizableSection: .text$adrp (
// A64-NEXT: ResizableSection: .text$add (
// A64-NEXT: ResizableSection: .text$ldr (
// A64-NEXT: ResizableSection: .text$read (
// A64-NEXT: ResizableSection: .text$tail (
// A64-NEXT: ResizableSection: .text$test (
// A64-NEXT: ResizableSection: .text$scale (
// A64-NEXT: ResizableSection: .text$fjump (
// A64-NEXT: }

.ifdef X64
  .section .text$jump,"xr"
.Ljump:
  jmp .Ljump
  leaq .Ljump(%rip), %rcx
  movq __imp_f@FUSED(%rip), %rdx

  .section .text$jcc,"xr"
.Ljcc:
  je .Ljcc
  leaq .Ljcc(%rip), %rcx

  .section .text$tail,"xr"
.Ltail:
  jmp ext
  leaq .Ltail(%rip), %rcx

  .section .text$memcall,"xr"
.Lmemcall:
  callq *__imp_f(%rip)
  leaq .Lmemcall(%rip), %rcx

  .section .text$memjump,"xr"
.Lmemjump:
  jmpq *__imp_f(%rip)
  leaq .Lmemjump(%rip), %rcx

  .section .text$memjumpp1,"xr"
.Lmemjumpp1:
  rex64 jmpq *__imp_f(%rip)
  leaq .Lmemjumpp1(%rip), %rcx

  .section .text$load,"xr"
.Lload:
  movq __imp_f(%rip), %rax
  leaq .Lload(%rip), %rcx

  .section .text$loadrex2,"xr"
.Lloadrex2:
  movq __imp_f(%rip), %r16
  leaq .Lloadrex2(%rip), %rcx

  .section .text$read,"xr"
.Lread:
  movq __imp_f@FUSED(%rip), %rax
  leaq .Lread(%rip), %rcx

  .section .text$readrex2,"xr"
.Lreadrex2:
  movq __imp_f@FUSED(%rip), %r16
  leaq .Lreadrex2(%rip), %rcx

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

  .section .text$call,"xr"
.Lcall:
  callq ext
  leaq .Lcall(%rip), %rcx

  .section .text$lea,"xr"
.Llea:
  leaq ext+8(%rip), %rax
  leaq .Llea(%rip), %rcx

  .section .text$tpoff,"xr"
.Ltpoff:
  movl tv@TPOFF(%r14), %eax
  leaq .Ltpoff(%rip), %rcx

  .section .text$cell,"xr"
.Lcell:
  movq .refptr.f(%rip), %rax
  leaq .Lcell(%rip), %rcx

  .section .text$var,"xr"
.Lvar:
  movq var(%rip), %rax
  leaq .Lvar(%rip), %rcx

.else
  .section .text$adrp,"xr"
.Ladrp:
  adrp x0, ext
  adr x2, .Ladrp

  .section .text$add,"xr"
.Ladd:
  add x0, x0, :lo12:ext
  adr x2, .Ladd

  .section .text$ldr,"xr"
.Lldr:
  ldr x0, [x0, :lo12:__imp_f]
  adr x2, .Lldr

  .section .text$read,"xr"
.Lread:
  ldr x0, [x0, :fused_lo12:__imp_f]
  adr x2, .Lread

  .section .text$tail,"xr"
.Ltail:
  b ext
  adr x2, .Ltail

  .section .text$test,"xr"
.Ltest:
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_f
  cbz x8, ext
  adr x2, .Ltest

  .section .text$scale,"xr"
.Lscale:
  .reloc ., IMAGE_REL_LLVM_TLS_SCALE
  adrp x8, place
  ldr x8, [x8, :lo12:place]
.Lb2:
  movz x9, #128
  madd x0, x1, x9, x8
  adr x2, .Lb2

  .section .text$call,"xr"
.Lcall:
  bl ext
  adr x2, .Lcall

  .section .text$fcall,"xr"
.Lfcall:
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x8
  adr x2, .Lfcall

  .section .text$adr,"xr"
.Ladr:
  adr x1, ext
  adr x2, .Ladr

  .section .text$cbz,"xr"
.Lcbz:
  cbz x0, ext
  adr x2, .Lcbz


.endif
