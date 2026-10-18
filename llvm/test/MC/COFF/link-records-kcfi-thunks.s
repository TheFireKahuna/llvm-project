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

// "LLRC", version 1, no capabilities, then a group of kind 8 and 36 bytes:
// symbol 8, dispatch, type 0x12345678, marker 0x071c5a06, offset 0, symbol 17;
// symbol 11, vfn_check, type 0x89abcdef, the marker, offset 4, symbol 20;
// then symbol 14, local_check, type 0x10, the marker, offset 0, symbol 21.
// CHECK:      Contents of section .llvm_link_records:
// CHECK-NEXT: 0000 4c4c5243 01000824 08007856 3412065a
// CHECK-NEXT: 0010 1c070011 0b02efcd ab89065a 1c070414
// CHECK-NEXT: 0020 0e041000 0000065a 1c070015

// ASM: .linkkcfithunk __llvm_kcfi_dispatch_12345678, dispatch, 0x12345678, 0x071c5a06, 0, __llvm_kcfi_mismatch_12345678
// ASM: .linkkcfithunk __llvm_kcfi_vfn_check_89abcdef, vfn_check, 0x89abcdef, 0x071c5a06, 4, __llvm_kcfi_check_mismatch_89abcdef
// ASM: .linkkcfithunk __llvm_kcfi_local_check_00000010, local_check, 0x00000010, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_00000010

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

  .section .text,"xr",discard,__llvm_kcfi_local_check_00000010
  .globl __llvm_kcfi_local_check_00000010
__llvm_kcfi_local_check_00000010:
  .linkkcfithunk __llvm_kcfi_local_check_00000010, local_check, 0x10, 0x071c5a06, 0, __llvm_kcfi_check_mismatch_00000010
  jne __llvm_kcfi_check_mismatch_00000010
  retq

.ifdef ERR
// ERR: error: expected 'dispatch', 'check', 'vfn_check', 'local_dispatch', 'local_check', 'member_miss' or 'member_outside_miss'
  .linkkcfithunk __llvm_kcfi_dispatch_12345678, member, 0, 0, 0, __llvm_kcfi_mismatch_12345678
// ERR: error: type must be a 32-bit unsigned value
  .linkkcfithunk __llvm_kcfi_dispatch_12345678, dispatch, 0x100000000, 0, 0, __llvm_kcfi_mismatch_12345678
.endif
