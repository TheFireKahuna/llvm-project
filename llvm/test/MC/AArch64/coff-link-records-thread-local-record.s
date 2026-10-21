// On ARM64, .linkfusedindex describes an instruction that adds the offset a
// fused access of a thread-local variable's record loaded to another
// register: a load, store or prefetch at their sum, or an add of them. The
// fact holds the load's site, and bit 14 where the register is the first of
// the two, the base or the add's first source; the linker decodes the rest.
// The offset's load, which only such instructions read, is described with
// kind 9.

// RUN: llvm-mc -triple aarch64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s

// A group of kind 2 and 0x18 bytes: .text with 4 sites.
// - 4b 02 0600: the record's load at 0x4, which only accesses read.
// - 48 04 0300 0900: an access at 0x8 of site 0, a load only indexes read.
// - 48 04 08808040: an index at 0xc of site 1, x11 the offset register.
// - 48 04 08808041: at 0x10, x11 the add's first source (1 << 14).
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 010a0218 00044b02 06004804
// CHECK-NEXT: 0010 03000900 48040880 80404804 08808041

  .text
g:
  adrp x8, "__imp_v$tls"
.La:
  .linkfusedread
  ldr x8, [x8, :lo12:"__imp_v$tls"]
  .linkfusedaccess .La, x8
.Lb:
  .linkfusedread
  ldr w11, [x8, #8]
  .linkfusedindex .Lb, x11
  ldr w0, [x9, x11]
  .linkfusedindex .Lb, x11
  add x2, x11, x9
  ret
