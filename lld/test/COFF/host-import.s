# REQUIRES: x86, aarch64

## A DLL binds what it imports from an executable in the executable that loads
## it, whatever that file's name: no import descriptor names the executable,
## and each import's pointer is a word of .sealed, after the word that .sealed
## begins with, which the image's fill writes by name. Calls, loads and the
## import's thunk read the pointer where they would read the entry. A word of
## data that holds the address of such an import is written by the fill from
## the pointer, with its offset, and a section of .rdata that holds one leaves
## for .sealed. The fill's list names each import as a word of kind 3, the
## first of the list, by its pointer and its name, which follows the entries,
## and zero for the executable. The import library of an executable states that it is one, which
## decides whatever the name; a library with no such statement names one by
## the suffix .exe. An executable leaves its imports to the loader, and an
## import by ordinal is in error.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{p} = X64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{run} = \
# DEFINE:   %{mc} host.s -o host.obj && %{mc} --defsym %{a}=1 p.s -o p.obj && \
# DEFINE:   %{mc} --defsym %{a}=1 rt.s -o rt.obj && \
# DEFINE:   rm -f rt.lib && llvm-ar rcs rt.lib rt.obj && \
# DEFINE:   lld-link -machine:%{a} -def:ntdll.def -out:ntdll.lib && \
# DEFINE:   lld-link -machine:%{a} -entry:main host.obj -out:host.bin && \
# DEFINE:   llvm-readobj --coff-link-records host.lib | \
# DEFINE:     FileCheck --check-prefix=RECORD %s && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry p.obj host.lib ntdll.lib \
# DEFINE:     rt.lib -out:p.dll && \
# DEFINE:   llvm-readobj --coff-imports p.dll > p.txt && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn -s -j .text -j .rdata \
# DEFINE:     -j .sealed -j .data p.dll >> p.txt && \
# DEFINE:   FileCheck --check-prefixes=IMP,CHECK,%{p} %s < p.txt \
# DEFINE:     --implicit-check-not=host && \
# DEFINE:   lld-link -machine:%{a} -def:host.def -implib:exe.lib && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry p.obj exe.lib ntdll.lib \
# DEFINE:     rt.lib -out:named.dll && \
# DEFINE:   llvm-readobj --coff-imports named.dll | \
# DEFINE:     FileCheck --check-prefix=IMP %s --implicit-check-not=host && \
# DEFINE:   lld-link -machine:%{a} -entry:f -subsystem:console p.obj exe.lib \
# DEFINE:     ntdll.lib rt.lib -out:p.exe && \
# DEFINE:   llvm-readobj --coff-imports p.exe | FileCheck --check-prefix=EXE %s \
# DEFINE:     --implicit-check-not=LdrGetProcedureAddressForCaller

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# REDEFINE: %{p} = A64
# RUN: %{run}

# RECORD:      Name: host.bin
# RECORD-NEXT: Kind: Executable

## The fill that binds imports by name has its own import, which an image
## without such imports never has.
# IMP:      Name: ntdll.dll
# IMP-NEXT: ImportLookupTableRVA:
# IMP-NEXT: ImportAddressTableRVA:
# IMP-NEXT: Symbol: LdrGetProcedureAddressForCaller
# IMP-NEXT: }

## The list: four words and .sealed. First the pointers of hdata and hfn,
## each with its name, after the words; then the word of .rdata, now in
## .sealed, hdata's address plus 8, and the word of .data, hfn's.
# CHECK:      Contents of section .rdata:
# CHECK:      180002010 00000000 00000000 04000000 00000000
# CHECK-NEXT: 180002020 00300000 20000000 00000000 00000000
# CHECK-NEXT: 180002030 08300000 03000000 70200000 00000000
# CHECK-NEXT: 180002040 10300000 03000000 76200000 00000000
# CHECK-NEXT: 180002050 18300000 08300000 08000000 00000000
# CHECK-NEXT: 180002060 00400000 10300000 00000000 00000000
# CHECK-NEXT: 180002070 68646174 61006866 6e00
# CHECK:      Contents of section .sealed:
# CHECK-NEXT: 180003000 00000000 00000000 00000000 00000000
# CHECK-NEXT: 180003010 00000000 00000000 f1f1f1f1 f1f1f1f1
# CHECK-NEXT: Contents of section .data:
# CHECK-NEXT: 180004000 f1f1f1f1 f1f1f1f1

