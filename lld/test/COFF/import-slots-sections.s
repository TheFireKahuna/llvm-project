# REQUIRES: x86, aarch64

## Read-only slots lie inside the IAT directory: .rdata sections holding them
## follow the tables, other read-only sections precede .rdata; slots in unwind
## data, the loader's thread-local template, or outside the laid-out directory
## are errors.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{e} = itanium
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-%{e}
# DEFINE: %{rt} =
# DEFINE: %{link} = lld-link -machine:%{a} -dll -noentry imp.lib %{rt} \
# DEFINE:   -include:__imp_fn
# DEFINE: %{dump} = llvm-readobj --file-headers --sections --coff-imports
# DEFINE: %{tls} = not %{link} tls.obj -out:tls.dll 2>&1 | \
# DEFINE:   FileCheck --check-prefix=TLS-itanium %s
# DEFINE: %{run} = lld-link -machine:%{a} -def:imp.def -out:imp.lib && \
# DEFINE:   lld-link -machine:%{a} -def:np.def -out:ntposix.lib && \
# DEFINE:   %{mc} %S/Inputs/fill-entry.s -o fill.obj && \
# DEFINE:   %{mc} rdata.s -o rdata.obj && %{mc} groups.s -o groups.obj && \
# DEFINE:   %{mc} ro.s -o ro.obj && %{mc} data.s -o data.obj && \
# DEFINE:   %{mc} xdata.s -o xdata.obj && %{mc} tls.s -o tls.obj && \
# DEFINE:   %{mc} code.s -o code.obj && %{link} rdata.obj -out:rdata.dll && \
# DEFINE:   %{dump} rdata.dll | FileCheck --check-prefix=RDATA %s && \
# DEFINE:   %{link} rdata.obj groups.obj ro.obj -out:all.dll \
# DEFINE:     -include:__imp_fn2 && \
# DEFINE:   %{dump} all.dll | FileCheck --check-prefix=ALL %s && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry imp.lib ro.obj \
# DEFINE:     -out:only.dll && \
# DEFINE:   %{dump} only.dll | FileCheck --check-prefix=ONLY %s && \
# DEFINE:   %{tls} && \
# DEFINE:   not lld-link -machine:%{a} -entry:tv -subsystem:console imp.lib \
# DEFINE:     tls.obj xdata.obj -out:bad.exe 2>&1 | \
# DEFINE:     FileCheck --check-prefixes=BAD,BAD-%{e} --allow-unused-prefixes %s && \
# DEFINE:   %{link} ro.obj -merge:.ro=.data -out:merged.dll && \
# DEFINE:   %{dump} merged.dll | FileCheck --check-prefix=MERGED %s && \
# DEFINE:   not %{link} ro.obj -merge:.ro=.rdata -out:bad.dll 2>&1 | \
# DEFINE:     FileCheck --check-prefix=OUTSIDE %s && \
# DEFINE:   not %{link} data.obj -section:.data,r -out:bad.dll 2>&1 | \
# DEFINE:     FileCheck --check-prefix=READONLY %s && \
# DEFINE:   not %{link} data.obj code.obj -merge:.data=.text -out:bad.dll \
# DEFINE:     2>&1 | FileCheck --check-prefix=EXEC %s && \
# DEFINE:   not %{link} ro.obj -section:.ro,rw -out:bad.dll 2>&1 | \
# DEFINE:     FileCheck --check-prefix=PAGES %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}
## An NT-POSIX DLL's thread-local data is read-only, and its fill registers it.
# REDEFINE: %{e} = ntposix
# REDEFINE: %{rt} = ntposix.lib fill.obj
# REDEFINE: %{tls} = %{link} tls.obj -out:tls.dll && \
# REDEFINE:   %{dump} tls.dll | FileCheck --check-prefix=TLS-ntposix %s
# RUN: %{run}

## The directory holds the table, its terminator and the section's two words.
# RDATA:      IATRVA: 0x[[#%X,IAT:]]
# RDATA-NEXT: IATSize: 0x20
# RDATA:      Name: imp.dll
# RDATA:      ImportAddressTableRVA: 0x[[#IAT]]
# RDATA:      Name: imp.dll
# RDATA:      ImportAddressTableRVA: 0x[[#IAT+24]]

