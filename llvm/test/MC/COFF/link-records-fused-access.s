// .linkfusedaccess says that the next instruction reads or writes memory
// through the register a load of a pointer to data filled, as the base of its
// memory operand, and .linkfusedadd that the next instruction adds a constant
// to that register. Each becomes a site of its own with no relocation, whose
// facts the streamer takes from the instruction it sees: an access only with a
// legacy, VEX or EVEX encoding, no index and the register read nowhere else,
// with the prefix that holds its base register's high bit; an add only
// of an immediate, or an lea of the register plus one. A load that only such
// accesses read is described with kind 6, and each section where they may
// change size is covered.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// A group of kind 2 and 66 bytes: .text with 14 sites.
// - 3b 02 0604: the load at 0x3, which only accesses read (kind 6), before
//   movl, 2 bytes.
// - 48 03 03f105: an access at 0x7 (kind 3) reading through site 0's
//   register: ModRM at 1, no prefix holding the base's high bit (15), 2
//   bytes, no displacement or SIB.
// - 28 03 038228: at 0x9, REX at 0, ModRM at 2, 4 bytes, an 8-bit
//   displacement (1 << 12).
// - 48 03 03f14f: at 0xd, a 32-bit displacement (2 << 12) and an immediate
//   after it, 7 bytes.
// - a301: the load at 0x17, which other instructions read too.
// - 48 05 0382aa8102: at 0x1b, through site 4's register, r12: REX at 0, a
//   SIB byte, an 8-bit displacement, 5 bytes.
// The accesses at 0x20 and 0x24 are not described.
// - d801 05 03848c8302: at 0x28, with three-byte VEX (prefix kind 1 << 15)
//   at 0, ModRM at 4 and a SIB byte, 6 bytes.
// - 9301: the load at 0x31; 48 05 04e001 0520: an add at 0x35 (kind 4) of
//   site 7's register, rax (0), with the constant 16 (kind 5, zigzag 0x20).
// - 73: the load at 0x3c; 48 06 04a102 05ff0f: a sub at 0x40, adding -1024
//   to rcx (1).
// - a301: the load at 0x4a; 48 05 04e902 0510: an lea at 0x4e writing r9
//   (9), adding 8.
// - 73: the load at 0x55, whose add is not described.
// Then .text is covered, with no value depending on what may change size.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01070242 000e3b02 06044803
// CHECK-NEXT: 0010 03f10528 03038228 480303f1 4fa30148
// CHECK-NEXT: 0020 050382aa 8102d801 0503848c 83029301
// CHECK-NEXT: 0030 480504e0 01052073 480604a1 0205ff0f
// CHECK-NEXT: 0040 a3014805 04e90205 10731e03 000000
// CHECK-EMPTY:

// ASM:      .linkfusedaccess .Lr0, %rax
// ASM-NEXT: movl (%rax), %ecx
// ASM:      .linkfusedadd .Lr4, %r8
// ASM-NEXT: leaq 8(%r8), %r9

  .text
  .globl f
f:
.Lr0:
  .linkfusedread
  movq __imp_v(%rip), %rax
  .linkfusedaccess .Lr0, %rax
  movl (%rax), %ecx            // 0x7
  .linkfusedaccess .Lr0, %rax
  movq 8(%rax), %rcx           // 0x9
  .linkfusedaccess .Lr0, %rax
  cmpl $5, 1024(%rax)          // 0xd
.Lr1:
  movq __imp_v(%rip), %r12
  .linkfusedaccess .Lr1, %r12
  movl 4(%r12), %edx           // 0x1b
  .linkfusedaccess .Lr1, %r12
  movl (%r12,%rdx), %edx       // 0x20: an index
  .linkfusedaccess .Lr1, %r12
  addq %r12, (%r12)            // 0x24: reads r12 elsewhere too
  .linkfusedaccess .Lr1, %r12
  vmovups (%r12), %xmm0        // 0x28
.Lr2:
  movq __imp_v(%rip), %rax
  .linkfusedadd .Lr2, %rax
  addq $16, %rax               // 0x35
.Lr3:
  movq __imp_v(%rip), %rcx
  .linkfusedadd .Lr3, %rcx
  subq $1024, %rcx             // 0x40
.Lr4:
  movq __imp_v(%rip), %r8
  .linkfusedadd .Lr4, %r8
  leaq 8(%r8), %r9             // 0x4e
.Lr5:
  movq __imp_v(%rip), %rax
  .linkfusedadd .Lr5, %rax
  addq %rcx, %rax              // 0x59: adds no constant
  retq
