// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium %s -o /dev/null \
// RUN:   2>&1 | FileCheck %s

  .data
a:
  .quad 0

// CHECK: [[#@LINE+1]]:13: error: log2 of the modulus must be in the range [0, 63]
.linkpin a, 64, 0
// CHECK: [[#@LINE+1]]:16: error: residue must be less than the modulus
.linkpin a, 6, 64
// CHECK: [[#@LINE+1]]:19: error: expected 'required'
.linkpin a, 6, 8, optional
// CHECK: [[#@LINE+1]]:12: error: expected comma
.linkpin a 6, 8
