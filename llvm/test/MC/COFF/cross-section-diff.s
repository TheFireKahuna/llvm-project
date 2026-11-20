// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r -x .rdata %t.o | FileCheck %s
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj -defsym=ERR=1 \
// RUN:   %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=WIDTHS=1 %s -o %t.x64.o
// RUN: llvm-readobj -r -x '.rdata$w' %t.x64.o | \
// RUN:   FileCheck %s --check-prefixes=WIDTHS,X64 -DPAIR=IMAGE_REL_AMD64_PAIR
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj \
// RUN:   -defsym=WIDTHS=1 %s -o %t.a64.o
// RUN: llvm-readobj -r -x '.rdata$w' %t.a64.o | \
// RUN:   FileCheck %s --check-prefixes=WIDTHS,A64 -DPAIR=IMAGE_REL_ARM64_PAIR
// REQUIRES: aarch64-registered-target

// A 32-bit difference A - B whose B lies outside the field's section is
// SSPAN32 naming A and PAIR naming B at the same offset, both local symbols,
// with a constant moving A's label within its section, or else B's. B must lie
// in A's section or in one associative to it, so that B's section lives while
// A's does.

// CHECK:      Section ({{[0-9]+}}) .rdata {
// CHECK-NEXT:   0x0 IMAGE_REL_AMD64_SSPAN32 f
// CHECK-NEXT:   0x0 IMAGE_REL_AMD64_PAIR .Lb
// CHECK-NEXT:   0x4 IMAGE_REL_AMD64_SSPAN32 $L.text_secrel_1
// CHECK-NEXT:   0x4 IMAGE_REL_AMD64_PAIR .Lb
// CHECK-NEXT:   0x8 IMAGE_REL_AMD64_SSPAN32 .La
// CHECK-NEXT:   0x8 IMAGE_REL_AMD64_PAIR .Lassoc
// CHECK-NEXT:   0xC IMAGE_REL_AMD64_SSPAN32 f
// CHECK-NEXT:   0xC IMAGE_REL_AMD64_PAIR $L.text_secrel_2
// CHECK-NEXT: }
// CHECK:      0x00000000 feffffff ffffffff 00000000 fdffffff

// Windows Itanium and NT-POSIX objects have a span of every width, on x86-64
// and AArch64, whose 32-bit span is a type of theirs. The field holds A - B
// where the assembler knows it.

// WIDTHS:      Section ({{[0-9]+}}) .rdata$w {
// WIDTHS-NEXT:   0x0 IMAGE_REL_LLVM_SSPAN8 f
// WIDTHS-NEXT:   0x0 [[PAIR]] .Lb
// WIDTHS-NEXT:   0x1 IMAGE_REL_LLVM_SSPAN16 f
// WIDTHS-NEXT:   0x1 [[PAIR]] .Lb
// X64-NEXT:      0x3 IMAGE_REL_AMD64_SSPAN32 f
// A64-NEXT:      0x3 IMAGE_REL_LLVM_SSPAN32 f
// WIDTHS-NEXT:   0x3 [[PAIR]] .Lb
// WIDTHS-NEXT:   0x7 IMAGE_REL_LLVM_SSPAN64 $L.text_secrel_{{[0-9]+}}
// WIDTHS-NEXT:   0x7 [[PAIR]] .Lb
// WIDTHS-NEXT: }
// X64:         0x00000000 fefefffe ffffffff ffffffff ffffff
// A64:         0x00000000 f8f8fff8 fffffff9 ffffffff ffffff

  .text
  .def f; .scl 3; .type 32; .endef
f:
  ret
  ret
.Lb:
  ret
  .globl h
h:
  ret

  .section .text$g,"xr",one_only,g
g:
.La:
  ret
  .section .data$g,"dw",associative,g
.Lassoc:
  .byte 0

  .section .rdata,"dr"
  .long f - .Lb
  .long f - .Lb + 1
  .long .La - .Lassoc
  .long f - .Lb - 1

.ifdef WIDTHS
  .section .rdata$w,"dr"
  .byte f - .Lb
  .short f - .Lb
  .long f - .Lb
  .quad f - .Lb + 1
.endif

.ifdef ERR
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: cross-section difference needs local symbols
  .long ext - .Lb
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: cross-section difference needs local symbols
  .long f - h
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: cross-section difference needs B in A's section or in one associative to it
  .long f - .Lassoc
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: cross-section difference puts its labels outside their sections
  .long f - .Lb + 6
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: Cannot represent a difference across sections
  .quad f - .Lb
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: Cannot represent a difference across sections
  .byte f - .Lb
// ERR: :[[#@LINE+1]]:{{[0-9]+}}: error: Cannot represent a difference across sections
  call f - .Lb
.endif
