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

# -import-slots asks for the same layout and flags without /guard:cf.
# RUN: lld-link %basename_t.obj -entry:main -out:%basename_t-slots.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -import-slots
# RUN: llvm-readobj --sections --coff-imports --coff-load-config \
# RUN:   %basename_t-slots.exe | FileCheck --check-prefix=SLOTS %s

# SLOTS:      Name: .didat
# SLOTS:      IMAGE_SCN_MEM_WRITE
# SLOTS:      ImportAddressTable: 0x5000
# SLOTS:      GuardFlags [ (0x3000)
# SLOTS-NEXT:   DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# SLOTS-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)
# SLOTS-NEXT: ]

# MinGW's delay-load helper stores to the table directly, so a MinGW image
# keeps the unprotected layout.
# RUN: lld-link -lldmingw %basename_t.obj -entry:main -out:%basename_t-mingw.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf
# RUN: llvm-readobj --coff-load-config %basename_t-mingw.exe \
# RUN:   | FileCheck --check-prefix=MINGW %s

# MINGW:     GuardFlags [ (0x14500)
# MINGW-NOT: DELAYLOAD

# An input section named .didat, or a section merged into .didat, would share
# the pages the loader reprotects; merging .didat itself would share them too.
# RUN: llvm-mc -triple x86_64-windows-msvc %p/Inputs/delayimports-didat.s \
# RUN:   -filetype=obj -o %basename_t-didat.obj
# RUN: not lld-link %basename_t.obj %basename_t-didat.obj -entry:main \
# RUN:   -out:%basename_t-err.exe %basename_t-exp.lib \
# RUN:   -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf 2>&1 \
# RUN:   | FileCheck --check-prefix=ERR-INPUT %s
# RUN: not lld-link %basename_t.obj -entry:main -out:%basename_t-err.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf -merge:.foo=.bar \
# RUN:   -merge:.bar=.didat 2>&1 | FileCheck --check-prefix=ERR-INTO %s
# RUN: not lld-link %basename_t.obj -entry:main -out:%basename_t-err.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -guard:cf -merge:.didat=.data 2>&1 \
# RUN:   | FileCheck --check-prefix=ERR-FROM %s

# ERR-INPUT: error: input sections named .didat cannot share the protected delay-load import address table's section
# ERR-INTO-DAG: error: /merge:.bar=.didat: .didat holds the protected delay-load import address table and cannot be merged into
# ERR-INTO-DAG: error: /merge:.foo=.bar: .didat holds the protected delay-load import address table and cannot be merged into
# ERR-FROM: error: /merge:.didat=.data: .didat holds the protected delay-load import address table and cannot be merged

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
