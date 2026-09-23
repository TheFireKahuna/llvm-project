; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llc -filetype=obj -function-sections %t/input.ll -o %t/input.obj
; RUN: lld-link /dll /noentry /import-slots /out:%t/native.dll %t/input.obj
; RUN: llvm-readobj --coff-imports --coff-exports %t/native.dll | FileCheck %s
; RUN: llvm-as %t/input.ll -o %t/input.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t/full.dll %t/input.bc
; RUN: llvm-readobj --coff-imports --coff-exports %t/full.dll | FileCheck %s
; RUN: opt -module-summary %t/input.ll -o %t/thin.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t/thin.dll %t/thin.bc
; RUN: llvm-readobj --coff-imports --coff-exports %t/thin.dll | FileCheck %s
; RUN: llvm-lto2 dump-symtab %t/input.bc | FileCheck %s --check-prefix=SYMTAB
; RUN: llvm-readobj --sections --relocations %t/input.obj | FileCheck %s --check-prefix=OBJECT
; RUN: %python %t/check.py %t
;
; CHECK: Import {
; CHECK-NEXT: Name: part-{{([0-9a-f]{64})}}-2.dll
; CHECK: Symbol: feature (0)
; CHECK: Export {
; CHECK: Name: entry
; SYMTAB: feature
; SYMTAB-NEXT: partition optional
; OBJECT: Name: .llvm.part
; OBJECT: RawDataSize: 18
; OBJECT: RelocationCount: 0
;
;--- input.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define i32 @feature() noinline partition "optional" {
  call void asm sideeffect "", ""()
  ret i32 7
}

define dllexport i32 @entry() {
  %value = call i32 @feature()
  ret i32 %value
}

;--- check.py
import pathlib
import struct
import sys
import re

root = pathlib.Path(sys.argv[1])
names = set()
for name in ['native', 'full', 'thin']:
    names.update(re.findall(rb'part-[0-9a-f]{64}-2\.dll',
                            (root / (name + '.dll')).read_bytes()))
providers = [root / name.decode() for name in names]
assert len(providers) == 3, providers
for path in providers:
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    optional = pe + 24
    assert struct.unpack_from('<I', data, optional + 16)[0] == 0
    characteristics = struct.unpack_from('<H', data, optional + 70)[0]
    assert characteristics & 0x40  # independently ASLR-relocated PE
    assert characteristics & 0x100  # NX compatible
    assert b'feature\0' in data
    assert b'entry\0' not in data

if sys.platform == 'win32':
    import ctypes
    policy = ctypes.c_uint32(1)
    set_policy = ctypes.WinDLL('kernel32', use_last_error=True).SetProcessMitigationPolicy
    set_policy.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    assert set_policy(2, ctypes.byref(policy), ctypes.sizeof(policy))
    for name in ['native', 'full', 'thin']:
        dll = ctypes.CDLL(str((root / (name + '.dll')).resolve()))
        assert dll.entry() == 7
