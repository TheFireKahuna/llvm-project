# REQUIRES: x86

# A reference to the import pointer of a symbol that is defined in the image
# is rewritten to the direct instruction of the same length: the load of the
# pointer becomes a lea, the indirect call an addr32-prefixed direct call, and
# the indirect jump a direct jump followed by a nop. No pointer is emitted for
# a symbol whose references were all rewritten, so there is nothing to warn
# about; a reference in any other form keeps the pointer and the warning.

# RUN: split-file %s %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/def.s -o %t.def.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/code.s -o %t.code.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %t.dir/addr.s -o %t.addr.obj

# RUN: lld-link -entry:main -subsystem:console -debug:symtab -opt:ref -out:%t.exe %t.code.obj %t.def.obj 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
# RUN: llvm-objdump -d --no-show-raw-insn %t.exe | FileCheck --check-prefix=DISASM %s
# RUN: llvm-objdump -d %t.exe | FileCheck --check-prefix=BYTES %s
# RUN: llvm-readobj --sections --coff-basereloc %t.exe | FileCheck --check-prefix=NOPTR %s

# QUIET-NOT: locally defined symbol imported

# DISASM:      <main>:
# DISASM-NEXT:   addr32 callq 0x{{[0-9a-f]+}} <f>
# DISASM-NEXT:   jmp 0x{{[0-9a-f]+}} <f>
# DISASM-NEXT:   nop
# DISASM-NEXT:   leaq 0x{{[0-9a-f]+}}(%rip), %rax
# DISASM-NEXT:   leaq 0x{{[0-9a-f]+}}(%rip), %r9
# DISASM-NEXT:   movq (%rax), %rax
# DISASM-NEXT:   retq
# DISASM:      <f>:
# DISASM-NEXT:   retq

# BYTES:      <main>:
# BYTES-NEXT:   67 e8 {{.*}} addr32 callq
# BYTES-NEXT:   e9 {{.*}} jmp
# BYTES-NEXT:   90{{ +}}nop
# BYTES-NEXT:   48 8d 05 {{.*}} leaq
# BYTES-NEXT:   4c 8d 0d {{.*}} leaq

# The function reached only through its import pointer survives -opt:ref, and
# no pointer is emitted: no .rdata, no base relocation.
# NOPTR-NOT: Name: .rdata
# NOPTR:      BaseReloc [
# NOPTR-NEXT: ]

# Taking the address of the pointer itself needs the pointer.
# RUN: lld-link -entry:main -subsystem:console -debug:symtab -out:%t2.exe %t.addr.obj %t.def.obj 2>&1 | FileCheck --check-prefix=WARN %s
# RUN: llvm-objdump -d --no-show-raw-insn %t2.exe | FileCheck --check-prefix=ADDR %s
# RUN: llvm-readobj --coff-basereloc %t2.exe | FileCheck --check-prefix=PTR %s

# WARN: warning: {{.*}}addr.obj: locally defined symbol imported: f (defined in {{.*}}def.obj) [LNK4217]
# WARN-NOT: locally defined symbol imported

# ADDR:      <main>:
# ADDR-NEXT:   addr32 callq 0x{{[0-9a-f]+}} <f>
# ADDR-NEXT:   leaq 0x{{[0-9a-f]+}}(%rip), %rax
# ADDR-NEXT:   retq

# PTR:      BaseReloc [
# PTR-NEXT:   Entry {
# PTR-NEXT:     Type: DIR64

#--- def.s
.section .text$f,"xr",one_only,f
.globl f
f:
  ret
.data
.globl v
v:
  .quad 1

#--- code.s
.text
.globl main
main:
  call *__imp_f(%rip)
  jmp *__imp_f(%rip)
  movq __imp_v(%rip), %rax
  movq __imp_v(%rip), %r9
  movq (%rax), %rax
  ret

#--- addr.s
.text
.globl main
main:
  call *__imp_f(%rip)
  leaq __imp_f(%rip), %rax
  ret
