; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llc -filetype=obj -data-sections -function-sections %t/input.ll -o %t/input.obj
; RUN: lld-link /dll /noentry /import-slots /out:%t/native.dll %t/input.obj
; RUN: lld-link /dll /noentry /import-slots /export:data,@65535,DATA /out:%t/ordinal.dll %t/input.obj
; RUN: llvm-as %t/input.ll -o %t/input.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t/full.dll %t/input.bc
; RUN: opt -module-summary %t/input.ll -o %t/thin.bc
; RUN: lld-link /dll /noentry /import-slots /out:%t/thin.dll %t/thin.bc
; RUN: %python %t/check.py %t

; Exact data offsets cross the final output boundary without fill code or
; writable initialization. Repeated demands share one provider alias. The
; public base export is preserved independently of the private exact exports.

;--- input.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"
@data = constant [4 x i64] [i64 11, i64 22, i64 33, i64 44], partition "provider"
@slot = dllexport constant ptr getelementptr ([4 x i64], ptr @data, i64 0, i64 1)
@slot_again = dllexport constant ptr getelementptr ([4 x i64], ptr @data, i64 0, i64 1)
@slot_two = dllexport constant ptr getelementptr ([4 x i64], ptr @data, i64 0, i64 2)
define dllexport ptr @get() noinline {
  ret ptr getelementptr ([4 x i64], ptr @data, i64 0, i64 1)
}
define dllexport ptr @local_get() noinline partition "provider" {
  ret ptr getelementptr ([4 x i64], ptr @data, i64 0, i64 1)
}
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}

;--- check.py
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
if sys.platform == 'win32':
    import ctypes
    policy = ctypes.c_uint32(1)
    set_policy = ctypes.windll.kernel32.SetProcessMitigationPolicy
    assert set_policy(2, ctypes.byref(policy), ctypes.sizeof(policy))
    class Region(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('allocation', ctypes.c_void_p),
                    ('allocation_protect', ctypes.c_uint32), ('partition', ctypes.c_uint16),
                    ('size', ctypes.c_size_t), ('state', ctypes.c_uint32),
                    ('protect', ctypes.c_uint32), ('kind', ctypes.c_uint32)]
    query = ctypes.windll.kernel32.VirtualQuery
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(Region), ctypes.c_size_t]
    query.restype = ctypes.c_size_t
for name in ['native', 'full', 'thin', 'ordinal']:
    path = root / (name + '.dll')
    image = path.read_bytes()
    assert b'__part_exact_' not in image, name
    assert b'__llvm_import_fill' not in image
    if sys.platform == 'win32':
        dll = ctypes.CDLL(str(path.resolve()))
        data = (ctypes.c_int64 * 4).in_dll(dll, 'data')
        slot = ctypes.c_void_p.in_dll(dll, 'slot').value
        again = ctypes.c_void_p.in_dll(dll, 'slot_again').value
        two = ctypes.c_void_p.in_dll(dll, 'slot_two').value
        assert slot == again == ctypes.addressof(data) + 8
        assert two == ctypes.addressof(data) + 16
        assert ctypes.c_int64.from_address(slot).value == 22
        assert ctypes.c_int64.from_address(two).value == 33
        dll.get.restype = dll.local_get.restype = ctypes.c_void_p
        assert dll.get() == dll.local_get() == slot
        # Native, FullLTO and ThinLTO all use one address acquisition. The
        # foreign getter reads the existing field, not an extra IAT word.
        code = ctypes.cast(dll.get, ctypes.c_void_p).value
        body = ctypes.string_at(code, 8)
        assert body[:3] == b'\x48\x8b\x05' and body[7] == 0xc3, (name, body)
        displacement = int.from_bytes(body[3:7], 'little', signed=True)
        assert code + 7 + displacement == ctypes.addressof(
            ctypes.c_void_p.in_dll(dll, 'slot')), name
        local = ctypes.string_at(dll.local_get, 8)
        assert local[:3] == b'\x48\x8d\x05' and local[7] == 0xc3, (name, local)
        for address in [ctypes.addressof(data),
                        ctypes.addressof(ctypes.c_void_p.in_dll(dll, 'slot'))]:
            region = Region()
            assert query(address, ctypes.byref(region), ctypes.sizeof(region))
            assert region.protect & 0xff == 2  # PAGE_READONLY, non-executable.
for path in root.glob('part-*.dll'):
    assert b'__part_exact_' not in path.read_bytes()
