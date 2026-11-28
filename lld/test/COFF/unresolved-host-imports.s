# REQUIRES: x86

## Under -unresolved-host-imports, as under ELF's -shared, a DLL imports each
## symbol that nothing in its link defines from the executable that loads it,
## by name: a function where an object refers to one, with a thunk, and data
## otherwise, which a word that names it directly reads through its pointer.
## No import descriptor names the executable. An executable's link ignores the
## option, and --no-undefined or -z defs after it restores the error; the
## last of the three decides. /force:unresolved is unchanged.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium p.s -o p.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium --defsym x64=1 rt.s \
# RUN:   -o rt.obj
# RUN: llvm-ar rcs rt.lib rt.obj
# RUN: lld-link -machine:x64 -def:ntdll.def -out:ntdll.lib
# RUN: lld-link -dll -noentry -unresolved-host-imports p.obj ntdll.lib rt.lib \
# RUN:   -out:p.dll
# RUN: llvm-readobj --coff-imports p.dll > p.txt
# RUN: llvm-objdump -d --no-show-raw-insn -s -j .text -j .rdata -j .data \
# RUN:   p.dll >> p.txt
# RUN: FileCheck %s < p.txt --implicit-check-not="Name: "

## The list's four imports, each with the executable's 0: hcall, a function
## with a thunk, and hfn, hdata and hword, data; the word of .data is hword's.
# CHECK:      Name: ntdll.dll
# CHECK:      Contents of section .rdata:
# CHECK:      180002010 00000000 00000000 05000000 00000000
# CHECK-NEXT: 180002020 00300000 28000000 00000000 00000000
# CHECK-NEXT: 180002030 08300000 03000000 80200000 00000000
# CHECK-NEXT: 180002040 10300000 03000000 86200000 00000000
# CHECK-NEXT: 180002050 18300000 03000000 8a200000 00000000
# CHECK-NEXT: 180002060 20300000 03000000 90200000 00000000
# CHECK-NEXT: 180002070 00400000 20300000 00000000 00000000
# CHECK-NEXT: 180002080 6863616c 6c006866 6e006864 61746100
# CHECK-NEXT: 180002090 68776f72 6400
# CHECK:      Contents of section .data:
# CHECK-NEXT: 180004000 f1f1f1f1 f1f1f1f1
# CHECK:      callq *0x200a(%rip)  # 0x180003010
# CHECK-NEXT: movq 0x200b(%rip), %rax  # 0x180003018
# CHECK-NEXT: callq 0x180001030
# CHECK:      180001030: jmpq *0x1fd2(%rip)  # 0x180003008

## A weak reference to what nothing defines is a weak import from the
## executable, 1 in the list, which is null where the executable lacks it; a
## word that names it directly is written from its pointer. A hidden one is
## no import and keeps its null default.
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium w.s -o w.obj
# RUN: lld-link -dll -noentry -unresolved-host-imports w.obj ntdll.lib rt.lib \
# RUN:   -out:w.dll
# RUN: llvm-objdump -d --no-show-raw-insn -s -j .text -j .rdata w.dll | \
# RUN:   FileCheck --check-prefix=WEAK %s
# WEAK:      Contents of section .rdata:
# WEAK:      180002010 00000000 00000000 03000000 00000000
# WEAK-NEXT: 180002020 00300000 18000000 00000000 00000000
# WEAK-NEXT: 180002030 08300000 03000000 60200000 01000000
# WEAK-NEXT: 180002040 10300000 03000000 66200000 01000000
# WEAK-NEXT: 180002050 00400000 10300000 00000000 00000000
# WEAK-NEXT: 180002060 68776561 6b006877 65616b66 6e00
# WEAK:      movq 0x2001(%rip), %rax  # 0x180003008
# WEAK-NEXT: movq 0x2002(%rip), %rcx  # 0x180003010
# WEAK-NEXT: movq $0x0, %rdx

# RUN: lld-link -dll -noentry -z defs -unresolved-host-imports -z now p.obj \
# RUN:   ntdll.lib rt.lib -out:p.dll 2>&1 | FileCheck --check-prefix=Z %s
# Z: warning: unknown -z value: now

# RUN: not lld-link -entry:f -subsystem:console -unresolved-host-imports \
# RUN:   p.obj ntdll.lib rt.lib -out:p.exe 2>&1 | FileCheck --check-prefix=UNDEF %s
# RUN: not lld-link -dll -noentry -unresolved-host-imports --no-undefined \
# RUN:   p.obj ntdll.lib rt.lib -out:p.dll 2>&1 | FileCheck --check-prefix=UNDEF %s
# RUN: not lld-link -dll -noentry -unresolved-host-imports -z defs p.obj \
# RUN:   ntdll.lib rt.lib -out:p.dll 2>&1 | FileCheck --check-prefix=UNDEF %s
# UNDEF: error: undefined symbol: hcall

# RUN: lld-link -dll -noentry -force:unresolved p.obj ntdll.lib rt.lib \
# RUN:   -out:p.dll 2>&1 | FileCheck --check-prefix=FORCE %s
# FORCE: warning: undefined symbol: hcall

#--- p.s
.def hcall
.scl 2
.type 32
.endef
.text
.globl f
f:
  callq *__imp_hfn(%rip)
  movq __imp_hdata(%rip), %rax
  callq hcall
  retq
.data
  .quad hword
.section .drectve,"yn"
  .ascii " -include:f"

#--- w.s
.text
.globl g
g:
  movq __imp_hweak(%rip), %rax
  movq __imp_hweakfn(%rip), %rcx
  movq __imp_hhid(%rip), %rdx
  retq
.weak hweak
.weak hweakfn
.weak hhid
.link_fact hhid, 1, 3
.data
  .quad hweakfn
.section .drectve,"yn"
  .ascii " -include:g"

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
