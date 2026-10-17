# REQUIRES: x86

## A DLL exports the bounds of its KCFI code range as
## private data, __llvm_code_start and __llvm_code_end, which the guard tables
## do not list, and its import library holds a member with the record of the
## range: with how many functions carry each, the KCFI types and second types
## of the unsealed functions in it. The member defines __llvm_code_range$<dll>,
## by which an importer's link finds it. The library also holds imports of the
## bounds named for the DLL, <bound>$<dll>, exported as the bounds. A DLL
## without a range exports an empty one, at an address outside the export
## directory, and its import library has no record.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/dll.s -filetype=obj -o %t.dll.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/vfn.s -filetype=obj -o %t.vfn.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/cfg.s -filetype=obj -o %t.cfg.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/dll.s -filetype=obj \
# RUN:   --defsym NOREC=1 -o %t.dll.norec.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/vfn.s -filetype=obj \
# RUN:   --defsym NOREC=1 -o %t.vfn.norec.obj

## The range is [0x180001000, 0x180001060): exported, which only its export
## lists, follows it. taken and vtaken are listed.
# RUN: lld-link %t.dll.obj %t.vfn.obj %t.cfg.obj -dll -noentry -export:exported \
# RUN:   -export:taken -guard:cf -out:%t.dll -implib:%t.lib
# RUN: llvm-readobj --coff-exports --coff-load-config %t.dll \
# RUN:   | FileCheck %s --check-prefix=EXPORTS
# RUN: llvm-nm --print-armap %t.lib | FileCheck %s --check-prefix=MAP
# RUN: llvm-readobj %t.lib | FileCheck %s --check-prefix=IMPORTS
# RUN: llvm-readobj --sections --section-data --symbols %t.lib \
# RUN:   | FileCheck %s --check-prefix=RECORD

# EXPORTS:      Ordinal: 1
# EXPORTS-NEXT: Name: __llvm_code_end
# EXPORTS-NEXT: RVA: 0x1060
# EXPORTS:      Ordinal: 2
# EXPORTS-NEXT: Name: __llvm_code_start
# EXPORTS-NEXT: RVA: 0x1000
# EXPORTS:      GuardFidTable [
# EXPORTS-NEXT:   0x180001010
# EXPORTS-NEXT:   0x180001030
# EXPORTS-NEXT:   0x180001050
# EXPORTS-NEXT:   0x180001070 flags 2
# EXPORTS-NEXT: ]

## The bounds have imports only under the names of their own.
# MAP:      Archive map
# MAP-NOT:  __imp___llvm_code_{{start|end}} in
# MAP:      __imp___llvm_code_end$kcfi-code-range-export.s.tmp.dll in
# MAP-NEXT: __imp___llvm_code_start$kcfi-code-range-export.s.tmp.dll in
# MAP-NOT:  __imp___llvm_code_{{start|end}} in
# MAP:      __llvm_code_range$kcfi-code-range-export.s.tmp.dll in
# MAP-NOT:  __imp___llvm_code_{{start|end}} in

# IMPORTS:      Type: data
# IMPORTS-NEXT: Name type: export as
# IMPORTS-NEXT: Export name: __llvm_code_start
# IMPORTS-NEXT: Symbol: __imp___llvm_code_start$kcfi-code-range-export.s.tmp.dll
# IMPORTS:      Type: data
# IMPORTS-NEXT: Name type: export as
# IMPORTS-NEXT: Export name: __llvm_code_end
# IMPORTS-NEXT: Symbol: __imp___llvm_code_end$kcfi-code-range-export.s.tmp.dll

## Type 0x11111111 has two functions in the range, and type 0x33333333 and
## second type 0x66666666 one each.
# RECORD:      Name: .llvm_link_records
# RECORD:      IMAGE_SCN_LNK_REMOVE
# RECORD:      SectionData (
# RECORD-NEXT:   0000: 4C4C5243 01000C11 02111111 11023333  |LLRC..........33|
# RECORD-NEXT:   0010: 33330101 66666666 01                 |33..ffff.|
# RECORD:      Name: __llvm_code_range$kcfi-code-range-export.s.tmp.dll
# RECORD-NEXT: Value: 0
# RECORD-NEXT: Section: IMAGE_SYM_ABSOLUTE (-1)

## Without a guard function table the DLL is not sealed: both bounds name the
## start of .text, and the import library has no record.
# RUN: lld-link %t.dll.obj %t.vfn.obj -dll -noentry -export:exported \
# RUN:   -out:%t.unsealed.dll -implib:%t.unsealed.lib
# RUN: llvm-readobj --coff-exports %t.unsealed.dll \
# RUN:   | FileCheck %s --check-prefix=EMPTY
# RUN: llvm-nm --print-armap %t.unsealed.lib | FileCheck %s --check-prefix=NOREC
# EMPTY:      Name: __llvm_code_end
# EMPTY-NEXT: RVA: 0x1000
# EMPTY:      Name: __llvm_code_start
# EMPTY-NEXT: RVA: 0x1000
# NOREC-NOT:  __llvm_code_range

## Without an object that says it has KCFI prefixes, nothing is exported for
## the range.
# RUN: lld-link %t.dll.norec.obj %t.vfn.norec.obj %t.cfg.obj -dll -noentry \
# RUN:   -export:exported -guard:cf -out:%t.norec.dll
# RUN: llvm-readobj --coff-exports %t.norec.dll \
# RUN:   | FileCheck %s --check-prefix=NOPFX
# NOPFX-NOT: __llvm_code

