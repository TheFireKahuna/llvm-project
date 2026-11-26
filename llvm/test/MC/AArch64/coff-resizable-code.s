// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym FUSED=1 %s -o %t.o
// RUN: llvm-readobj -r --symbols --coff-link-records %t.o | FileCheck %s
// RUN: llvm-objdump -d -j .text %t.o | FileCheck %s --check-prefix=DIS
// RUN: llvm-objdump -s -j .rdata %t.o | FileCheck %s --check-prefix=TABLE
// RUN: llvm-readobj --unwind %t.o | FileCheck %s --check-prefix=UNWIND
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj \
// RUN:   --defsym FUSED=1 %s -o %t.n.o
// RUN: llvm-readobj -r --symbols --coff-link-records %t.n.o | FileCheck %s
// RUN: llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s -o %t.m.o
// RUN: llvm-readobj -r --symbols %t.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=Label \
// RUN:     --implicit-check-not=IMAGE_REL_LLVM --implicit-check-not=PAIR
// RUN: llvm-readobj --unwind %t.m.o | FileCheck %s --check-prefix=MSVC-UNWIND
// RUN: not llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR --implicit-check-not=error:

// On Windows Itanium and NT-POSIX the linker may delete an instruction of an
// adrp and the add, or the load of a word or doubleword, of its address into
// its register, and a b that no unwind frame covers. A section with one is
// listed in the Sections group of the link records where every value that
// depends on its layout is a relocation.
//
// A branch, an adr or a bl whose distance crosses such an instruction keeps
// its relocation, against a label, also where it names a symbol; one whose
// distance the linker cannot change has none. An immediate that is such a distance is the page offset
// type of its instruction and a PAIR, with its constant in the field. An
// entry of a compressed jump table is USPAN8_W or USPAN16_W and a PAIR, and
// holds the distance in instructions as assembled. Alignment with
// no-operations after the first such instruction has an ALIGN relocation at
// the start of its padding, whose index holds the alignment, the four bytes
// of a no-operation and the most bytes to skip: 132 is 16 bytes, 2179 is 8
// and at most 4, and 133 is 32. An alignment of 4 bytes or less, whose
// padding resizing never changes, has none and leaves .text listed. No
// relocation refers to such a section by an offset from its start: each names
// a label, of the class of labels and with a short name.

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{[0-9]+}}) .text {
// CHECK-NEXT:     0x0 IMAGE_REL_ARM64_BRANCH19 [[AFTER:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x4 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_g
// CHECK-NEXT:     0x8 IMAGE_REL_ARM64_PAGEOFFSET_12L __imp_g
// CHECK-NEXT:     0x24 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0x28 IMAGE_REL_ARM64_PAGEOFFSET_12A var
// CHECK-NEXT:     0x2C IMAGE_REL_ARM64_BRANCH19 [[LOOP:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x30 IMAGE_REL_ARM64_BRANCH14 [[LOOP]]
// CHECK-NEXT:     0x34 IMAGE_REL_ARM64_REL21 [[LOOP]]
// CHECK-NEXT:     0x38 IMAGE_REL_ARM64_BRANCH26 [[LOOP]]
// CHECK-NEXT:     0x3C IMAGE_REL_ARM64_PAGEOFFSET_12A [[AFTER]]
// CHECK-NEXT:     0x3C IMAGE_REL_ARM64_PAIR [[LOOP]]
// CHECK-NEXT:     0x40 IMAGE_REL_ARM64_PAGEOFFSET_12L [[AFTER]]
// CHECK-NEXT:     0x40 IMAGE_REL_ARM64_PAIR [[LOOP]]
// CHECK-NEXT:     0x48 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_g
// CHECK-NEXT:     0x4C IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:     0x50 IMAGE_REL_LLVM_FUSED_CALL __imp_g
// CHECK-NEXT:     0x5C IMAGE_REL_LLVM_ALIGN - (132)
// CHECK-NEXT:     0x64 IMAGE_REL_LLVM_ALIGN - (2179)
// CHECK-NEXT:     0x68 IMAGE_REL_LLVM_BRANCH_TAIL ext
// CHECK-NEXT:     0x6C IMAGE_REL_LLVM_ALIGN - (2179)
// CHECK-NEXT:     0x78 IMAGE_REL_LLVM_ALIGN - (133)
// CHECK-NEXT:   }

// In a section the linker does not resize, the assembler resolves what it
// can, as on another triple, and no relocation names a label.

// CHECK:        Section ({{[0-9]+}}) .text$l {
// CHECK-NEXT:     0x8 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0xC IMAGE_REL_ARM64_PAGEOFFSET_12A var
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .text$p {
// CHECK-NEXT:     0x4 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0x8 IMAGE_REL_ARM64_PAGEOFFSET_12A var
// CHECK-NEXT:   }

// The unwind data of a function with such an instruction is not packed, and
// its length and the start of each epilogue scope are SSPAN_WORDS18 from a
// label at the function's start, which its .pdata entry names too. A function
// whose length the linker cannot change keeps its packed entry, also with an
// adrp and a store or a byte load, which the linker never folds.

// CHECK-NEXT:   Section ({{[0-9]+}}) .text$u {
// CHECK-NEXT:     0x8 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0xC IMAGE_REL_ARM64_PAGEOFFSET_12L var
// CHECK-NEXT:     0x10 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0x14 IMAGE_REL_ARM64_PAGEOFFSET_12L var
// CHECK-NEXT:     0x28 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0x2C IMAGE_REL_ARM64_PAGEOFFSET_12A var
// CHECK-NEXT:     0x3C IMAGE_REL_ARM64_BRANCH26 $L{{[0-9a-f]+}}
// CHECK-NEXT:     0x50 IMAGE_REL_ARM64_PAGEBASE_REL21 var
// CHECK-NEXT:     0x54 IMAGE_REL_ARM64_PAGEOFFSET_12A var
// CHECK-NEXT:   }
// CHECK-NEXT:   Section ({{[0-9]+}}) .xdata {
// CHECK-NEXT:     0x0 IMAGE_REL_LLVM_SSPAN_WORDS18 [[ATEND:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x0 IMAGE_REL_ARM64_PAIR [[UNPACKED:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_SSPAN_WORDS18 [[EPILOGUE:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x4 IMAGE_REL_ARM64_PAIR [[UNPACKED]]
// CHECK-NEXT:     0xC IMAGE_REL_LLVM_SSPAN_WORDS18 [[END:\$L[0-9a-f]+]]
// CHECK-NEXT:     0xC IMAGE_REL_ARM64_PAIR [[ATEND]]
// CHECK-NEXT:   }

// An entry of a compressed jump table whose distance is zero is a number.

// CHECK-NEXT:   Section ({{[0-9]+}}) .rdata {
// CHECK-NEXT:     0x1 IMAGE_REL_LLVM_USPAN8_W [[AFTER]]
// CHECK-NEXT:     0x1 IMAGE_REL_ARM64_PAIR [[LOOP]]
// CHECK-NEXT:     0x2 IMAGE_REL_LLVM_USPAN16_W [[AFTER]]
// CHECK-NEXT:     0x2 IMAGE_REL_ARM64_PAIR [[LOOP]]
// CHECK-NEXT:   }
// CHECK:        Section ({{[0-9]+}}) .pdata {
// CHECK:        Section ({{[0-9]+}}) .pdata {
// CHECK:        Section ({{[0-9]+}}) .pdata {
// CHECK-NEXT:     0x0 IMAGE_REL_ARM64_ADDR32NB $L{{[0-9a-f]+}}
// CHECK-NEXT:     0x8 IMAGE_REL_ARM64_ADDR32NB [[UNPACKED]]
// CHECK-NEXT:     0xC IMAGE_REL_ARM64_ADDR32NB .xdata
// CHECK-NEXT:     0x10 IMAGE_REL_ARM64_ADDR32NB [[ATEND]]
// CHECK-NEXT:     0x14 IMAGE_REL_ARM64_ADDR32NB .xdata
// CHECK-NEXT:   }

// CHECK:          Name: [[AFTER]]
// CHECK-NEXT:     Value: 72
// CHECK-NEXT:     Section: .text
// CHECK-NEXT:     BaseType: Null
// CHECK-NEXT:     ComplexType: Null
// CHECK-NEXT:     StorageClass: Label
// CHECK:          Name: [[ATEND]]
// CHECK-NEXT:     Value: 72
// CHECK:          Name: [[UNPACKED]]
// CHECK-NEXT:     Value: 36
// CHECK:          Name: [[EPILOGUE]]
// CHECK-NEXT:     Value: 52
// CHECK:          Name: [[END]]
// CHECK-NEXT:     Value: 100

// A section is not listed where its layout decides a value that has no
// relocation: data alignment after the first such instruction in .text$d, a
// literal load across one in .text$l, and one in an epilogue or a prologue,
// whose unwind codes stand for its instructions, in .text$e and .text$p.

// CHECK:      LinkRecords {
// CHECK:          Kind: Sections (0x2)
// CHECK-NEXT:     ResizableSection: .text (
// CHECK-NEXT:     ResizableSection: .text$u (
// CHECK-NEXT:   }

// The fields hold what the assembler computed: a branch its distance where
// it has no relocation, the two immediates their constants, 4 and 8, and the
// two entries the 14 instructions between their labels.

// DIS:       0: b4000000      cbz     x0, 0x0
// DIS:      14: 54ffffe1      b.ne    0x10
// DIS:      2c: 54000000      b.eq    0x2c
// DIS:      3c: 91001063      add     x3, x3, #0x4
// DIS-NEXT: 40: f9400464      ldr     x4, [x3, #0x8]
// DIS:      50: d63f0100      blr     x8
// DIS-NEXT: 54: d503201f      nop
// DIS-NEXT: 58: d503201f      nop
// DIS-NEXT: 5c: d503201f      nop
// DIS-NEXT: 60: d65f03c0      ret
// DIS-NEXT: 64: d503201f      nop
// DIS-NEXT: 68: 14000000      b       0x68
// DIS-NEXT: 6c: d503201f      nop
// DIS-NEXT: 70: d503201f      nop
// DIS-NEXT: 74: d503201f      nop
// DIS-NEXT: 78: d503201f      nop
// DIS-NEXT: 7c: d503201f      nop
// DIS-EMPTY:

// TABLE:      Contents of section .rdata:
// TABLE-NEXT: 0000 000e0e00

// UNWIND:      Function: packed
// UNWIND-NEXT: Fragment: No
// UNWIND-NEXT: FunctionLength: 36
// UNWIND:      Function: unpacked
// UNWIND:        FunctionLength: 36
// UNWIND:        EpiloguePacked: No
// UNWIND:            StartOffset: 4
// UNWIND:      Function: atend
// UNWIND:        FunctionLength: 28
// UNWIND:        EpiloguePacked: Yes

// On other triples the assembler resolves each of these, and the unwind data
// of atend is packed.

// MSVC: 0x68 IMAGE_REL_ARM64_BRANCH26 ext
// MSVC-UNWIND:      Function: atend
// MSVC-UNWIND-NEXT: Fragment: No
// MSVC-UNWIND-NEXT: FunctionLength: 28

  .text
  .globl f
f:
  cbz x0, .Lafter
  adrp x8, __imp_g
  ldr x8, [x8, :lo12:__imp_g]
  blr x8
.Lloop:
  subs x0, x0, #1
  b.ne .Lloop
  tbz x0, #3, .Lloop
  adr x1, .Lloop
  bl .Lloop
  adrp x9, var
  add x9, x9, :lo12:var
  b.eq .Lloop
  tbnz x0, #3, .Lloop
  adr x1, .Lloop
  bl .Lloop
  add x3, x3, #(.Lafter - .Lloop + 4)
  ldr x4, [x3, #(.Lafter - .Lloop + 8)]
  nop
.Lafter:
  adrp x8, __imp_g
.ifdef FUSED
  ldr x8, [x8, :fused_lo12:__imp_g]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_g
.else
  ldr x8, [x8, :lo12:__imp_g]
.endif
  blr x8
  nop
  nop
  .p2align 4
  ret
  .p2align 2
  .p2align 3, , 4
  b ext
  .p2align 3, , 4
  nop
  nop
  .p2align 5

  .section .text$d,"xr"
  adrp x0, var
  add x0, x0, :lo12:var
  .p2align 3, 0

  .section .text$e,"xr"
  .seh_proc epilogue
epilogue:
  sub sp, sp, #16
  .seh_stackalloc 16
  .seh_endprologue
  nop
  .seh_startepilogue
  adrp x0, var
  .seh_nop
  add x0, x0, :lo12:var
  .seh_nop
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  ret
  .seh_endproc

  .section .text$l,"xr"
  cbz x0, .Lliteral
  ldr x0, .Lliteral
  adrp x1, var
  add x1, x1, :lo12:var
.Lliteral:
  .xword 0

  .section .text$p,"xr"
  .seh_proc prologue
prologue:
  sub sp, sp, #16
  .seh_stackalloc 16
  adrp x0, var
  .seh_nop
  add x0, x0, :lo12:var
  .seh_nop
  .seh_endprologue
  .seh_startepilogue
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  ret
  .seh_endproc

  .section .text$u,"xr"
  .seh_proc packed
packed:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  adrp x0, var
  str x1, [x0, :lo12:var]
  adrp x0, var
  ldrb w0, [x0, :lo12:var]
  bl packed
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
  .seh_endproc
  .seh_proc unpacked
unpacked:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  .seh_endprologue
  adrp x0, var
  add x0, x0, :lo12:var
  cbz x0, .Lskip
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
.Lskip:
  bl packed
  ldp x29, x30, [sp], #16
  ret
  .seh_endproc
  .seh_proc atend
atend:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  adrp x0, var
  add x0, x0, :lo12:var
  nop
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
  .seh_endproc

  .section .rdata,"dr"
  .byte (.Lloop - .Lloop) >> 2
  .byte (.Lafter - .Lloop) >> 2
  .hword (.Lafter - .Lloop) >> 2

// The field of such an immediate holds its constant, which an add takes as
// unsigned and a load as a signed number of bytes, an entry of a compressed
// jump table is not negative, and an adrp's addend has 21 bits.
.ifdef ERR
  .section .text$x,"xr"
.Lx:
  adrp x0, var
  add x0, x0, :lo12:var
.Ly:
// ERR-DAG: [[#@LINE+1]]:{{[0-9]+}}: error: fixup value out of range
  adrp x1, var+0x100000
// ERR-DAG: [[#@LINE+1]]:{{[0-9]+}}: error: fixup value out of range
  add x1, x1, #(.Ly - .Lx - 12)
// ERR-DAG: [[#@LINE+1]]:{{[0-9]+}}: error: fixup value out of range
  ldr x1, [x1, #(.Ly - .Lx + 2048)]
  .section .rdata,"dr"
// ERR-DAG: [[#@LINE+1]]:{{[0-9]+}}: error: fixup value out of range
  .byte (.Lx - .Ly) >> 2
.endif
