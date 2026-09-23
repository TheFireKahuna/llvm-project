; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llc -filetype=obj -function-sections %t/input.ll -o %t/native.obj
; RUN: llvm-as %t/input.ll -o %t/full.bc
; RUN: opt -module-summary %t/input.ll -o %t/thin.bc
; RUN: llvm-mc -triple=x86_64-windows -filetype=obj %t/directories.s -o %t/directories.obj
; RUN: %python %t/check.py %t lld-link llvm-readobj llvm-pdbutil

; Exercise the normal PE, load-config, PDB and auxiliary writers together.
; A failed writer must not publish any part of the new output set.

;--- input.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

define i32 @feature(i32 %n) noinline uwtable partition "optional" !dbg !5 {
  call void @llvm.dbg.value(metadata i32 %n, metadata !9, metadata !DIExpression()), !dbg !10
  call void asm sideeffect "", ""(), !dbg !10
  %v = add i32 %n, 7, !dbg !10
  ret i32 %v, !dbg !10
}

define dllexport i32 @entry() uwtable !dbg !11 {
  %v = call i32 @feature(i32 40), !dbg !14
  ret i32 %v, !dbg !14
}

declare void @llvm.dbg.value(metadata, metadata, metadata)
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "partition.cpp", directory: "/src")
!2 = !{i32 2, !"CodeView", i32 1}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 2, !"Dwarf Version", i32 4}
!5 = distinct !DISubprogram(name: "feature", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !8)
!6 = !DISubroutineType(types: !7)
!7 = !{!15, !15}
!8 = !{!9}
!9 = !DILocalVariable(name: "n", arg: 1, scope: !5, file: !1, line: 1, type: !15)
!10 = !DILocation(line: 2, column: 3, scope: !5)
!11 = distinct !DISubprogram(name: "entry", scope: !1, file: !1, line: 4, type: !12, scopeLine: 4, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!12 = !DISubroutineType(types: !13)
!13 = !{!15}
!14 = !DILocation(line: 5, column: 3, scope: !11)
!15 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)

;--- directories.s
        .globl @feat.00
        .set @feat.00, 0x4800
        .section .gfids$y,"dr"
        .symidx feature
        .section .gljmp$y,"dr"
        .symidx feature
        .section .gehcont$y,"dr"
        .symidx feature

        .section .rdata$load,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 280
        .zero 124
        .quad __guard_fids_table, __guard_fids_count
        .long __guard_flags
        .zero 12
        .quad __guard_iat_table, __guard_iat_count
        .quad __guard_longjmp_table, __guard_longjmp_count
        .zero 72
        .quad __guard_eh_cont_table, __guard_eh_cont_count

        .section .tls$AAA,"dw"
_tls_start:
        .byte 7
        .section .tls$ZZZ,"dw"
_tls_end:
        .byte 0
        .data
        .p2align 2
_tls_index:
        .long 0
        .section .CRT$XLA,"dr"
        .p2align 3
__xl_a:
        .quad 0, 0
        .section .rdata$T,"dr"
        .p2align 3
        .globl _tls_used
_tls_used:
        .quad _tls_start, _tls_end, _tls_index, __xl_a+8
        .long 0, 0

;--- check.py
import pathlib
import os
import re
import struct
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
link, readobj, pdbutil = sys.argv[2:]

def run(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == 0, result.stdout.decode(errors='replace')
    return result.stdout.decode()

def pe(path):
    data = path.read_bytes()
    header = struct.unpack_from('<I', data, 0x3c)[0]
    count, = struct.unpack_from('<H', data, header + 6)
    size, = struct.unpack_from('<H', data, header + 20)
    optional = header + 24
    sections = []
    for i in range(count):
        p = optional + size + i * 40
        virtual, rva, raw_size, raw = struct.unpack_from('<4I', data, p + 8)
        flags, = struct.unpack_from('<I', data, p + 36)
        sections.append((rva, max(virtual, raw_size), raw, flags))
    def offset(rva):
        for base, size, raw, flags in sections:
            if base <= rva < base + size:
                return raw + rva - base
        raise AssertionError(hex(rva))
    def directory(n):
        return struct.unpack_from('<2I', data, optional + 112 + n * 8)
    return data, optional, sections, offset, directory

for mode, source in [('native', 'native.obj'), ('full', 'full.bc'), ('thin', 'thin.bc')]:
    main = root / (mode + '.dll')
    args = [link, '/dll', '/noentry', '/import-slots', '/debug',
            '/guard:cf,longjmp,ehcont', '/dependentloadflag:0x1000',
            '/out:' + str(main), '/pdb:' + str(main.with_suffix('.pdb')),
            '/map:' + str(main.with_suffix('.map')),
            '/lldmap:' + str(main.with_suffix('.lldmap')),
            str(root / source), str(root / 'directories.obj')]
    run(*args)
    data = main.read_bytes()
    name, = set(re.findall(rb'part-[0-9a-f]{64}-2\.dll', data))
    child = root / name.decode()
    for path in [main, child]:
        data, optional, sections, offset, directory = pe(path)
        config, size = directory(10)
        assert config and size == 280, (path, config, size)
        at = offset(config)
        assert struct.unpack_from('<H', data, at + 78)[0] == 0x1000
        flags, = struct.unpack_from('<I', data, at + 144)
        assert flags & 0x10000 and flags & 0x400000
        image_base, = struct.unpack_from('<Q', data, optional + 24)
        for field, stride in [(128, 5), (160, 5), (176, 5), (264, 5)]:
            table, count = struct.unpack_from('<2Q', data, at + field)
            if not count:
                assert table == 0
                continue
            table = offset(table - image_base)
            for i in range(count):
                rva, = struct.unpack_from('<I', data, table + i * stride)
                assert any(base <= rva < base + length for base, length, _, _ in sections)
        assert bool(directory(9)[0]) == (path == main)
        assert path.with_suffix('.pdb').is_file()
        assert path.with_suffix('.map').is_file()
        assert path.with_suffix('.lldmap').is_file()
        debug = run(pdbutil, 'dump', '-types', '-ids', '-publics', str(path.with_suffix('.pdb')))
        assert 'LF_PROCEDURE' in debug, (path, debug)
        assert ('`feature`' if path == child else '`entry`') in debug, (path, debug)
    assert b'__imp_feature\0' in main.with_suffix('.lib').read_bytes()

    # Force the last auxiliary writer to fail. Earlier successful encoders
    # must leave the old image, library, PDB and maps byte-identical.
    before = {p: p.read_bytes() for p in root.iterdir() if p.is_file()}
    bad = root / 'nonexistent' / 'fail.lldmap'
    bad_args = [a if not a.startswith('/lldmap:') else '/lldmap:' + str(bad) for a in args]
    failed = subprocess.run(bad_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert failed.returncode != 0, failed.stdout
    after = {p: p.read_bytes() for p in root.iterdir() if p.is_file()}
    assert before == after, set(before) ^ set(after)

if sys.platform == 'win32':
    import ctypes
    # LOAD_LIBRARY_SEARCH_DEFAULT_DIRS honors the process's configured user
    # directories. SYSTEM32-only search deliberately excludes generated peers.
    dll_directory = os.add_dll_directory(str(root))
    policy = ctypes.c_uint32(1)
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.SetProcessMitigationPolicy.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    assert kernel32.SetProcessMitigationPolicy(2, ctypes.byref(policy), ctypes.sizeof(policy))
    for mode in ['native', 'full', 'thin']:
        image = ctypes.CDLL(str(root / (mode + '.dll')))
        assert image.entry() == 47
