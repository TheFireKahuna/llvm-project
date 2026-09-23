# REQUIRES: x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %t/provider.s -o %t/provider.obj
# RUN: lld-link /dll /noentry %t/provider.obj /out:%t/provider.dll /implib:%t/provider.lib /export:entity,DATA /export:interior,DATA
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %t/consumer.s -o %t/consumer.obj
# RUN: lld-link /dll /noentry /auto-import /import-slots %t/consumer.obj %t/provider.lib /out:%t/consumer.dll /export:get
# RUN: llvm-readobj --coff-imports %t/consumer.dll | FileCheck %s --check-prefix=IMPORT
# RUN: llvm-objdump -d %t/consumer.dll | FileCheck %s --check-prefix=CODE
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %t/bad.s -o %t/bad.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /auto-import /import-slots %t/bad.obj %t/provider.lib /out:%t/bad.dll 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %t/overlap.s -o %t/overlap.obj
# RUN: %python %t/overlap.py %t/overlap.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /auto-import /import-slots %t/overlap.obj %t/provider.lib /out:%t/overlap.dll 2>&1 | FileCheck %s --check-prefix=OVERLAP

# Both signed-addend spellings normalize to exact published addresses. The
# linker rewrites operands, not symbol identity or instruction coordinates.
# IMPORT: Name: provider.dll
# IMPORT-DAG: Symbol: entity (0)
# IMPORT-DAG: Symbol: interior (0)
# CODE: movq {{.*}}(%rip), %rax
# CODE-NEXT: movq {{.*}}(%rip), %rdx
# CODE-NEXT: retq
# BAD: an exact native binding is required
# OVERLAP: overlapping canonical reference

#--- provider.s
.section .rdata,"dr"
.globl entity, interior
entity:
.quad 0
interior:
.quad 0

#--- consumer.s
.section .rdata,"dr",discard,entity
.globl entity, interior
entity:
.quad 0
interior:
.quad 0
.section .rdata,"dr"
.quad interior-8, entity+8
.text
.globl get
get:
leaq interior-8(%rip), %rax
leaq entity+8(%rip), %rdx
retq
.section .llvm.bind,"yn"
.long 1, 2
.symidx entity
.byte 3
.symidx interior
.byte 3

#--- overlap.s
.section .rdata,"dr",discard,entity
.globl entity, interior
entity:
.quad 0
interior:
.quad 0
.section .rdata,"dr"
slot:
.quad entity+8, interior
.section .llvm.bind,"yn"
.long 1, 2
.symidx entity
.byte 3
.symidx interior
.byte 3

#--- bad.s
.section .rdata,"dr",discard,entity
.globl entity
entity:
.quad 0
unbound:
.quad 0
.section .rdata,"dr"
.quad unbound+1
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
for i in range(struct.unpack_from('<H', data, 2)[0]):
    section = 20 + 40 * i
    if struct.unpack_from('<H', data, section + 32)[0] != 2:
        continue
    relocations = struct.unpack_from('<I', data, section + 24)[0]
    assert struct.unpack_from('<I', data, relocations + 10)[0] == 8
    struct.pack_into('<I', data, relocations + 10, 4)
    path.write_bytes(data)
    break
else:
    raise AssertionError('missing two-relocation contribution')
