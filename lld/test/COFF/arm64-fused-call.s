// REQUIRES: aarch64
// RUN: echo '.globl f; f: ret' | llvm-mc -triple aarch64-windows-msvc \
// RUN:   -filetype=obj -o %t.dll.obj
// RUN: lld-link -dll -noentry -export:f %t.dll.obj -out:%t.dll -implib:%t.lib
// RUN: llvm-mc -triple aarch64-windows-msvc -filetype=obj \
// RUN:   %S/Inputs/loadconfig-arm64.s -o %t.lc.obj
// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -o %t.fused.obj
// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s \
// RUN:   -defsym=PLAIN=1 -o %t.plain.obj
// RUN: lld-link -entry:main -subsystem:console -guard:cf %t.fused.obj \
// RUN:   %t.lc.obj %t.lib -out:%t.fused.exe
// RUN: lld-link -entry:main -subsystem:console -guard:cf %t.plain.obj \
// RUN:   %t.lc.obj %t.lib -out:%t.plain.exe
// RUN: llvm-objdump -d %t.fused.exe | FileCheck %s
// RUN: llvm-objdump -d %t.plain.exe | FileCheck %s
// RUN: llvm-readobj --coff-load-config %t.fused.exe | \
// RUN:   FileCheck %s --check-prefix=FUSED
// RUN: llvm-readobj --coff-load-config %t.plain.exe | \
// RUN:   FileCheck %s --check-prefix=PLAIN

// A fused read is applied as the load it is, a fused call leaves its blr and
// a fused jump its br, so the image's code is that of an object without them.
// The import that only fused calls and jumps reach is no indirect call target.

// CHECK:      adrp x8, 0x140002000
// CHECK-NEXT: ldr x8, [x8, #0x[[#%x,]]]
// CHECK-NEXT: blr x8
// CHECK-NEXT: adrp x16, 0x140002000
// CHECK-NEXT: ldr x16, [x16, #0x[[#%x,]]]
// CHECK-NEXT: br x16

// FUSED: GuardAddressTakenIatEntryCount: 0
// PLAIN: GuardAddressTakenIatEntryCount: 1

  .text
  .globl main
main:
  adrp x8, __imp_f
.ifdef PLAIN
  ldr x8, [x8, :lo12:__imp_f]
.else
  ldr x8, [x8, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_f
.endif
  blr x8
  adrp x16, __imp_f
.ifdef PLAIN
  ldr x16, [x16, :lo12:__imp_f]
.else
  ldr x16, [x16, :fused_lo12:__imp_f]
  .reloc ., IMAGE_REL_LLVM_FUSED_JUMP, __imp_f
.endif
  br x16
