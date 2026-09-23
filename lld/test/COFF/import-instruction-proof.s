# REQUIRES: x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %t/provider.s -o %t/provider.obj
# RUN: lld-link /dll /noentry /export:entity,DATA /out:%t/provider.dll /implib:%t/provider.lib %t/provider.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %t/input.s -o %t/input.obj
# RUN: lld-link /dll /noentry /auto-import /import-slots /export:get /out:%t/good.dll %t/input.obj %t/provider.lib
# RUN: llvm-objdump -d %t/good.dll | FileCheck %s --check-prefix=GOOD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows -defsym kind=1 %t/input.s -o %t/segment.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /auto-import /import-slots /export:get /out:%t/bad.dll %t/segment.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows -defsym kind=2 %t/input.s -o %t/address.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /auto-import /import-slots /export:get /out:%t/bad.dll %t/address.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows -defsym kind=3 %t/input.s -o %t/embedded.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /auto-import /import-slots /export:get /out:%t/bad.dll %t/embedded.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows -defsym kind=4 %t/input.s -o %t/overlap.obj
# RUN: %python %t/overlap.py %t/overlap.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /auto-import /import-slots /export:get /out:%t/bad.dll %t/overlap.obj %t/provider.lib 2>&1 | FileCheck %s --check-prefix=BAD

# A preceding immediate ending in 0x64 is not a segment prefix. Decode the
# instruction stream rather than rejecting a valid LEA based on that byte.
# GOOD: movl $0x64000000, %ecx
# GOOD-NEXT: movq {{.*}}(%rip), %rax
# GOOD-NEXT: retq
# BAD: unredirectable canonical reference to entity

#--- provider.s
.section .rdata,"dr"
.globl entity
entity:
.quad 0

#--- input.s
.section .rdata,"dr",discard,entity
.globl entity
entity:
.quad 0
.text
.globl get
get:
.ifndef kind
.set kind, 0
.endif
.if kind == 0 || kind == 4
movl $0x64000000, %ecx
leaq entity(%rip), %rax
.elseif kind == 1
# LEA ignores FS; converting this to a load through FS changes its meaning.
.byte 0x64
leaq entity(%rip), %rax
.elseif kind == 2
# The address-size prefix changes the effective-address calculation.
.byte 0x67
leaq entity(%rip), %rax
.else
# The apparent LEA bytes are inside the immediate of one ten-byte MOVABS.
.byte 0x48, 0xb8, 0x48, 0x8d, 0x05
.long entity - . - 4
.byte 0
.endif
retq
.if kind == 4
.quad get
.endif
.section .llvm.bind,"yn"
.long 1, 2
.symidx entity
.byte 3

#--- overlap.py
import pathlib
import struct
import sys

path = pathlib.Path(sys.argv[1])
data = bytearray(path.read_bytes())
for index in range(struct.unpack_from('<H', data, 2)[0]):
    section = 20 + 40 * index
    if data[section:section + 8].rstrip(b'\0') != b'.text':
        continue
    assert struct.unpack_from('<H', data, section + 32)[0] == 2
    relocations = struct.unpack_from('<I', data, section + 24)[0]
    assert struct.unpack_from('<H', data, relocations + 8)[0] == 4
    offset = struct.unpack_from('<I', data, relocations)[0]
    # A separate absolute relocation now overlaps the LEA opcode and operand.
    struct.pack_into('<I', data, relocations + 10, offset - 3)
    path.write_bytes(data)
    break
else:
    raise AssertionError('missing code contribution')
