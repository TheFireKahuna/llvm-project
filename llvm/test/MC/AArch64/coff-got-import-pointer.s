// On Windows Itanium and NT-POSIX a symbol's import pointer, __imp_X, is its
// GOT entry, so :got:X and :got_lo12:X, which position-independent ELF
// assembly uses to reach external data, reach the page and offset of __imp_X.
// Other COFF targets have no relocation for them.

// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -dr %t.o | FileCheck %s
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj %s -o %t.n.o
// RUN: llvm-objdump -dr %t.n.o | FileCheck %s
// RUN: not llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ERR

// CHECK:      adrp x8, 0x0
// CHECK-NEXT:   IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_v
// CHECK-NEXT: ldr x8, [x8]
// CHECK-NEXT:   IMAGE_REL_ARM64_PAGEOFFSET_12L __imp_v

// ERR: error: relocation specifier :got: unsupported on COFF targets
// ERR: error: relocation specifier :got_lo12: unsupported on COFF targets

  adrp x8, :got:v
  ldr x8, [x8, :got_lo12:v]
  ret
