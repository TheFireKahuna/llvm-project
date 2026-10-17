// Windows Itanium and NT-POSIX objects describe how each instruction uses a
// relocation that is not a branch and whose target the link may resolve
// elsewhere, so the linker can rewrite it once it finds the target in the
// image. Other COFF targets do not.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-readobj --sections %t.o | FileCheck %s --check-prefix=SEC
// RUN: llvm-mc -triple x86_64-pc-windows-ntposix -filetype=obj %s -o %t.n.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.n.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.m.o
// RUN: llvm-objdump -h %t.m.o | FileCheck %s --check-prefix=MSVC
// RUN: llvm-mc -triple i686-unknown-windows-itanium -filetype=obj %s \
// RUN:   -defsym=X86=1 -o %t.x86.o
// RUN: llvm-objdump -h %t.x86.o | FileCheck %s --check-prefix=MSVC

// MSVC-NOT: .llvm_link_records

// The section is for the linker only.
// SEC:      Name: .llvm_link_records
// SEC:      Characteristics [ (0x100800)
// SEC-NEXT:   IMAGE_SCN_ALIGN_1BYTES (0x100000)
// SEC-NEXT:   IMAGE_SCN_LNK_REMOVE (0x800)
// SEC-NEXT: ]

// "LLRC", version 1, the x86-64 sites and call-only capabilities (3), then one
// group of kind 2 and 20 bytes: .text's symbol (index 0) with 11 sites, then
// .text$c's symbol (index 6) with 2. Each site is ULEB128 (delta << 4 | form),
// with forms 0 other, 1 call, 2 jump, 3 load, 4 address and 5 jump after one
// prefix.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01030214 000b2162 75737374
// CHECK-NEXT: 0010 70709312 e4014006 02a30161
// CHECK-EMPTY:

.ifndef X86
  .text
  .globl f
f:
  callq *__imp_g(%rip)         // 0x2: call
  jmpq *__imp_g(%rip)          // 0x8: jump
  rex64 jmpq *__imp_g(%rip)    // 0xf: jump after one prefix
  movq __imp_g(%rip), %rax     // 0x16: load
  movq __imp_g(%rip), %r9      // 0x1d: load
  leaq g(%rip), %rcx           // 0x24: address
  cmpq %rax, __imp_g(%rip)     // 0x2b: other
  movq __imp_g+8(%rip), %rax   // 0x32: other, since it is not g's pointer
  // The jump is relaxed to 5 bytes past the fill, which moves the sites that
  // follow it.
  jmp .Lpast                   // 0x36
  callq g                      // 0x3b, a branch: no site
  .fill 128, 1, 0x90
.Lpast:
  movq __imp_g(%rip), %rax     // 0xc3: load
  leaq local(%rip), %rax       // a strong definition here: no site
  leaq c(%rip), %rax           // 0xd1: address of a COMDAT symbol
  .long g - .                  // 0xd5: other, data in code
  .quad h                      // not REL32: no site
local:
  ret

  .section .text$c,"xr",discard,c
  .globl c
c:
  leaq c(%rip), %rax           // the same section: no site
  movq __imp_g(%rip), %rax     // 0xa: load
  callq *__imp_g(%rip)         // 0x10: call

  .data
  // A data section has no sites.
  .long g - .
.else
  .text
  call *__imp_g
.endif
