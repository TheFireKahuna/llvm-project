// A .linkvtable directive tells the linker where the primary address point of
// a vtable is, with its class and the class's primary base. Vtables travel in
// a LinkRecordVTables group (kind 9, critical) of the link-only records, keyed
// by symbol index. A symbol the symbol table leaves out is named through its
// section's symbol, with the offset moved by its offset in the section.

// RUN: llvm-mc -triple x86_64-unknown-windows-itanium %s | \
// RUN:   FileCheck --check-prefix=ASM %s
// RUN: llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -s -j .llvm_link_records %t.o | \
// RUN:   FileCheck --check-prefix=OBJ %s
// RUN: llvm-objdump -t %t.o | FileCheck --check-prefix=SYM %s
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck --check-prefix=ERR %s
// RUN: not llvm-mc -triple x86_64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym UNDEF=1 %s -o /dev/null 2>&1 | \
// RUN:   FileCheck --check-prefix=UNDEF %s

// ASM: .linkvtable _ZTV1B, 16, "1B", ""
// ASM: .linkvtable _ZTV1D, 32, "1D", "1B"
// ASM: .linkvtable .Linner, 16, "N12_GLOBAL__N_11LE.x", "1D"

// SYM: [ 8](sec  4)(fl 0x00)(ty   0)(scl   2) (nx 0) 0x00000000 _ZTV1B
// SYM: [11](sec  5)(fl 0x00)(ty   0)(scl   2) (nx 0) 0x00000000 _ZTV1D
// SYM: [12](sec  6)(fl 0x00)(ty   0)(scl   3) (nx 1) 0x00000000 .rdata$inner

// "LLRC", version 1, the x86-64 capabilities (7), then a group of kind 9 and
// 40 bytes: _ZTV1B (8) at 16, class "1B", no base; _ZTV1D (11) at 32, class
// "1D", base "1B"; .Linner, 8 bytes into its section, as the section's symbol
// (12) at 8 + 16 = 24.
// OBJ:      Contents of section .llvm_link_records:
// OBJ-NEXT: 0000 4c4c5243 010f0928 08100231 42000b20
// OBJ-NEXT: 0010 02314402 31420c18 144e3132 5f474c4f
// OBJ-NEXT: 0020 42414c5f 5f4e5f31 314c452e 78023144

  .section .rdata,"dr",discard,_ZTV1B
  .globl _ZTV1B
  .p2align 3
_ZTV1B:
  .quad 0, 0, 0
  .linkvtable _ZTV1B, 16, "1B", ""

  .section .rdata,"dr",discard,_ZTV1D
  .globl _ZTV1D
  .p2align 3
_ZTV1D:
  .quad 0, 0, 0, 0, 0
  .linkvtable _ZTV1D, 32, "1D", "1B"

  .section .rdata$inner,"dr"
  .p2align 3
  .quad 0
.Linner:
  .quad 0, 0, 0
  .linkvtable .Linner, 16, "N12_GLOBAL__N_11LE.x", "1D"

.ifdef ERR
// ERR: error: class name must not be empty
  .linkvtable _ZTV1B, 16, "", ""
// ERR: error: offset must not be negative
  .linkvtable _ZTV1B, -8, "1B", ""
.endif

.ifdef UNDEF
// UNDEF: error: vtable 'undefined' is not defined
  .linkvtable undefined, 16, "1U", ""
.endif
