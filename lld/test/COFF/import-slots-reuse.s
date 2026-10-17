# REQUIRES: x86
## Under -import-slots, an import that the image keeps in in-place slots gets
## no entry in its DLL's import address table unless something needs one: its
## address is taken to be one of its slots, which code that loads, calls or
## jumps through the address in an instruction its object describes reads
## instead, as does the import thunk and the load that an address-take
## becomes. The slot must be naturally aligned and read-only, in a section
## that no /merge changes. An instruction that takes the entry's address, a
## reference from data, an object that does not describe its instructions and
## a root each keep the entry. An import nothing reads may be kept in a
## writable slot.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc undesc.s -o undesc.obj
# RUN: lld-link -def:a.def -out:a.lib -machine:x64
# RUN: lld-link -import-slots -opt:noref -entry:main -subsystem:console \
# RUN:   main.obj undesc.obj a.lib -merge:merged=.data -include:__imp_root \
# RUN:   -debug:symtab -out:main.exe
# RUN: llvm-readobj --coff-imports main.exe | \
# RUN:   FileCheck --check-prefix=IMPORTS %s
# RUN: llvm-nm -n main.exe > out.txt
# RUN: llvm-objdump -d --no-show-raw-insn main.exe >> out.txt
# RUN: FileCheck %s < out.txt

## The DLL's own descriptor lists only the imports that keep their entries.
# IMPORTS:      Name: a.dll
# IMPORTS-NEXT: ImportLookupTableRVA:
# IMPORTS-NEXT: ImportAddressTableRVA:
# IMPORTS-NEXT: Symbol: data (2)
# IMPORTS-NEXT: Symbol: lea (4)
# IMPORTS-NEXT: Symbol: merged (6)
# IMPORTS-NEXT: Symbol: mis (7)
# IMPORTS-NEXT: Symbol: root (8)
# IMPORTS-NEXT: Symbol: rw (9)
# IMPORTS-NEXT: Symbol: undesc (11)
# IMPORTS-NEXT: }

## The others are kept in their read-only slots, unused_rw in its writable one.
# CHECK:      [[#%x,RO:]] R __imp_load
# CHECK-NEXT: [[#RO + 8]] R __imp_call
# CHECK-NEXT: [[#RO + 16]] R __imp_thunk
# CHECK-NEXT: [[#RO + 24]] R __imp_addr
# CHECK-NEXT: [[#RO + 32]] R __imp_jmp
# CHECK-NEXT: [[#%x,RW:]] D wdata
# CHECK-NEXT: [[#RW + 8]] D __imp_unused_rw

# CHECK:      <main>:
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#RO]]
# CHECK-NEXT: callq *{{.*}}(%rip) # 0x[[#RO + 8]]
# CHECK-NEXT: callq 0x[[#%x,THUNK:]]
# CHECK-NEXT: movq {{.*}}(%rip), %rax # 0x[[#RO + 24]]
# CHECK-NEXT: leaq {{.*}}(%rip), %rax
# CHECK-NEXT: movq {{.*}}(%rip), %rax
# CHECK-NEXT: movq {{.*}}(%rip), %rax
# CHECK-NEXT: movq {{.*}}(%rip), %rax
# CHECK-NEXT: movq {{.*}}(%rip), %rax
# CHECK-NEXT: jmpq *{{.*}}(%rip) # 0x[[#RO + 32]]
# CHECK:      [[#THUNK]]: jmpq *{{.*}}(%rip) # 0x[[#RO + 16]]

#--- a.def
LIBRARY a.dll
EXPORTS
  addr
  call
  data
  jmp
  lea
  load
  merged
  mis
  root
  rw
  thunk
  undesc
  unused_rw

#--- main.s
  .text
  .globl main
main:
  movq __imp_load(%rip), %rax
  callq *__imp_call(%rip)
  callq thunk
  leaq addr(%rip), %rax
  leaq __imp_lea(%rip), %rax
  movq __imp_rw(%rip), %rax
  movq __imp_mis(%rip), %rax
  movq __imp_merged(%rip), %rax
  movq __imp_root(%rip), %rax
  jmpq *__imp_jmp(%rip)

  .data
  .globl wdata
wdata:
  .quad rw
  .quad unused_rw
  .quad __imp_data
  .quad data

  .section .rdata,"dr"
  .p2align 3
  .quad load
  .quad call
  .quad thunk
  .quad addr
  .quad jmp
  .quad lea
  .quad root
  .quad undesc

  .section mis,"dr"
  .p2align 2
  .long 0
  .quad mis

  .section merged,"dr"
  .p2align 3
  .quad merged

#--- undesc.s
  .text
  .globl other
other:
  movq __imp_undesc(%rip), %rax
  retq
