# REQUIRES: x86

# Make a DLL that exports exportfn1.
# RUN: yaml2obj %p/Inputs/export.yaml -o %basename_t-exp.obj
# RUN: lld-link /out:%basename_t-exp.dll /dll %basename_t-exp.obj /export:exportfn1 /implib:%basename_t-exp.lib
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %basename_t.obj

# Without /guard:cf the tables keep their layout: the address table is
# ordinary writable data and no protection is claimed.
# RUN: lld-link %basename_t.obj -entry:main -out:%basename_t-plain.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll
# RUN: llvm-readobj --sections --coff-imports %basename_t-plain.exe \
# RUN:   | FileCheck --check-prefix=PLAIN %s

# PLAIN-NOT: Name: .didat
# PLAIN:     ImportAddressTable: 0x3

# With /guard:cf the address table is alone in a writable .didat, the name
# table joins the rest of the read-only data, the module handle stays in
# .data, and the image asks the loader to keep the table read-only.
# RUN: lld-link %basename_t.obj -entry:main -out:%basename_t-prot.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf
# RUN: llvm-readobj --sections --coff-imports --coff-load-config \
# RUN:   %basename_t-prot.exe | FileCheck --check-prefix=PROT %s

# PROT:      Name: .rdata
# PROT:      VirtualAddress: 0x2000
# PROT:      Name: .data
# PROT:      VirtualAddress: 0x3000
# PROT:      IMAGE_SCN_MEM_WRITE
# PROT:      Name: .didat
# PROT:      VirtualSize: 0x10
# PROT-NEXT: VirtualAddress: 0x5000
# PROT:      IMAGE_SCN_MEM_WRITE
# PROT:      ModuleHandle: 0x3
# PROT-NEXT: ImportAddressTable: 0x5000
# PROT-NEXT: ImportNameTable: 0x2
# PROT:      GuardFlags [ (0x17500)
# PROT-NEXT:   CF_EXPORT_SUPPRESSION_INFO_PRESENT (0x4000)
# PROT-NEXT:   CF_FUNCTION_TABLE_PRESENT (0x400)
# PROT-NEXT:   CF_INSTRUMENTED (0x100)
# PROT-NEXT:   CF_LONGJUMP_TABLE_PRESENT (0x10000)
# PROT-NEXT:   DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# PROT-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)
# PROT-NEXT: ]

# A delay-load helper whose object says that it writes the table only while
# the table is writable gets the same layout and flags without /guard:cf, and
# in a MinGW image too.
# RUN: llvm-mc -triple x86_64-windows-msvc %p/Inputs/delayimports-helper.s \
# RUN:   -filetype=obj -o %basename_t-helper.obj
# RUN: llvm-mc -triple x86_64-windows-msvc %p/Inputs/delayimports-helper.s \
# RUN:   -filetype=obj --defsym RECORD=1 -o %basename_t-helper-rec.obj
# RUN: lld-link %basename_t.obj %basename_t-helper-rec.obj -entry:main \
# RUN:   -out:%basename_t-delay.exe %basename_t-exp.lib \
# RUN:   -delayload:%basename_t-exp.dll
# RUN: llvm-readobj --sections --coff-imports --coff-load-config \
# RUN:   %basename_t-delay.exe | FileCheck --check-prefix=DELAY %s
# RUN: lld-link -lldmingw %basename_t.obj %basename_t-helper-rec.obj \
# RUN:   -entry:main -out:%basename_t-mingw-delay.exe %basename_t-exp.lib \
# RUN:   -delayload:%basename_t-exp.dll
# RUN: llvm-readobj --sections --coff-imports --coff-load-config \
# RUN:   %basename_t-mingw-delay.exe | FileCheck --check-prefix=DELAY %s

# DELAY:      Name: .didat
# DELAY:      IMAGE_SCN_MEM_WRITE
# DELAY:      ImportAddressTable: 0x5000
# DELAY:      GuardFlags [ (0x3000)
# DELAY-NEXT:   DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# DELAY-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)
# DELAY-NEXT: ]

# The record speaks for the helper the thunks call, not for the image: a helper
# without it keeps the unprotected layout although another object has it.
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj --defsym RECORD=1 \
# RUN:   -o %basename_t-rec.obj
# RUN: lld-link %basename_t-rec.obj %basename_t-helper.obj -entry:main \
# RUN:   -out:%basename_t-other.exe %basename_t-exp.lib \
# RUN:   -delayload:%basename_t-exp.dll
# RUN: llvm-readobj --sections --coff-imports %basename_t-other.exe \
# RUN:   | FileCheck --check-prefix=PLAIN %s

