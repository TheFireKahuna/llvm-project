; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llc -filetype=obj -function-sections -data-sections %t/input.ll -o %t/input.obj
; RUN: lld-link /dll /noentry /import-slots /lldrttiprivate:_ZTI1T /lldrttiprivate:_ZTS1T /out:%t/native.dll %t/input.obj
; RUN: llvm-as %t/input.ll -o %t/input.bc
; RUN: lld-link /dll /noentry /import-slots /lldrttiprivate:_ZTI1T /lldrttiprivate:_ZTS1T /out:%t/full.dll %t/input.bc
; RUN: opt -module-summary %t/input.ll -o %t/thin.bc
; RUN: lld-link /dll /noentry /import-slots /lldrttiprivate:_ZTI1T /lldrttiprivate:_ZTS1T /out:%t/thin.dll %t/thin.bc
; RUN: %python %t/check.py %t
; RUN: env LLD_IN_TEST=1 not lld-link /dll /noentry /import-slots /lldrttiprivate:missing /out:%t/bad.dll %t/input.obj 2>&1 | FileCheck %s
; CHECK: /lldrttiprivate:missing does not name a private canonical definition

;--- input.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

@_ZTS1T = linkonce_odr constant [2 x i8] c"T\00", !coff.binding !1
@_ZTI1T = linkonce_odr constant [2 x ptr] [ptr null, ptr @_ZTS1T], !coff.binding !2

define ptr @feature() noinline partition "optional" {
  call void asm sideeffect "", ""()
  ret ptr @_ZTI1T
}

define dllexport ptr @entry() {
  %value = call ptr @feature()
  ret ptr %value
}

define dllexport ptr @local() {
  ret ptr @_ZTI1T
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 5}
!2 = !{i32 3}

;--- check.py
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
if sys.platform == 'win32':
    import ctypes
    policy = ctypes.c_uint32(1)
    set_policy = ctypes.WinDLL('kernel32', use_last_error=True).SetProcessMitigationPolicy
    set_policy.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    assert set_policy(2, ctypes.byref(policy), ctypes.sizeof(policy))
for name in ['native', 'full', 'thin']:
    main = root / (name + '.dll')
    data = main.read_bytes()
    members = set(re.findall(rb'part-[0-9a-f]{64}-[23]\.dll', data))
    assert len(members) == 2, members
    parts = [(root / member.decode()).read_bytes() for member in members]
    assert not any(b'rtti2-' in image for image in [data] + parts)
    # The shared metadata output has local name pointers/base relocations,
    # and neither it nor the optional code imports the main image.
    assert not any((name + '.dll').encode() in image for image in parts)
    if sys.platform == 'win32':
        import ctypes
        dll = ctypes.CDLL(str(main.resolve()))
        dll.entry.restype = ctypes.c_void_p
        dll.local.restype = ctypes.c_void_p
        assert dll.entry() == dll.local()
        descriptor = (ctypes.c_void_p * 2).from_address(dll.local())
        assert ctypes.string_at(descriptor[1]) == b'T'
