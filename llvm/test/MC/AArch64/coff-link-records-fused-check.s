// A fact that names a load and the register it fills is checked against the
// instructions it describes, as on x86-64. Writing a W register clears the
// upper half of its X register, so what the load gives it is the whole value.

// RUN: not llvm-mc -triple aarch64-windows-itanium -filetype=obj %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --implicit-check-not=error:

  .text
  .p2align 2
f:
  adrp x8, __imp_d
.La:
  ldr x8, [x8, :lo12:__imp_d]
  mov w8, #1
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads X8, which an instruction other than the load at '.La' last wrote
  .linkfusedaccess .La, x8
  ldr w0, [x8]

  adrp x9, "__imp_v$tls"
.Lb:
  ldr x9, [x9, :lo12:"__imp_v$tls"]
  mov x10, x9
  .linkfusedaccess .Lb, x10
.Lc:
  ldr w11, [x10, #8]
  .linkfusedindex .Lc, x11
  ldr w0, [x12, x11]
  .linkfusedindex .Lc, x11
// CHECK: [[#@LINE+1]]:3: error: fused instruction does not read X11
  ldr w0, [x12, x13]

  adrp x14, __imp_e
.Ld:
  ldr x14, [x14, :lo12:__imp_e]
// CHECK: [[#@LINE+1]]:3: error: fused instruction reads X15, which the load at '.Ld' does not fill
  .linkfusedadd .Ld, x15
  add x3, x15, #16
  ret
