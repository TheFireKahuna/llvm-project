# REQUIRES: x86, aarch64

## Weakness belongs to the reference: an object with no weak reference to fin
## refers to it strongly through __imp_fin (loading the member, no null
## default), whichever producer made it, unless the link has no fork object;
## a hidden or protected weak reference keeps its null default.

# RUN: rm -rf %t && split-file %s %t && cd %t

# DEFINE: %{a} = x64
# DEFINE: %{t} = x86_64
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=%{t}-windows-itanium
# DEFINE: %{link} = lld-link -machine:%{a} -entry:main -subsystem:console \
# DEFINE:   -debug:symtab -verbose -out:a.exe
# DEFINE: %{check} = FileCheck --check-prefix=LOAD %s && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a} %s
# DEFINE: %{own} = \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a}-weak %s && \
# DEFINE:   llvm-readobj --coff-imports a.exe | FileCheck --allow-empty \
# DEFINE:     --implicit-check-not=def.dll %s
# DEFINE: %{run} = %{mc} %{a}/strong.s -o strong.obj && \
# DEFINE:   %{mc} %{a}/weak.s -o weak.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/strong.s \
# DEFINE:     -o foreign.obj && %{mc} def.s -o def.obj && \
# DEFINE:   llvm-lib -machine:%{a} def.obj -out:def.lib && \
# DEFINE:   %{link} strong.obj weak.obj def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} weak.obj strong.obj def.lib 2>&1 | %{check} && \
# DEFINE:   %{link} def.lib weak.obj strong.obj 2>&1 | %{check} && \
# DEFINE:   %{link} weak.obj def.lib strong.obj 2>&1 | %{check} && \
# DEFINE:   %{link} weak.obj strong.obj def.obj && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a} %s && \
# DEFINE:   not %{link} strong.obj weak.obj 2>&1 | \
# DEFINE:     FileCheck --check-prefix=UNDEF %s && \
# DEFINE:   not %{link} weak.obj strong.obj 2>&1 | \
# DEFINE:     FileCheck --check-prefix=UNDEF %s && \
# DEFINE:   %{link} -entry:other weak.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=WEAK %s && \
# DEFINE:   llvm-objdump -d a.exe | FileCheck --check-prefix=%{a}-weak %s && \
# DEFINE:   %{link} foreign.obj weak.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=LOAD %s && \
# DEFINE:   %{link} weak.obj foreign.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=LOAD %s && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{t}-windows-msvc %{a}/weak.s \
# DEFINE:     -o foreign-weak.obj && \
# DEFINE:   %{link} foreign.obj foreign-weak.obj def.lib 2>&1 | \
# DEFINE:     FileCheck --check-prefix=WEAK %s && \
# DEFINE:   lld-link -machine:%{a} -dll -noentry def.obj -export:fin \
# DEFINE:     -out:def.dll -implib:imp.lib && \
# DEFINE:   %{mc} %{a}/weak.s --defsym VIS=3 -o hidden.obj && \
# DEFINE:   %{mc} %{a}/weak.s --defsym VIS=2 -o protected.obj && \
# DEFINE:   %{link} -entry:other hidden.obj imp.lib && %{own} && \
# DEFINE:   %{link} -entry:other imp.lib hidden.obj && %{own} && \
# DEFINE:   %{link} -entry:other protected.obj imp.lib && %{own} && \
# DEFINE:   %{mc} --defsym %{a}=1 rt.s -o rt.obj && \
# DEFINE:   rm -f rt.lib && llvm-ar rcs rt.lib rt.obj && \
# DEFINE:   lld-link -machine:%{a} -def:ntdll.def -out:ntdll.lib && \
# DEFINE:   %{link} -entry:other weak.obj imp.lib ntdll.lib rt.lib && \
# DEFINE:   llvm-readobj --coff-imports a.exe > cell.txt && \
# DEFINE:   llvm-objdump -d --no-show-raw-insn -s -j .text -j .rdata a.exe \
# DEFINE:     >> cell.txt && \
# DEFINE:   FileCheck --check-prefixes=CELL,%{a}-cell %s < cell.txt \
# DEFINE:     --implicit-check-not=def.dll && \
# DEFINE:   %{link} strong.obj weak.obj imp.lib ntdll.lib rt.lib && \
# DEFINE:   llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=DLL %s && \
# DEFINE:   %{link} imp.lib weak.obj strong.obj ntdll.lib rt.lib && \
# DEFINE:   llvm-readobj --coff-imports a.exe | FileCheck --check-prefix=DLL %s

