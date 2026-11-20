// An object for Windows Itanium or NT-POSIX is marked by @comp.id, carries
// link records with a Symbols group, and names in .drectve, its first
// section, the symbol that a linker which reads the records defines.

// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-windows-itanium %s -o %t.o
// RUN: llvm-readobj -S --sd --symbols --coff-link-records %t.o | FileCheck %s
// RUN: %if aarch64-registered-target %{ llvm-mc -filetype=obj -triple aarch64-unknown-windows-itanium %s \
// RUN:   -o %t.a64.o %}
// RUN: %if aarch64-registered-target %{ llvm-readobj -S --sd --symbols --coff-link-records %t.a64.o \
// RUN:   | FileCheck %s %}
// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-windows-ntposix %s \
// RUN:   -o %t.nt.o
// RUN: llvm-readobj -S --sd --symbols --coff-link-records %t.nt.o \
// RUN:   | FileCheck %s

// CHECK:      Sections [
// CHECK-NEXT:   Section {
// CHECK-NEXT:     Number: 1
// CHECK-NEXT:     Name: .drectve
// CHECK:          SectionData (
// CHECK-NEXT:       0000: 202F6578 706F7274 3A66202F 494E434C  | /export:f /INCL|
// CHECK-NEXT:       0010: 5544453A 5F5F6C6C 766D5F6C 696E6B5F  |UDE:__llvm_link_|
// CHECK-NEXT:       0020: 7265636F 7264735F 7631               |records_v1|
// CHECK-NEXT:     )
// CHECK:          Name: .llvm_link_records
// CHECK:          Characteristics [ (0x100200)
// CHECK-NEXT:       IMAGE_SCN_ALIGN_1BYTES (0x100000)
// CHECK-NEXT:       IMAGE_SCN_LNK_INFO (0x200)
// CHECK-NEXT:     ]
// CHECK-NEXT:     SectionData (
// CHECK-NEXT:       0000: 4C4C5243 01000109 0F020305 11080103  |LLRC............|
// CHECK-NEXT:       0010: 00                                   |.|
// CHECK-NEXT:     )
// CHECK:      Symbols [
// CHECK-NEXT:   Symbol {
// CHECK-NEXT:     Name: @comp.id
// CHECK-NEXT:     Value: 1280049152
// CHECK-NEXT:     Section: IMAGE_SYM_ABSOLUTE (-1)
// CHECK:      LinkRecords {
// CHECK-NEXT:   Group {
// CHECK-NEXT:     Kind: Symbols (0x1)
// CHECK-NEXT:     Fact {
// CHECK-NEXT:       Symbol: f (15)
// CHECK-NEXT:       Tag: 0x2
// CHECK-NEXT:       Operands (
// CHECK-NEXT:         0000: 051108 
// CHECK-NEXT:       )
// CHECK-NEXT:     }
// CHECK-NEXT:     Fact {
// CHECK-NEXT:       Symbol: .text (1)
// CHECK-NEXT:       Tag: 0x3
// CHECK-NEXT:       Operands (
// CHECK-NEXT:       )
// CHECK-NEXT:     }
// CHECK-NEXT:   }
// CHECK-NEXT:   Flags: 0x0
// CHECK-NEXT: }

// The mark is the writer's: an assembly file that defines @comp.id has one
// such symbol, with the writer's value.
// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-windows-itanium \
// RUN:   -defsym=MARK=1 %s -o %t.mark.o
// RUN: llvm-readobj --symbols %t.mark.o | FileCheck %s --check-prefix=ONCE
// ONCE:      Name: @comp.id
// ONCE-NEXT: Value: 1280049152
// ONCE-NOT:  Name: @comp.id

// Objects for the MSVC and MinGW environments, and 32-bit objects for Windows
// Itanium, carry none of the three.
// DEFINE: %{other} = llvm-readobj -S --sd --symbols --coff-link-records %t.o \
// DEFINE:   | FileCheck %s --check-prefix=OTHER --implicit-check-not=comp.id \
// DEFINE:     --implicit-check-not=llvm_link_records --implicit-check-not=LinkRecords
// RUN: llvm-mc -filetype=obj -triple x86_64-pc-windows-msvc %s -o %t.o && %{other}
// RUN: llvm-mc -filetype=obj -triple x86_64-w64-windows-gnu %s -o %t.o && %{other}
// RUN: llvm-mc -filetype=obj -triple i686-unknown-windows-itanium %s -o %t.o && %{other}

// OTHER:      Number: 1
// OTHER-NEXT: Name: .text
// OTHER:      Name: .drectve
// OTHER:      0000: 202F6578 706F7274 3A66  | /export:f|

// .link_fact adds a fact to the Symbols group: the index of its symbol, the
// tag, the size of the operands and the operands, where an operand that is a
// symbol is the symbol's index and any other is a number. A temporary symbol
// is named by its section's symbol.
// RUN: not llvm-mc -filetype=obj -triple x86_64-unknown-windows-itanium \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
// ERR: [[#@LINE+11]]:22: error: operand of a link fact is neither a symbol nor absolute

  .text
  .globl f
f:
.Ltemporary:
  ret
x = 7
  .link_fact f, 2, 5, g, x + 1
  .link_fact .Ltemporary, 3
.ifdef ERR
  .link_fact f, 0, f + 1
.endif
  .section .drectve,"yn"
  .ascii " /export:f"
.ifdef MARK
  .def @comp.id; .scl 3; .type 0; .endef
  .globl @comp.id
@comp.id = 17
.endif
