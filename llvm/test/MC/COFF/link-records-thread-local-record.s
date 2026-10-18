// An access of a thread-local variable's record is also a load: .linkfusedread
// after its .linkfusedaccess says that only the instructions naming it read
// its register. Those that load the TLS index through the address it loads
// are fused accesses (kind 3) naming its site, and it is described with kind 6;
// those whose memory operand adds the offset it loads, with scale 1, as base
// or index, are fused indexes (kind 8), and it is described with kind 9 where
// they are all that read it. An index with a legacy or three-byte VEX encoding
// names the prefix holding both registers' high bits; EVEX is not described.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s

// A group of kind 2 and 0x2e bytes: .text with 7 sites.
// - 3b 02 0600: the record's load at 0x3, which only accesses read (kind 6).
// - 48 05 038206 0600: an access at 0x7 (kind 3) of site 0, REX at 0, ModRM at
//   2, 3 bytes; a load only accesses read (kind 6).
// - 38 04 03f18540: an access at 0xa of site 1, the access just described.
// - f801 03 038228: an access at 0x19 of site 0 with an 8-bit displacement, 4
//   bytes, a load that an instruction not described reads too.
// - 48 05 0882aac001: an index at 0x1d (kind 8) of site 3, REX at 0, the
//   register the SIB byte's index, an 8-bit displacement, 5 bytes.
// - 58 05 08848cc301: at 0x22, three-byte VEX (1 << 15) at 0, the register
//   the SIB byte's base (1 << 14), ModRM at 4, 6 bytes.
// - 68 05 088288c101: at 0x28, an lea whose other register is r12, REX at 0,
//   the register the base, 4 bytes.
// The EVEX access at 0x2c is not described.
// Then .text is covered.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 0107022e 00073b02 06004805
// CHECK-NEXT: 0010 03820606 00380403 f18540f8 01030382
// CHECK-NEXT: 0020 28480508 82aac001 58050884 8cc30168
// CHECK-NEXT: 0030 05088288 c1011e03 000000
// CHECK-EMPTY:

// ASM:      .linkfusedindex .Lr2, %r9
// ASM-NEXT: movl 4(%rcx,%r9), %eax

  .text
  .globl f
f:
.Lr0:
  .linkfusedread
  movq __imp_v$tls(%rip), %rax
  .linkfusedaccess .Lr0, %rax
.Lr1:
  .linkfusedread
  movq (%rax), %rcx              // 0x7
  .linkfusedaccess .Lr1, %rcx
  movl (%rcx), %ecx              // 0xa
  movq %gs:88, %rdx
  movq (%rdx,%rcx,8), %rcx
  .linkfusedaccess .Lr0, %rax
.Lr2:
  .linkfusedread
  movl 8(%rax), %r9d             // 0x19
  .linkfusedindex .Lr2, %r9
  movl 4(%rcx,%r9), %eax         // 0x1d
  .linkfusedindex .Lr2, %r9
  vmovss (%r9,%rcx), %xmm0       // 0x22
  .linkfusedindex .Lr2, %r9
  leaq (%r9,%r12), %rdx          // 0x28
  .linkfusedindex .Lr2, %r9
  vmovss (%rcx,%r9), %xmm16      // 0x2c
  retq
