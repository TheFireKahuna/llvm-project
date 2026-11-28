// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj -r --coff-link-records %t.o | FileCheck %s
// RUN: llvm-mc -triple aarch64-pc-windows-ntposix %s | \
// RUN:   llvm-mc -triple aarch64-pc-windows-ntposix -filetype=obj -o %t.s.o
// RUN: llvm-readobj -r --coff-link-records %t.s.o | FileCheck %s
// RUN: not llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj \
// RUN:   -defsym=ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR --implicit-check-not=error:
// RUN: not llvm-mc -triple aarch64-pc-windows-msvc -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=MSVC

// On Windows Itanium and NT-POSIX :fused_lo12: makes the load of a pointer a
// fused read, and a .reloc of IMAGE_REL_LLVM_FUSED_CALL marks a blr of the
// register as a fused call and one of IMAGE_REL_LLVM_FUSED_JUMP a br of it as
// a fused jump. The checks the two machines share are pinned in
// MC/COFF/reloc-types-fused.s; these are AArch64's forms and registers.

// CHECK:      Section ({{[0-9]+}}) .text {
// CHECK-NEXT:   0x0 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_f
// CHECK-NEXT:   0x4 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0xC IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT:   0x14 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_f
// CHECK-NEXT:   0x18 IMAGE_REL_LLVM_FUSED_READ __imp_f
// CHECK-NEXT:   0x1C IMAGE_REL_LLVM_FUSED_JUMP_TAIL __imp_f
// CHECK-NEXT:   0x20 IMAGE_REL_LLVM_FUSED_TEST __imp_f
// CHECK-NEXT:   0x24 IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_f
// CHECK-NEXT:   0x28 IMAGE_REL_ARM64_PAGEOFFSET_12L __imp_f
// CHECK-NEXT:   0x30 IMAGE_REL_LLVM_FUSED_CALL __imp_f
// CHECK-NEXT: }
// CHECK:      Flags: 0x8

// MSVC-DAG: [[#@LINE+5]]:{{[0-9]+}}: error: relocation specifier :fused_lo12: unsupported on COFF targets
// MSVC-DAG: [[#@LINE+6]]:{{[0-9]+}}: error: unknown relocation name

// A write of another register leaves the load the last writer.
  adrp x19, __imp_f
  ldr x19, [x19, :fused_lo12:__imp_f]
  mov w8, #1
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x19
  ret
  adrp x16, __imp_f
  ldr x16, [x16, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  br x16

// A .reloc of IMAGE_REL_LLVM_FUSED_TEST marks the test of a register a load
// of the pointer set.
  .reloc ., IMAGE_REL_LLVM_FUSED_TEST, __imp_f
  cbz x16, .

// A load of the pointer that another instruction reads too is no fused read,
// and is still what a fused call through its register goes by.
  adrp x8, __imp_f
  ldr x8, [x8, :lo12:__imp_f]
  cbz x8, .
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x8

.ifdef ERR
// A fused read is a 64-bit load of the pointer itself, a fused call a blr and
// a fused jump a br, and a write of the register's lower half ends the read.
// ERR-COUNT-3: error: fused read is not a 64-bit ldr of a pointer
  ldr w8, [x8, :fused_lo12:__imp_f]
  str x8, [x9, :fused_lo12:__imp_f]
  ldr x8, [x8, :fused_lo12:__imp_f+8]
// ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused call does not name a pointer at a blr
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  br x8
// ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused jump does not name a pointer at a br
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
  blr x8
  ldr x8, [x8, :fused_lo12:__imp_f]
  mov w8, w9
// ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused call reads X8, which no load of '__imp_f' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x8
  ldr x8, [x8, :lo12:__imp_g]
// ERR: [[#@LINE+1]]:{{[0-9]+}}: error: fused call reads X8, which no load of '__imp_f' last wrote
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
  blr x8
.endif
