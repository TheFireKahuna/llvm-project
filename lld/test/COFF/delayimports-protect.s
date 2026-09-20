# REQUIRES: x86

# Make a DLL that exports exportfn1.
# RUN: yaml2obj %p/Inputs/export.yaml -o %basename_t-exp.obj
# RUN: lld-link /out:%basename_t-exp.dll /dll %basename_t-exp.obj /export:exportfn1 /implib:%basename_t-exp.lib
# RUN: llvm-mc -triple x86_64-windows-msvc %s -filetype=obj -o %basename_t.obj

# Without the option the tables keep the layout they had: the address table is
# ordinary writable data and no protection is claimed.
# RUN: lld-link %basename_t.obj -entry:main -out:%basename_t-plain.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll
# RUN: llvm-readobj --sections --coff-imports --coff-load-config \
# RUN:   %basename_t-plain.exe | FileCheck --check-prefix=PLAIN %s

# PLAIN-NOT: Name: .didat
# PLAIN:     ImportAddressTable: 0x3
# PLAIN:     GuardFlags [ (0x0)

# With it the address table is alone in .didat, the name table joins the rest of
# the read-only data, and the module handle stays writable elsewhere.
# RUN: lld-link %basename_t.obj -entry:main -out:%basename_t-prot.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -delayload-protect
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
# PROT:      GuardFlags [ (0x3000)
# PROT-NEXT:   DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# PROT-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)
# PROT-NEXT: ]

# The flags ride beside the Control Flow Guard ones rather than replacing them.
# RUN: lld-link %basename_t.obj -entry:main -out:%basename_t-cf.exe \
# RUN:   %basename_t-exp.lib -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -delayload-protect -guard:cf
# RUN: llvm-readobj --coff-load-config %basename_t-cf.exe | \
# RUN:   FileCheck --check-prefix=CF %s

# CF:      GuardFlags [ (0x10017500)
# CF:        DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# CF-NEXT:   PROTECT_DELAYLOAD_IAT (0x1000)
# CF-NEXT: ]

# An input section of that name would share the pages the loader reprotects.
# RUN: llvm-mc -triple x86_64-windows-msvc %p/Inputs/delayimports-didat.s \
# RUN:   -filetype=obj -o %basename_t-didat.obj
# RUN: not lld-link %basename_t.obj %basename_t-didat.obj -entry:main \
# RUN:   -out:%basename_t-err.exe %basename_t-exp.lib \
# RUN:   -alternatename:__delayLoadHelper2=main \
# RUN:   -delayload:%basename_t-exp.dll -delayload-protect 2>&1 | \
# RUN:   FileCheck --check-prefix=ERR %s

# ERR: -delayload-protect: .didat holds input sections

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