## .ro, then .rdata: the table, the section of .rdata, and the two $ groups.
# ALL:      IATRVA: 0x1000
# ALL-NEXT: IATSize: 0x1038
# ALL:      Name: .ro
# ALL-NEXT: VirtualSize: 0x10
# ALL-NEXT: VirtualAddress: 0x1000
# ALL:      Name: .rdata
# ALL-NEXT: VirtualSize:
# ALL-NEXT: VirtualAddress: 0x2000
# ALL:      Name: imp.dll
# ALL:      ImportAddressTableRVA: 0x2000
# ALL:      Name: imp.dll
# ALL:      ImportAddressTableRVA: 0x2030
# ALL:      Name: imp.dll
# ALL:      ImportAddressTableRVA: 0x1008
# ALL:      Name: imp.dll
# ALL:      ImportAddressTableRVA: 0x2020
# ALL-NOT:  Name: imp.dll

## Without the root fn has no entry, and the directory is .ro alone.
# ONLY:      IATRVA: 0x1000
# ONLY-NEXT: IATSize: 0x10

## The image's thread-local template holds read-only slots, in a section
## before .rdata, in a DLL as in an executable; the loader's holds none, since
## threads copy it before the image's fill runs.
# TLS-itanium:      error: tls.obj: .tls$ holds the address of fn, imported from imp.dll, at offset 0x0, in the loader's thread-local template, which threads copy before the image's fill runs
# TLS-ntposix:      IATRVA: 0x2000
# TLS-ntposix-NEXT: IATSize: 0x1020
# TLS-ntposix:      Name: .tls
# TLS-ntposix-NOT:  IMAGE_SCN_MEM_WRITE
# TLS-ntposix:      Name: .rdata

# BAD-itanium-DAG: error: tls.obj: .tls$ holds the address of fn, imported from imp.dll, at offset 0x0, in the loader's thread-local template, which threads copy before the image's fill runs
# BAD-DAG: error: xdata.obj: .xdata holds the address of fn, imported from imp.dll, at offset 0x8, but is read-only and cannot be laid out with the import address table; refer to __imp_fn there

## Merged into a writable section, .ro's slot is writable and stays there.
# MERGED:     IATSize: 0x10
# MERGED-NOT: Name: .ro
# MERGED:     ImportAddressTableRVA: 0x[[#%X,DATA:]]
# MERGED:     ImportAddressTableRVA: 0x[[#DATA+0x1008]]

# OUTSIDE:  error: ro.obj: the address of fn, imported from imp.dll, at offset 0x8 of .ro is in the read-only section .rdata, where /merge:.ro=.rdata put it, outside the import address table, where the loader cannot write it; keep its section apart from /merge and /section, or make it writable
# READONLY: error: data.obj: the address of fn, imported from imp.dll, at offset 0x0 of .data is in the read-only section .data outside the import address table
# EXEC:     error: data.obj: the address of fn, imported from imp.dll, at offset 0x0 of .data is in the executable section .text, where the loader cannot write it
# PAGES:    error: section .rdata shares the pages of the import address table, which holds in-place import slots, with section .ro but not its protection

#--- imp.def
LIBRARY imp.dll
EXPORTS
fn
fn2

#--- rdata.s
.section .rdata,"dr"
  .quad 3
  .quad fn

#--- groups.s
.section .rdata$a,"dr"
  .quad 4
.section .rdata$b,"dr"
  .quad fn2

#--- ro.s
.section .ro,"dr"
  .quad 1
  .quad fn

#--- data.s
.data
  .quad fn

#--- np.def
LIBRARY ntposix.dll
EXPORTS
__tp_register

#--- xdata.s
.section .xdata,"dr"
  .quad 2
  .quad fn

#--- tls.s
.section .tls$,"dw"
.globl tv
tv:
  .quad fn

#--- code.s
.text
  ret
