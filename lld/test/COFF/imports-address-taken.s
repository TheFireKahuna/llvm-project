# REQUIRES: x86

# Taking the address of an imported function through its thunk, the form a
# compiler emits when it was not told that the function is imported, is
# rewritten to load the function's true address from the import address
# table, so that the pointer equals the one seen inside the DLL and the one a
# static initialiser receives. The entry is listed as address-taken for
# Control Flow Guard. A call keeps the thunk, and so does every reference to
# a delay-loaded DLL, whose entry holds the loader's stub until the first
# call.

# RUN: split-file %s %t.dir
# RUN: yaml2obj %p/Inputs/export.yaml -o %t.exp.obj
# RUN: lld-link -out:%t.exp.dll -dll %t.exp.obj -export:exportfn1 -implib:%t.exp.lib
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/main.s -o %t.main.obj

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -guard:cf -out:%t.exe %t.main.obj %t.exp.lib
# RUN: llvm-objdump -d %t.exe | FileCheck --check-prefix=DISASM %s
# RUN: llvm-readobj --coff-load-config --coff-imports %t.exe | FileCheck --check-prefix=CFG %s

# DISASM:      <main>:
# DISASM-NEXT:   48 8b 05 {{.*}} movq {{.*}}(%rip), %rax
# DISASM-NEXT:   e8 {{.*}} callq
# DISASM-NEXT:   c3{{ +}}retq

# CFG:      Import {
# CFG:        ImportAddressTableRVA: 0x[[IAT:[0-9A-F]+]]
# CFG:      GuardAddressTakenIatEntryCount: 1
# CFG:      GuardIatTable [
# CFG-NEXT:   0x14000[[IAT]]
# CFG-NEXT: ]

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -guard:cf -out:%t.delay.exe %t.main.obj %t.exp.lib -delayload:%basename_t.tmp.exp.dll -alternatename:__delayLoadHelper2=main
# RUN: llvm-objdump -d %t.delay.exe | FileCheck --check-prefix=DELAY %s

# DELAY:      <main>:
# DELAY-NEXT:   48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax
# DELAY-NEXT:   e8 {{.*}} callq

#--- main.s
.text
.globl main
main:
  leaq exportfn1(%rip), %rax
  call exportfn1
  ret

.section .gfids$y,"dr"
.section .giats$y,"dr"
.section .gljmp$y,"dr"

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
