// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --symbols --coff-link-records -x .xdata %t.o | \
// RUN:   FileCheck %s
// RUN: llvm-readobj --codeview %t.o | FileCheck %s --check-prefix=CV
// RUN: llvm-dwarfdump --debug-line --debug-frame %t.o 2>/dev/null | \
// RUN:   FileCheck %s --check-prefix=DWARF

// What describes code that the linker may resize, which the fused read after
// the function makes its section, follows its layout. Each
// distance that such an instruction lies in is a span from one label to
// another, which holds the distance as assembled, and each address names a
// label.

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{[0-9]+}}) .text {
// CHECK-NEXT:     0x2 IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0xA IMAGE_REL_LLVM_MEM_CALL __imp_g
// CHECK-NEXT:     0x11 IMAGE_REL_LLVM_LEA .rdata
// CHECK-NEXT:     0x18 IMAGE_REL_AMD64_SREL32 [[INTEXT:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x21 IMAGE_REL_LLVM_MEM_CALL __imp_g

// A jump table in its function's section: the lea of the table keeps its
// value under SREL32, and an entry is the span from the table to its target.
// The second entry's distance holds no such instruction, so it is a number.
// CHECK-NEXT:     0x27 IMAGE_REL_AMD64_SSPAN32 [[T1:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x27 IMAGE_REL_AMD64_PAIR [[INTEXT]]
// CHECK-NEXT:     0x32 IMAGE_REL_LLVM_FUSED_READ __imp_g
// CHECK-NEXT:   }

// The prologue of this function holds a call through a pointer, so its size
// at 0x1 and the offset of its unwind code at 0x4 are spans too. The table of
// call sites follows the handler: a site's start, its length and its landing
// pad, from a label at the function's start or at the site's.
// CHECK-NEXT:   Section ({{[0-9]+}}) .xdata {
// CHECK-NEXT:     0x1 IMAGE_REL_LLVM_SSPAN8 [[BODY:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x1 IMAGE_REL_AMD64_PAIR [[BEGIN:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x4 IMAGE_REL_LLVM_SSPAN8 [[BODY]]
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_PAIR [[BEGIN]]
// CHECK-NEXT:     0x8 IMAGE_REL_LLVM_ADDR32NB_CALL __gxx_personality_seh0
// CHECK-NEXT:     0xC IMAGE_REL_LLVM_SSPAN_ULEB [[SITE:\$L[0-9a-f]+]]
// CHECK-NEXT:     0xC IMAGE_REL_AMD64_PAIR [[BEGIN]]
// CHECK-NEXT:     0xD IMAGE_REL_LLVM_SSPAN_ULEB [[T1]]
// CHECK-NEXT:     0xD IMAGE_REL_AMD64_PAIR [[SITE]]
// CHECK-NEXT:     0xE IMAGE_REL_AMD64_SSPAN32 [[T2:\$L[0-9a-f]+]]
// CHECK-NEXT:     0xE IMAGE_REL_AMD64_PAIR [[BEGIN]]
// CHECK-NEXT:   }

// A jump table in another section: each entry names a label at its target.
// CHECK-NEXT:   Section ({{[0-9]+}}) .rdata {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_REL32 [[T1]]
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_REL32 [[T2]]
// CHECK-NEXT:   }

// CodeView has no span: the linker remaps it. Its addresses name labels.
// CHECK-NEXT:   Section ({{[0-9]+}}) .debug$S {
// CHECK-NEXT:     0xC IMAGE_REL_AMD64_SECREL [[BEGIN]]
// CHECK-NEXT:     0x10 IMAGE_REL_AMD64_SECTION [[BEGIN]]
// CHECK-NEXT:   }

// A frame's length and an advance of its CFA.
// CHECK-NEXT:   Section ({{[0-9]+}}) .debug_frame {
// CHECK-NEXT:     0x1C IMAGE_REL_AMD64_SECREL .debug_frame
// CHECK-NEXT:     0x20 IMAGE_REL_AMD64_ADDR64 [[BEGIN]]
// CHECK-NEXT:     0x28 IMAGE_REL_LLVM_SSPAN64 [[END:\$L[0-9a-f]+]]
// CHECK-NEXT:     0x28 IMAGE_REL_AMD64_PAIR [[BEGIN]]
// CHECK-NEXT:     0x31 IMAGE_REL_LLVM_USPAN8 [[BODY]]
// CHECK-NEXT:     0x31 IMAGE_REL_AMD64_PAIR [[BEGIN]]
// CHECK-NEXT:   }

// The function's entry in .pdata ends at a label.
// CHECK-NEXT:   Section ({{[0-9]+}}) .pdata {
// CHECK-NEXT:     0x0 IMAGE_REL_AMD64_ADDR32NB [[BEGIN]]
// CHECK-NEXT:     0x4 IMAGE_REL_AMD64_ADDR32NB [[END]]
// CHECK-NEXT:     0x8 IMAGE_REL_AMD64_ADDR32NB .xdata
// CHECK-NEXT:   }

// An advance of the line table across such an instruction has a fixed width.
// CHECK-NEXT:   Section ({{[0-9]+}}) .debug_line {
// CHECK-NEXT:     0x28 IMAGE_REL_AMD64_ADDR64 [[BEGIN]]
// CHECK-NEXT:     0x34 IMAGE_REL_LLVM_USPAN16 [[BODY]]
// CHECK-NEXT:     0x34 IMAGE_REL_AMD64_PAIR [[BEGIN]]
// CHECK-NEXT:     0x39 IMAGE_REL_LLVM_USPAN16 $L{{[0-9a-f]+}}
// CHECK-NEXT:     0x39 IMAGE_REL_AMD64_PAIR [[SITE]]
// CHECK-NEXT:   }
// CHECK-NEXT: ]

// The targets that the longjmp and EH continuation tables list by index are
// the symbols at them.
// CHECK:          Name: $cfgsj_f0
// CHECK-NEXT:     Value: 14
// CHECK-NEXT:     Section: .text
// CHECK:          Name: $ehgcr_0_1
// CHECK-NEXT:     Value: 30
// CHECK-NEXT:     Section: .text
// CHECK:          Name: [[BODY]]
// CHECK-NEXT:     Value: 7
// CHECK:          Name: [[BEGIN]]
// CHECK-NEXT:     Value: 0
// CHECK:          Name: [[END]]
// CHECK-NEXT:     Value: 47

// CHECK:      Hex dump of section '.xdata':
// CHECK-NEXT: 0x00000000 19070100 07300000 00000000 08162500

// CHECK:          Kind: Sections (0x2)
// CHECK-NEXT:     ResizableSection: .text

// CV:      CodeSize: 0x2F
// CV:      +0x0 [
// CV:      +0x7 [
// CV:      +0x8 [

// DWARF:      FDE cie=00000000 pc=00000000...0000002f
// DWARF:      DW_CFA_advance_loc1: 7 to 0x7
// DWARF:      0x0000000000000000 1
// DWARF-NEXT: 0x0000000000000007 2
// DWARF-NEXT: 0x0000000000000008 3
// DWARF-NEXT: 0x0000000000000036 3 {{.*}} end_sequence

  .text
  .cv_file 1 "a.c"
  .cv_func_id 0
  .file 1 "a.c"
  .def f; .scl 2; .type 32; .endef
  .globl f
f:
.Lbegin:
  .seh_proc f
  .seh_handler __gxx_personality_seh0, @unwind, @except
  .cfi_startproc
  .loc 1 1
  .cv_loc 0 1 1 0
  callq *__imp_g(%rip)
  pushq %rbx
  .seh_pushreg %rbx
  .seh_endprologue
  .cfi_def_cfa_offset 16
  .loc 1 2
  .cv_loc 0 1 2 0
  nop
.Lsite:
  .loc 1 3
  .cv_loc 0 1 3 0
  callq *__imp_g(%rip)
$cfgsj_f0:
  leaq .Ltab(%rip), %rcx
  leaq .Lintext(%rip), %rcx
  jmpq *%rax
$ehgcr_0_1:
.Lt1:
  nop
  callq *__imp_g(%rip)
.Lt2:
  popq %rbx
  retq
.Lintext:
  .long .Lt1 - .Lintext
  .long .Lt2 - .Lintext
.Lend:
  .cfi_endproc
  .seh_handlerdata
  .uleb128 .Lsite - .Lbegin
  .uleb128 .Lt1 - .Lsite
  .long .Lt2 - .Lbegin
  .text
  .seh_endproc
  movq __imp_g@FUSED(%rip), %rax
  .cfi_sections .debug_frame

  .section .rdata,"dr"
.Ltab:
  .long .Lt1 - .Ltab
  .long .Lt2 - .Ltab

  .section .gljmp$y,"dr"
  .symidx $cfgsj_f0
  .section .gehcont$y,"dr"
  .symidx $ehgcr_0_1

  .section .debug$S,"dr"
  .long 4
  .cv_linetable 0, .Lbegin, .Lend
  .cv_filechecksums
  .cv_stringtable
