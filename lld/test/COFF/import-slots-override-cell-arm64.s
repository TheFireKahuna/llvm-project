# REQUIRES: aarch64
## The AArch64 form of a DLL's fill of its overridable-default cells: the
## builtins' __llvm_fill, which the C initializer table runs, loads a list the
## linker builds, the cells ordered by key, each {key u64 (bit 0 the read-only
## mark), rva u32, kind u32}, preceded by the read-only ranges. The linker names
## the fill to the table and keeps it only where it is needed.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium rt.s -o rt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium use.s -o use.obj
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:arm64
# RUN: lld-link -import-slots -dll -noentry rt.obj -out:rt.dll -implib:rt.lib
# RUN: lld-link -import-slots -dll -noentry use.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:use.dll
# RUN: llvm-objdump -d --no-show-raw-insn use.dll > use.txt
# RUN: llvm-objdump -s -j .rdata use.dll >> use.txt
# RUN: FileCheck --input-file=use.txt %s

## __llvm_fill loads its list, whose address its adrp and add name.
# CHECK:      adrp x0, 0x[[#%x,PAGE:]]
# CHECK-NEXT: add x0, x0, #0x[[#%x,OFF:]]

## The list header: one read-only range, two cells; no TLS; no section to seal;
## then the range {rva, size} over the read-only import entry at 0x20C8, and the
## cells, {key, rva, kind}: the writable one in .data and the read-only import
## entry whose key has bit 0 set, each of slot 0, whose RVA is at 0x44 in the
## executable's header, each of kind 2.
# CHECK:      [[#%x,PAGE+OFF]]      01000000 02000000 00000000 00000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x10]] 00000000 00000000 00000000 00000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x20]] {{[0-9a-f]+ [0-9a-f]+}} 00000000 00000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x30]] c8200000 08000000 00000000 44000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x40]] 00300000 02000000 01000000 44000000
# CHECK-NEXT: [[#%x,PAGE+OFF+0x50]] c8200000 02000000

## A thread-local cell is a slot in the template of the DLL's thread-local data.
## Its list names the TLS index and template start, and its kind is a slot in
## the template (fillCellSlot | fillCellThreadLocal = 6).
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium tl.s -o tl.obj
# RUN: lld-link -import-slots -dll -noentry tl.obj crt.obj rt.lib \
# RUN:   ntdll.lib -out:tl.dll
# RUN: llvm-objdump -d --no-show-raw-insn tl.dll > tl.txt
# RUN: llvm-objdump -s -j .rdata tl.dll >> tl.txt
# RUN: FileCheck --check-prefix=TL --input-file=tl.txt %s
# TL:      adrp x0, 0x[[#%x,PAGE:]]
# TL-NEXT: add x0, x0, #0x[[#%x,OFF:]]
## One cell; the TLS index 0x3000 and template start 0x5000; then the cell, a
## slot in the template at 0x5008, of kind 6.
# TL:      [[#%x,PAGE+OFF]]      00000000 01000000 00000000 00000000
# TL-NEXT: [[#%x,PAGE+OFF+0x10]] 00300000 00500000 00000000 00000000
# TL-NEXT: [[#%x,PAGE+OFF+0x20]] {{[0-9a-f]+ [0-9a-f]+}} 00000000 00000000
# TL-NEXT: [[#%x,PAGE+OFF+0x30]] 00000000 44000000 08500000 06000000

#--- rt.s
        .text
        .weak _Znwy
_Znwy:
        ret
        .section .drectve,"yni"
        .ascii " /OVERRIDABLE:_Znwy,0 /EXPORT:_Znwy"

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
## The C initializer table's bounds, as the C runtime provides them, so the
## linker names the fill to the table the runtime's start-up runs, and a
## stand-in for the builtins' __llvm_fill: it loads its list and references the
## NtProtectVirtualMemory it makes read-only cells writable with.
        .section .CRT$XIA,"dr"
        .globl __xi_a
__xi_a:
        .xword 0
        .section .CRT$XIZ,"dr"
        .globl __xi_z
__xi_z:
        .xword 0

        .def __llvm_fill
        .scl 2
        .type 32
        .endef
        .section .text,"xr",one_only,__llvm_fill
        .globl __llvm_fill
__llvm_fill:
        adrp x0, __llvm_fill_list
        add x0, x0, :lo12:__llvm_fill_list
        adrp x1, __imp_NtProtectVirtualMemory
        ldr x1, [x1, :lo12:__imp_NtProtectVirtualMemory]
        blr x1
        ret

#--- use.s
        .text
        .globl dllmain
dllmain:
        bl _Znwy
        ret
        .data
        .globl gnew
gnew:
        .xword _Znwy
        .section .rdata,"dr",discard,cnew
        .globl cnew
        .p2align 3
cnew:
        .xword _Znwy

#--- tl.s
        .text
        .globl dllmain
dllmain:
        ret
        .section .tls,"dw"
        .globl _tls_start
_tls_start:
        .xword 0
        .section .tls$,"dw"
        .globl tnew
        .p2align 3
tnew:
        .xword _Znwy
        .section .tls$ZZZ,"dw"
        .globl _tls_end
_tls_end:
        .xword 0
        .data
        .globl _tls_index
_tls_index:
        .word 0
        .section .rdata,"dr"
        .globl _tls_used
        .p2align 3
_tls_used:
        .xword _tls_start
        .xword _tls_end
        .xword _tls_index
        .xword 0
        .word 0
        .word 0
