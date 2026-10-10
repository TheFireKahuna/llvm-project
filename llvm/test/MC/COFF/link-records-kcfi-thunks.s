// .linkkcfithunk describes a KCFI thunk for a linker that replaces it with a
// form of its own: a group of the non-critical kind 8 giving, for each thunk,
// its symbol's index, its kind, the type it checks, the marker of the prefixes
// it compares, the bytes of any patchable prefix, and the index of its
// mismatch routine's symbol.

// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-pc-windows-msvc %s | \
// RUN:   FileCheck --check-prefix=ASM %s
// RUN: not llvm-mc -triple x86_64-pc-windows-msvc --defsym ERR=1 %s \
// RUN:   -o /dev/null 2>&1 | FileCheck --check-prefix=ERR %s

// "LLRC", version 1, no capabilities, then a group of kind 8 and 24 bytes:
// symbol 8, dispatch, type 0x12345678, marker 0x071c5a06, offset 0, symbol 14;
// then symbol 11, vfn_check, type 0x89abcdef, the marker, offset 4, symbol 17.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01000818 08007856 3412065a
// CHECK-NEXT: 0010 1c07000e 0b02efcd ab89065a 1c070411

// ASM: .linkkcfithunk __llvm_kcfi_dispatch_12345678, dispatch, 0x12345678, 0x071c5a06, 0, __llvm_kcfi_mismatch_12345678
// ASM: .linkkcfithunk __llvm_kcfi_vfn_check_89abcdef, vfn_check, 0x89abcdef, 0x071c5a06, 4, __llvm_kcfi_check_mismatch_89abcdef

  .weak __llvm_kcfi_mismatch_12345678
__llvm_kcfi_mismatch_12345678 = __llvm_kcfi_trap

  .section .text,"xr",discard,__llvm_kcfi_dispatch_12345678
  .globl __llvm_kcfi_dispatch_12345678
__llvm_kcfi_dispatch_12345678:
  .linkkcfithunk __llvm_kcfi_dispatch_12345678, dispatch, 0x12345678, 0x071c5a06, 0, __llvm_kcfi_mismatch_12345678
  jne __llvm_kcfi_mismatch_12345678
  jmpq *%rax

  .section .text,"xr",discard,__llvm_kcfi_vfn_check_89abcdef
  .globl __llvm_kcfi_vfn_check_89abcdef
__llvm_kcfi_vfn_check_89abcdef:
  .linkkcfithunk __llvm_kcfi_vfn_check_89abcdef, vfn_check, 0x89abcdef, 0x071c5a06, 4, __llvm_kcfi_check_mismatch_89abcdef
  jne __llvm_kcfi_check_mismatch_89abcdef
  retq

.ifdef ERR
// ERR: error: expected 'dispatch', 'check' or 'vfn_check'
  .linkkcfithunk __llvm_kcfi_dispatch_12345678, member, 0, 0, 0, __llvm_kcfi_mismatch_12345678
// ERR: error: type must be a 32-bit unsigned value
  .linkkcfithunk __llvm_kcfi_dispatch_12345678, dispatch, 0x100000000, 0, 0, __llvm_kcfi_mismatch_12345678
.endif