# MinGW's delay-load helper stores to the table directly, so /guard:cf leaves a
# MinGW image with the unprotected layout.
# RUN: lld-link -lldmingw %basename_t.obj -entry:main -out:%basename_t-mingw.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf
# RUN: llvm-readobj --coff-load-config %basename_t-mingw.exe \
# RUN:   | FileCheck --check-prefix=MINGW %s

# MINGW:     GuardFlags [ (0x14500)
# MINGW-NOT: DELAYLOAD

# A MinGW image whose helper has the record keeps the table read-only under
# /guard:cf too.
# RUN: lld-link -lldmingw %basename_t.obj %basename_t-helper-rec.obj \
# RUN:   -entry:main -out:%basename_t-mingw-rec.exe %basename_t-exp.lib \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf
# RUN: llvm-readobj --coff-load-config %basename_t-mingw-rec.exe \
# RUN:   | FileCheck --check-prefix=MINGW-REC %s

# MINGW-REC:      GuardFlags [ (0x17500)
# MINGW-REC:        DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# MINGW-REC-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)

# Input sections named .didat follow the table in .didat, in name order and
# whether or not they are writable, starting on the next page: the loader makes
# the section read-only at load and reopens only the table's pages, so no page
# holds both, and no warning is given. Without them .didat holds only the
# table, as the PROT run shows. A section merged into .didat, or merging .didat
# itself, would share the table's pages.
# RUN: llvm-mc -triple x86_64-windows-msvc %p/Inputs/delayimports-didat.s \
# RUN:   -filetype=obj -o %basename_t-didat.obj
# RUN: lld-link %basename_t.obj %basename_t-didat.obj -entry:main \
# RUN:   -out:%basename_t-input.exe %basename_t-exp.lib \
# RUN:   -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf 2>&1 \
# RUN:   | FileCheck --allow-empty --check-prefix=WARN-INPUT %s
# RUN: llvm-readobj --sections --coff-imports --coff-load-config \
# RUN:   %basename_t-input.exe | FileCheck --check-prefix=INPUT %s
# RUN: llvm-readobj --hex-dump=.didat %basename_t-input.exe \
# RUN:   | FileCheck --check-prefix=INPUT-HEX %s
# RUN: not lld-link %basename_t.obj -entry:main -out:%basename_t-err.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf -merge:.foo=.bar \
# RUN:   -merge:.bar=.didat 2>&1 | FileCheck --check-prefix=ERR-INTO %s
# RUN: not lld-link %basename_t.obj -entry:main -out:%basename_t-err.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf -merge:.didat=.data 2>&1 \
# RUN:   | FileCheck --check-prefix=ERR-FROM %s

# WARN-INPUT-NOT: didat

# INPUT:      Name: .pdata
# INPUT:      Name: .didat
# INPUT-NEXT: VirtualSize: 0x1010
# INPUT-NEXT: VirtualAddress: 0x5000
# INPUT:      IMAGE_SCN_MEM_WRITE
# INPUT:      Name: .reloc
# INPUT:      ImportAddressTable: 0x5000
# INPUT:      GuardFlags [ (0x17500)
# INPUT:        DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# INPUT-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)

# INPUT-HEX:      0x140005000 10100040 01000000 00000000 00000000
# INPUT-HEX-NOT:  01000000 00000000 02000000
# INPUT-HEX:      0x140006000 01000000 00000000 02000000 00000000
# ERR-INTO-DAG: error: /merge:.bar=.didat: .didat holds the protected delay-load import address table and cannot be merged into
# ERR-INTO-DAG: error: /merge:.foo=.bar: .didat holds the protected delay-load import address table and cannot be merged into
# ERR-FROM: error: /merge:.didat=.data: .didat holds the protected delay-load import address table and cannot be merged

.ifdef RECORD
	.linkprotecteddelayiat
.endif

	.text
	.globl	main
main:
	movq	__imp_exportfn1(%rip), %rax
	callq	*%rax
	xorl	%eax, %eax
	retq

# Load configuration directory entry (winnt.h _IMAGE_LOAD_CONFIG_DIRECTORY64).
# The linker will define the __guard_* symbols.
        .section .rdata,"dr"
.globl _load_config_used
_load_config_used:
        .long 256
        .fill 124, 1, 0
        .quad __guard_fids_table
        .quad __guard_fids_count
        .long __guard_flags
        .fill 12, 1, 0
        .quad __guard_iat_table
        .quad __guard_iat_count
        .quad __guard_longjmp_table
        .quad __guard_fids_count
        .fill 84, 1, 0
