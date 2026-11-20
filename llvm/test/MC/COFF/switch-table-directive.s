// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-readobj --coff-link-records --symbols %t.o | FileCheck %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck %s --check-prefix=ASM
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym UNDEFINED=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR
// RUN: llvm-mc -triple x86_64-pc-windows-msvc -filetype=obj %s -o %t.m.o
// RUN: llvm-readobj --symbols %t.m.o | \
// RUN:   FileCheck %s --check-prefix=MSVC --implicit-check-not=Label

// .switch_table states a dispatch through a jump table: its branch, its
// table, the label that the entries are relative to, the number of entries,
// their type, the instruction that loads the entry and flags. The object
// names each place by a symbol of its own, which is a label where the place
// has none or a symbol that another object may define.

// ASM: .switch_table branch, table, table, 2, 4, load, 1

// CHECK:      Name: [[BRANCH:\$L.*]]
// CHECK-NEXT: Value: 7
// CHECK:      StorageClass: Label
// CHECK:      Kind: Sections (0x2)
// CHECK-NEXT: SwitchTable {
// CHECK-NEXT:   Branch: [[BRANCH]] ({{[0-9]+}})
// CHECK-NEXT:   Table: table ({{[0-9]+}})
// CHECK-NEXT:   Base: table ({{[0-9]+}})
// CHECK-NEXT:   Count: 2
// CHECK-NEXT:   Type: Int32 (0x4)
// CHECK-NEXT:   Load: load ({{[0-9]+}})
// CHECK-NEXT:   Flags: 1
// CHECK-NEXT: }

// ERR: error: switch table names 'missing', which this object does not define

// MSVC: Name: .text

  .text
  .globl branch
load:
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
branch:
  jmpq *%rax
.Ltarget:
  retq
  .section .rdata,"dr"
table:
  .long .Ltarget-table
  .long .Ltarget-table
  .switch_table branch, table, table, 2, 4, load, 1
.ifdef UNDEFINED
  .switch_table branch, missing, table, 2, 4, load, 1
.endif
