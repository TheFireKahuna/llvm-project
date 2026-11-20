// RUN: llvm-mc -triple=aarch64-unknown-windows-itanium %s | FileCheck %s --check-prefix=ASM
// RUN: llvm-mc -triple=aarch64-unknown-windows-itanium -filetype=obj %s -o %t.o
// RUN: llvm-objdump -dr %t.o | FileCheck %s
// RUN: llvm-mc -triple=aarch64-unknown-windows-ntposix -filetype=obj %s -o %t.o
// RUN: llvm-objdump -dr %t.o | FileCheck %s
// RUN: not llvm-mc -triple=aarch64-windows-msvc -filetype=obj %s -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=MSVC
// RUN: not llvm-mc -triple=aarch64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym ERR=1 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
// RUN: not llvm-mc -triple=aarch64-unknown-windows-itanium -filetype=obj \
// RUN:   --defsym ERR=2 %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPC

// Windows Itanium and NT-POSIX objects have relocations for an absolute
// symbol's value in MOVZ and MOVK, whose field holds a 16-bit addend, and as
// the bitmask immediate of AND, ANDS and TST, whose field holds none.

.ifndef ERR
// ASM:      movz x0, #:abs_g0:ext
// CHECK:      d2800000 mov x0, #0x0
// CHECK-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0 ext
	movz x0, #:abs_g0:ext
// ASM:      movz x1, #:abs_g1:ext+4
// CHECK:      d2a00081 mov x1, #0x40000
// CHECK-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G1 ext
	movz x1, #:abs_g1:ext+4
// CHECK:      f2800081 movk x1, #0x4
// CHECK-NEXT: IMAGE_REL_LLVM_MOVW_UABS_G0_NC ext
	movk x1, #:abs_g0_nc:ext+4
// ASM:      and x2, x3, #:abs_logimm:mask
// CHECK:      92000062 and x2, x3,
// CHECK-NEXT: IMAGE_REL_LLVM_LOGIMM mask
	and x2, x3, #:abs_logimm:mask
// CHECK:      72000062 ands w2, w3,
// CHECK-NEXT: IMAGE_REL_LLVM_LOGIMM mask
	ands w2, w3, #:abs_logimm:mask
// ASM:      tst x4, #:abs_logimm:mask
// CHECK:      f200009f tst x4,
// CHECK-NEXT: IMAGE_REL_LLVM_LOGIMM mask
	tst x4, #:abs_logimm:mask

// A value the assembler knows is encoded for the register size.
// CHECK:      92781cc5 and x5, x6, #0xff00
// CHECK-NEXT: 72181cbf tst w5, #0xff00
	and x5, x6, #:abs_logimm:known
	tst w5, #:abs_logimm:known
known = 0xff00

// MSVC: [[#@LINE-27]]:{{.*}} error: relocation specifier :abs_g0: unsupported on COFF targets
// MSVC: [[#@LINE-17]]:{{.*}} error: relocation specifier :abs_logimm: unsupported on COFF targets

.elseif ERR == 1
// ERR:      [[#@LINE+1]]:{{.*}} error: fixup value out of range
	movz x1, #:abs_g1:ext+0x10000
// ERR:      [[#@LINE+1]]:{{.*}} error: relocation specifier :abs_g2: unsupported on COFF targets
	movz x1, #:abs_g2:ext
// ERR:      [[#@LINE+1]]:{{.*}} error: fixup value out of range
	and x2, x3, #:abs_logimm:mask+1
// ERR:      [[#@LINE+1]]:{{.*}} error: fixup value out of range
	and x5, x6, #:abs_logimm:five
// ERR:      [[#@LINE+1]]:{{.*}} error: fixup value out of range
	and w5, w6, #:abs_logimm:wide
five = 5
wide = 0xff00000000

.else
// OPC: [[#@LINE+1]]:{{.*}} error: a symbolic bitmask immediate needs AND, ANDS or TST
	orr x2, x3, #:abs_logimm:mask
// OPC: [[#@LINE+1]]:{{.*}} error: expected compatible register or logical immediate
	bic x2, x3, #:abs_logimm:mask
.endif
