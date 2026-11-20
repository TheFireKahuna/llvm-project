# RUN: llvm-mc -filetype=obj -triple=aarch64-linux-gnu %s -o %t.o
# RUN: llvm-readobj -r %t.o | FileCheck %s
# RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS

## A .reloc type keeps its relocation and leaves its instruction as assembled,
## whatever its number.

# CHECK:      0x0 R_AARCH64_LDST8_ABS_LO12_NC - 0x1234
# CHECK-NEXT: 0x4 R_AARCH64_JUMP26 - 0x1000
# CHECK-NEXT: 0x8 R_AARCH64_JUMP26 target 0x0
# DIS:      39400020 ldrb w0, [x1]
# DIS-NEXT: 14000001 b
# DIS-NEXT: 14000001 b

.reloc ., R_AARCH64_LDST8_ABS_LO12_NC, 0x1234
ldrb w0, [x1]
.reloc ., R_AARCH64_JUMP26, 0x1000
b #4
.reloc ., R_AARCH64_JUMP26, target
b #4
