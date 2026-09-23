; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llc -filetype=obj %t/consumer.ll -o %t/consumer.obj
; RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj -out:%t/native.dll
; RUN: llvm-readobj --coff-imports %t/native.dll | FileCheck %s --check-prefix=CONSUMER
; RUN: %python %t/check.py %t
; RUN: llvm-as %t/consumer.ll -o %t/consumer.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots -lldsavetemps %t/consumer.bc -out:%t/full.dll
; RUN: llvm-readobj --coff-imports %t/full.dll | FileCheck %s --check-prefix=CONSUMER
; RUN: FileCheck %s --check-prefix=RESOLUTION < %t/full.dll.resolution.txt
; RUN: %python %t/check.py %t
; RUN: opt -module-summary %t/consumer.ll -o %t/consumer.thin.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.thin.bc -out:%t/thin.dll
; RUN: llvm-readobj --coff-imports %t/thin.dll | FileCheck %s --check-prefix=CONSUMER
; RUN: %python %t/check.py %t
; RUN: lld-link -dll -noentry -auto-import -import-slots %t/consumer.obj -out:%t/published.dll -export:entity,DATA -export:name,DATA
; RUN: llvm-readobj --coff-imports --coff-exports %t/published.dll | FileCheck %s --check-prefix=PUBLISHED
; RUN: llvm-ar crs %t/consumer.lib %t/consumer.bc
; RUN: lld-link -dll -noentry -auto-import -import-slots -include:slot %t/consumer.lib -out:%t/archive.dll
; RUN: llvm-readobj --coff-imports %t/archive.dll | FileCheck %s --check-prefix=CONSUMER
; RUN: %python %t/check.py %t
; RUN: llc -filetype=obj %t/cycle.ll -o %t/cycle.obj
; RUN: env LLD_IN_TEST=1 not lld-link -dll -noentry -auto-import -import-slots %t/cycle.obj -out:%t/cycle.dll 2>&1 | FileCheck %s --check-prefix=CYCLE

; No offered RTTI import library, hand-authored owner, runtime resolver or
; provider subprocess. Independent native/FullLTO/ThinLTO links choose the same
; actual descriptor and name providers, using separate output-local IATs.
; CONSUMER: Name: rtti2-{{[a-f0-9]+}}.dll
; CONSUMER: Symbol: entity (0)
; RESOLUTION: -r={{.*}},entity,px
; RESOLUTION: -r={{.*}},name,px
; RESOLUTION: -r={{.*}},alias,x
; PUBLISHED-NOT: Import {
; PUBLISHED: Name: entity
; PUBLISHED: Name: name
; CYCLE: cyclic canonical provider dependency from cycle to cycle

;--- consumer.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-itanium"
$entity = comdat any
$name = comdat any
@entity = linkonce_odr constant [2 x ptr] [ptr null, ptr @name], comdat, !coff.binding !1
@name = linkonce_odr constant [7 x i8] c"Entity\00", comdat, !coff.binding !2
@alias = weak alias [2 x ptr], ptr @entity
@slot = dllexport constant ptr @alias
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}
!2 = !{i32 5}

;--- cycle.ll
target triple = "x86_64-pc-windows-itanium"
$cycle = comdat any
@cycle = linkonce_odr constant ptr @cycle, comdat, !coff.binding !1
@root = dllexport constant ptr @cycle
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"coff.rtti_abi", i32 2}
!1 = !{i32 3}

;--- check.py
import hashlib
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
for identity in ("entity", "name"):
    digest = hashlib.sha256(b"llvm.itanium.coff.rtti2" +
                            struct.pack("<H", 0x8664) + identity.encode()).hexdigest()
    path = root / ("rtti2-" + digest + ".dll")
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    optional = pe + 24
    assert struct.unpack_from("<I", data, optional + 16)[0] == 0  # no entry
    assert struct.unpack_from("<H", data, optional)[0] == 0x20b
    directories = optional + 112
    assert struct.unpack_from("<II", data, directories + 9 * 8) == (0, 0)  # no TLS
    count = struct.unpack_from("<H", data, pe + 6)[0]
    section_table = optional + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(count):
        characteristics = struct.unpack_from("<I", data, section_table + 40 * i + 36)[0]
        assert characteristics & 0xe0000000 == 0x40000000  # read-only, NX
    assert identity.encode() + b"\0" in data  # full semantic named export
    saved = path.with_suffix(".reference")
    if saved.exists():
        assert data == saved.read_bytes(), "provider depends on compilation mode"
    else:
        saved.write_bytes(data)