# RUN: %{run}
# REDEFINE: %{a} = arm64
# REDEFINE: %{t} = aarch64
# RUN: %{run}

## An import library's offer of fin satisfies a reference that only weak
## mentions name. Its pointer is a word of .sealed that the image's fill
## writes by name from def.dll if a load of def.dll precedes the image's, and
## leaves null otherwise, so the image does not import from def.dll. The
## fill's list names def.dll in UTF-16. A strong mention, in any order, makes
## fin an import that the loader binds.
# CELL:      Name: ntdll.dll
# CELL:      Contents of section .rdata:
# CELL:      140002030 00000000 00000000 08300000 03000000
# CELL-NEXT: 140002040 58200000 48200000 64006500 66002e00
# CELL-NEXT: 140002050 64006c00 6c000000 66696e00
# DLL:       Name: def.dll
# DLL-NEXT:  ImportLookupTableRVA:
# DLL-NEXT:  ImportAddressTableRVA:
# DLL-NEXT:  Symbol: fin

# LOAD: Loaded def.lib(def.obj) for fin
# WEAK-NOT: for fin
# UNDEF: error: undefined symbol: __declspec(dllimport) fin

# x64-LABEL: <main>:
# x64-NEXT:  callq {{.*}} <fin>

# x64-weak-LABEL: <other>:
# x64-weak-NEXT:  movq $0x0, %rax

# arm64-LABEL: <main>:
# arm64-NEXT:  adr x16, 0x[[#%x,FIN:]]
# arm64-NEXT:  blr x16
# arm64:       {{0*}}[[#FIN]] <fin>:

# arm64-weak-LABEL: <other>:
# arm64-weak-NEXT:  mov x0, #0x0

# x64-cell-LABEL: <other>:
# x64-cell-NEXT:  movq {{.*}}(%rip), %rax  # 0x140003008

# arm64-cell-LABEL: <other>:
# arm64-cell-NEXT:  ldr x0, 0x140003008

## Only weak mentions name fin, so neither /delayload nor an export makes
## def.dll a dependency.
# RUN: not lld-link -machine:arm64 -entry:other -subsystem:console weak.obj \
# RUN:   imp.lib ntdll.lib rt.lib -delayload:def.dll \
# RUN:   -alternatename:__delayLoadHelper2=other 2>&1 | \
# RUN:   FileCheck --check-prefix=DELAY %s
# RUN: not lld-link -machine:arm64 -dll -noentry weak.obj imp.lib ntdll.lib \
# RUN:   rt.lib -export:fin 2>&1 | FileCheck --check-prefix=EXPORT %s
## A DLL's name is UTF-8, and its weak imports' list names it in UTF-16.
# RUN: lld-link -machine:arm64 -def:u.def -implib:u.lib
# RUN: lld-link -machine:arm64 -entry:other -subsystem:console weak.obj u.lib \
# RUN:   ntdll.lib rt.lib -out:u.exe
# RUN: llvm-objdump -s -j .rdata u.exe | FileCheck --check-prefix=UTF16 %s
# UTF16:      6400e900 2e006400 6c006c00 00006669
# UTF16-NEXT: 6e00

# DELAY:  error: /delayload:def.dll: only weak references name fin, which this would make a dependency; refer to it strongly
# EXPORT: error: /export:fin: only weak references name fin, imported from def.dll, which this would make a dependency; refer to it strongly

#--- x64/strong.s
.globl main
main:
  callq *__imp_fin(%rip)
  retq

## VIS states the weak reference's visibility.
#--- x64/weak.s
.globl other
other:
  movq __imp_fin(%rip), %rax
  retq
.weak fin
.ifdef VIS
.link_fact fin, 1, VIS
.endif

#--- def.s
.globl fin
fin:
  ret

#--- arm64/strong.s
.globl main
main:
  adrp x16, __imp_fin
  ldr x16, [x16, :lo12:__imp_fin]
  blr x16
  ret

#--- arm64/weak.s
.globl other
other:
  adrp x0, __imp_fin
  ldr x0, [x0, :lo12:__imp_fin]
  ret
.weak fin
.ifdef VIS
.link_fact fin, 1, VIS
.endif

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
LdrGetProcedureAddressForCaller
NtProtectVirtualMemory
RtlExitUserProcess

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

#--- u.def
LIBRARY dé.dll
EXPORTS
fin
