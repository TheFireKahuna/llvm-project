// COFF has no relocation for an ELF specifier such as @GOTPCREL, which is an
// error rather than a plain reference to the symbol, which would load the
// symbol's value where the instruction expects its address. @PLT on a call
// reaches the function either way, and the COFF specifiers keep their meaning.

// RUN: not llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --implicit-check-not=error:
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj -defsym=OK=1 %s -o %t.o
// RUN: llvm-objdump -dr -s -j .text -j .data %t.o | FileCheck %s --check-prefix=OK

// CHECK: error: relocation specifier @GOTPCREL unsupported on COFF targets
// CHECK: error: relocation specifier @GOTOFF unsupported on COFF targets
// CHECK: error: relocation specifier @TPOFF unsupported on COFF targets

// OK:      callq
// OK-NEXT:   IMAGE_REL_AMD64_REL32 f
// OK:        IMAGE_REL_AMD64_SECREL v
// OK:        IMAGE_REL_AMD64_ADDR32NB v

  .text
.ifndef OK
  movq v@GOTPCREL(%rip), %rax
  movq v@GOTOFF(%rip), %rax
  movl $v@TPOFF, %eax
.else
  callq f@PLT
  retq
  .data
  .long v@SECREL32
  .long v@IMGREL
.endif