## A DLL whose code references neither bound, such as one that makes no KCFI
## check itself, still exports its range and records it.
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/plain.s -filetype=obj -o %t.plain.obj
# RUN: lld-link %t.plain.obj %t.cfg.obj -dll -noentry -export:leaf -guard:cf \
# RUN:   -out:%t.plain.dll -implib:%t.plain.lib
# RUN: llvm-readobj --coff-exports %t.plain.dll | FileCheck %s --check-prefix=PLAIN
# RUN: llvm-nm --print-armap %t.plain.lib | FileCheck %s --check-prefix=PLAIN-MAP
# PLAIN:      Name: __llvm_code_end
# PLAIN-NEXT: RVA: 0x1018
# PLAIN:      Name: __llvm_code_start
# PLAIN-NEXT: RVA: 0x1000
# PLAIN-MAP:  __llvm_code_range$kcfi-code-range-export.s.tmp.plain.dll in

## So does one linked with an archive whose index lists the bounds, which a
## member that is not loaded references weakly.
# RUN: llvm-mc -triple x86_64-windows-msvc %t.dir/weak.s -filetype=obj -o %t.weak.obj
# RUN: llvm-lib -out:%t.weak.lib %t.weak.obj
# RUN: llvm-nm --print-armap %t.weak.lib | FileCheck %s --check-prefix=WEAK-MAP
# RUN: lld-link %t.plain.obj %t.cfg.obj %t.weak.lib -dll -noentry -export:leaf \
# RUN:   -guard:cf -out:%t.lazy.dll -implib:%t.lazy.lib
# RUN: llvm-readobj --coff-exports %t.lazy.dll | FileCheck %s --check-prefix=PLAIN
# RUN: llvm-nm --print-armap %t.lazy.lib | FileCheck %s --check-prefix=LAZY-MAP
# WEAK-MAP:   __llvm_code_start in
# LAZY-MAP:   __llvm_code_range$kcfi-code-range-export.s.tmp.lazy.dll in

## The names are reserved for the range, under any spelling of an export.
# RUN: not lld-link %t.dll.obj -dll -noentry -export:__llvm_code_start \
# RUN:   -out:%t.err.dll 2>&1 | FileCheck %s --check-prefix=ERR
# RUN: not lld-link %t.dll.obj -dll -noentry \
# RUN:   -export:taken,EXPORTAS,__llvm_code_end \
# RUN:   -out:%t.err.dll 2>&1 | FileCheck %s --check-prefix=ERR-AS
# ERR: error: cannot export __llvm_code_start: the name is reserved for the KCFI code range
# ERR-AS: error: cannot export taken as __llvm_code_end: the name is reserved for the KCFI code range

#--- dll.s
.ifndef NOREC
  .linktypeprefixes
.endif
        .globl @feat.00
@feat.00 = 0x800

        .def exported; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,exported
        .p2align 4
        .fill 4, 1, 0x90
__cfi_exported:
        nopl 0x71c5a06(%rax)
        movl $0x22222222, %eax
        .globl exported
exported:
        retq

        .def taken; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,taken
        .p2align 4
        .fill 4, 1, 0x90
__cfi_taken:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl taken
taken:
        movl $1, %eax
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

        .data
        .quad __llvm_code_start
        .quad __llvm_code_end
        .quad taken

        .section .gfids$y,"dr"
        .symidx taken

#--- vfn.s
.ifndef NOREC
  .linktypeprefixes
.endif
        .def vtaken; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,vtaken
        .p2align 4
__cfi_vtaken:
        .long 0x66666666
        nopl 0x71c5a06(%rax)
        movl $0x33333333, %eax
        .globl vtaken
vtaken:
        retq

        .def other; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,other
        .p2align 4
        .fill 4, 1, 0x90
__cfi_other:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl other
other:
        movl $2, %eax
        retq

        .data
        .quad vtaken
        .quad other

        .section .gfids$y,"dr"
        .symidx vtaken
        .symidx other

#--- plain.s
  .linktypeprefixes
        .globl @feat.00
@feat.00 = 0x800

        .def leaf; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,leaf
        .p2align 4
        .fill 4, 1, 0x90
__cfi_leaf:
        nopl 0x71c5a06(%rax)
        movl $0x11111111, %eax
        .globl leaf
leaf:
        movl $1, %eax
        retq

        .section .gfids$y,"dr"
        .symidx leaf

#--- weak.s
  .linktypeprefixes
        .def unused; .scl 2; .type 32; .endef
        .section .text,"xr",one_only,unused
        .globl unused
unused:
        leaq __llvm_code_start(%rip), %rax
        leaq __llvm_code_end(%rip), %rax
        retq

        .weak __llvm_code_start
__llvm_code_start = __llvm_code_empty
        .weak __llvm_code_end
__llvm_code_end = __llvm_code_empty
        .section .rdata,"dr",discard,__llvm_code_empty
        .globl __llvm_code_empty
__llvm_code_empty:
        .byte 0

#--- cfg.s
        .section .rdata,"dr"
        .p2align 3
        .globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 128, 1, 0
