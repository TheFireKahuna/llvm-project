# REQUIRES: x86
## Under -import-slots, where a load of the import pointer of data that a
## DLL's instance preempts stays, since its register's address is stored, an
## access through the register that the object describes as one that only
## reads reads the image's own copy of the data directly, in code the linker
## may resize, while the load and the store keep reaching the instance.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -import-slots -entry:main -subsystem:console main.obj lib.lib \
# RUN:   -out:main.exe -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d --print-imm-hex main.exe > main.txt
# RUN: llvm-objdump -s -j .rdata main.exe >> main.txt
# RUN: FileCheck --input-file=main.txt %s
# RUN: llvm-readobj --coff-imports main.exe | FileCheck --check-prefix=IMPORTS %s

# LOG: Keeping the copy of tbl in main.obj for 1 value read

# CHECK:      <.text>:
# CHECK-NEXT: 48 8b 05 {{.*}} movq {{.*}}(%rip), %rax
# CHECK-NEXT: 48 89 01 movq %rax, (%rcx)
# CHECK-NEXT: 8b 05 {{.*}} movl {{.*}}(%rip), %eax # 0x[[#%x,TBL:]]
# CHECK-NEXT: c3 retq
# CHECK:      Contents of section .rdata:
# CHECK:      [[#%x,TBL - 8]] 01000000 02000000 03000000 04000000

# IMPORTS:      Name: lib.dll
# IMPORTS:      Symbol: tbl
# IMPORTS-NEXT: }

#--- lib.def
LIBRARY lib.dll
EXPORTS
  tbl DATA

#--- main.s
  .section .rdata,"dr",discard,tbl
  .globl tbl
  .p2align 4
tbl:
  .long 1, 2, 3, 4

  .text
  .globl main
main:
.Lr0:
  movq __imp_tbl(%rip), %rax
  movq %rax, (%rcx)
  .linkfusedaccess .Lr0, %rax
  .linkvalueonly
  movl 8(%rax), %eax
  retq
