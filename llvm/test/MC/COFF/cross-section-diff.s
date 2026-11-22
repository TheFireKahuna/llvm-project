// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r -x .rdata %t.o | FileCheck %s
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj -defsym=ERR=1 \
// RUN:   %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

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
.endif