# X64:      callq *0x200a(%rip)  # 0x180003010
# X64-NEXT: movq 0x1ffb(%rip), %rax  # 0x180003008
# X64-NEXT: callq 0x180001030
# X64:      180001030: jmpq *0x1fda(%rip)  # 0x180003010

# A64:      ldr x16, 0x180003010
# A64-NEXT: blr x16
# A64-NEXT: ldr x0, 0x180003008
# A64-NEXT: bl 0x180001020
# A64:      180001020: ldr x16, 0x180003010
# A64-NEXT: br x16

# EXE:      Name: host.exe
# EXE:      Symbol: hdata
# EXE-NEXT: Symbol: hfn

# RUN: lld-link -machine:arm64 -def:ord.def -implib:ord.lib
# RUN: not lld-link -machine:arm64 -dll -noentry p.obj ord.lib ntdll.lib rt.lib \
# RUN:   -out:bad.dll 2>&1 | FileCheck --check-prefix=ORDINAL %s
# ORDINAL: error: __imp_hfn, imported from host.exe by ordinal, is bound by name as the image starts; import it by name
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium q.s -o q.obj
# RUN: not lld-link -machine:arm64 -dll -noentry q.obj exe.lib -out:bad.dll \
# RUN:   2>&1 | FileCheck --check-prefix=NORT %s
# NORT: error: __imp_hfn, imported from host.exe, is bound by name as the image starts, which needs __llvm_fill_dll_imports; link the builtins library

#--- host.s
.text
.globl main
main:
  ret
.globl hfn
hfn:
  ret
.data
.globl hdata
hdata:
  .quad 1
.section .drectve,"yn"
  .ascii " -export:hfn -export:hdata,data"

#--- host.def
NAME host.exe
EXPORTS
hdata DATA
hfn

#--- ord.def
NAME host.exe
EXPORTS
hdata DATA
hfn @5 NONAME

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
LdrGetProcedureAddressForCaller
NtProtectVirtualMemory
RtlExitUserProcess

#--- p.s
.text
.globl f
f:
.ifdef x64
  callq *__imp_hfn(%rip)
  movq __imp_hdata(%rip), %rax
  callq hfn
  retq
.else
  adrp x16, __imp_hfn
  ldr x16, [x16, :lo12:__imp_hfn]
  blr x16
  adrp x0, __imp_hdata
  ldr x0, [x0, :lo12:__imp_hdata]
  bl hfn
  ret
.endif
.data
  .quad hfn
.section .rdata,"dr"
  .quad hdata+8
.section .drectve,"yn"
  .ascii " -include:f"

#--- q.s
.data
  .quad __imp_hfn

#--- rt.s
.macro entry name, import
.section .text,"xr",one_only,\name
.globl \name
.def \name
.type 32
.endef
\name:
.ifdef x64
  leaq __llvm_fill_list(%rip), %rax
  jmpq *__imp_\import(%rip)
.else
  adrp x0, __llvm_fill_list
  adrp x16, __imp_\import
  ldr x16, [x16, :lo12:__imp_\import]
  br x16
.endif
.endm
  entry __llvm_fill_dll, NtProtectVirtualMemory
  entry __llvm_fill_exe, NtProtectVirtualMemory
  entry __llvm_fill_dll_imports, LdrGetProcedureAddressForCaller
  entry __llvm_fill_exe_imports, LdrGetProcedureAddressForCaller
.section .text,"xr",one_only,__llvm_fill_true
.globl __llvm_fill_true
__llvm_fill_true:
  ret
