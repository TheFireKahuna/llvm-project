# REQUIRES: x86
# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t/input.s -o %t/input.obj
# RUN: lld-link /dll /noentry /import-slots /export:slots,DATA /export:get /out:%t/main.dll %t/input.obj
# RUN: llvm-readobj --coff-imports %t/main.dll | FileCheck %s --check-prefix=IMPORT
# RUN: %python %t/check.py %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym bad=1 %t/input.s -o %t/padding.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:slots,DATA /out:%t/bad.dll %t/padding.obj 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym bad=2 %t/input.s -o %t/neighbor.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:slots,DATA /out:%t/bad.dll %t/neighbor.obj 2>&1 | FileCheck %s --check-prefix=BAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym code_load=1 %t/input.s -o %t/load.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:get /out:%t/bad.dll %t/load.obj 2>&1 | FileCheck %s --check-prefix=LOAD
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc -defsym code_load=2 %t/input.s -o %t/code-padding.obj
# RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /export:get /out:%t/bad.dll %t/code-padding.obj 2>&1 | FileCheck %s --check-prefix=LOAD

# A negative interior-label addend and a raw section coordinate both reuse the
# existing public base export. Other section coordinates use the precise extent
# that contains them. Padding and named-object arithmetic into the neighbor do
# not gain permission to cross the boundary.
# IMPORT: Symbol: data (0)
# BAD: main-image root slots
# LOAD: main-image root get

#--- input.s
.section .rdata$provider,"dr"
.p2align 3
.quad 0
.globl data, middle, neighbor
data:
.quad 11
middle:
.quad 22
neighbor:
.quad 99

.section .rdata,"dr"
.p2align 3
.globl slots
slots:
.ifdef bad
.if bad == 1
.quad .rdata$provider
.else
.quad data+16
.endif
.else
.quad middle-8, .rdata$provider+8, .rdata$provider+16, .rdata$provider+24
.endif

.section .text$get,"xr"
.globl get
get:
.ifdef code_load
.if code_load == 1
movq .rdata$provider+16(%rip), %rax
.else
leaq .rdata$provider(%rip), %rax
.endif
.else
leaq .rdata$provider+16(%rip), %rax
.endif
retq

.section .text$local,"xr"
.globl local_get
local_get:
leaq data+8(%rip), %rax
retq

.section .llvm.extent,"i"
.long 1
.symidx data
.uleb128 16
.symidx neighbor
.uleb128 8

.section .llvm.part,"i"
.long 1
.asciz "provider"
.uleb128 2
.symidx data
.symidx local_get

#--- check.py
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
assert b'__part_exact_' not in (root / 'main.dll').read_bytes()
if sys.platform == 'win32':
    import ctypes
    policy = ctypes.c_uint32(1)
    assert ctypes.windll.kernel32.SetProcessMitigationPolicy(
        2, ctypes.byref(policy), ctypes.sizeof(policy))
    dll = ctypes.CDLL(str((root / 'main.dll').resolve()))
    data = (ctypes.c_int64 * 2).in_dll(dll, 'data')
    slots = (ctypes.c_void_p * 4).in_dll(dll, 'slots')
    assert slots[0] == slots[1] == ctypes.addressof(data)
    assert slots[2] == ctypes.addressof(data) + 8
    assert ctypes.c_int64.from_address(slots[2]).value == 22
    assert ctypes.c_int64.from_address(slots[3]).value == 99
    dll.get.restype = ctypes.c_void_p
    dll.local_get.restype = ctypes.c_void_p
    assert dll.get() == dll.local_get() == slots[2]
    getter = ctypes.cast(dll.get, ctypes.c_void_p).value
    code = ctypes.string_at(getter, 8)
    assert code[:3] == b'\x48\x8b\x05' and code[7] == 0xc3
    # The address load reuses the existing immutable field; it does not need
    # an additional ordinary IAT word or an add instruction.
    slot_address = getter + 7 + struct.unpack_from('<i', code, 3)[0]
    assert slot_address == ctypes.addressof(slots) + 16
    assert ctypes.string_at(dll.local_get, 3) == b'\x48\x8d\x05'
