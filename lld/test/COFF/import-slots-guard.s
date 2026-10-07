# REQUIRES: x86
## An in-place import slot holding a function's address is listed in the
## address-taken import address table, and an import used only through slots
## has no other entry; the import thunk taken as an address is not listed. A
## function of a delay-loaded DLL keeps its thunk, with a base relocation, and
## gets no descriptor.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc main.s -o main.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -def:b.def -out:b.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc delay.s -o delay.obj

# RUN: lld-link -import-slots -guard:cf -entry:main -subsystem:console \
# RUN:   main.obj a.lib -out:guard.exe
# RUN: llvm-readobj --coff-imports --coff-load-config guard.exe | \
# RUN:   FileCheck --check-prefix=GUARD %s

# GUARD:      Name: a.dll
# GUARD-NEXT: ImportLookupTableRVA:
# GUARD-NEXT: ImportAddressTableRVA: 0x3000
# GUARD-NEXT: Symbol: func1 (0)
# GUARD-NEXT: Symbol: var1 (1)
# GUARD-NEXT: }
# GUARD-NOT:  Name: a.dll
# GUARD:      GuardCFFunctionCount: 0
# GUARD:      GuardAddressTakenIatEntryCount: 1
# GUARD:      GuardIatTable [
# GUARD-NEXT:   0x140003000
# GUARD-NEXT: ]

# RUN: lld-link -import-slots -entry:start -subsystem:console delay.obj \
# RUN:   b.lib -delayload:b.dll -out:delay.exe
# RUN: llvm-readobj --coff-imports --coff-basereloc delay.exe | \
# RUN:   FileCheck --check-prefix=DELAY %s
# RUN: llvm-objdump -d delay.exe | FileCheck --check-prefix=THUNK %s
# RUN: llvm-objdump -s -j .data delay.exe | FileCheck --check-prefix=WORD %s

# DELAY-NOT:  ImportLookupTableRVA
# DELAY:      DelayImport {
# DELAY-NEXT:   Name: b.dll
# DELAY:      BaseReloc [
# DELAY:          Address: 0x3000

## The word holds the thunk's address.
# THUNK:      140001010: ff 25 {{.*}} jmpq *0x3fea(%rip)
# WORD:      Contents of section .data:
# WORD-NEXT: 140003000 10100040 01000000

#--- a.def
LIBRARY a.dll
EXPORTS
  func1
  var1 DATA

#--- b.def
LIBRARY b.dll
EXPORTS
  func2

#--- main.s
  .text
  .globl main
main:
  retq

  .data
  .globl slots
slots:
  .quad func1
  .quad var1

  .section .rdata,"dr"
  .p2align 3
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

#--- delay.s
  .text
  .globl start
start:
  retq
  .globl __delayLoadHelper2
__delayLoadHelper2:
  retq

  .data
  .quad func2
