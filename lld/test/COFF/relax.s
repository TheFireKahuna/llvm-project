# REQUIRES: x86

## In code whose object describes every value that depends on where it lies,
## the linker changes the size of the instructions the object's sites
## describe: a call or jump through the import pointer of a function in the
## image becomes the five-byte direct one, a load whose register only fused
## accesses read goes, each access reaching the data RIP-relative, and a load
## takes the constant a fused add adds, the add going. It lays the code out
## again, redoing alignment, lengthening a jump whose 8-bit displacement no
## longer reaches, and writing every value the object describes from the new
## layout, in the section and in others. Code where a value would no longer
## fit its field keeps its size.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium main.s -o main.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium dll.s -o dll.obj
# RUN: lld-link -dll -noentry dll.obj -out:d.dll
# RUN: lld-link -entry:main -subsystem:console -out:a.exe main.obj d.lib \
# RUN:   -verbose 2>&1 | FileCheck --check-prefix=LOG %s
# RUN: llvm-objdump -d a.exe | FileCheck %s
# RUN: llvm-objdump -s -j .rdata a.exe | FileCheck --check-prefix=DATA %s
# RUN: llvm-readobj --unwind a.exe | FileCheck --check-prefix=UNWIND %s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium vec.s -o vec.obj
# RUN: lld-link -entry:main -subsystem:console -out:v.exe main.obj vec.obj \
# RUN:   d.lib
# RUN: llvm-objdump -d v.exe | FileCheck --check-prefix=VEC %s

# LOG: Fused accesses and adds: 4 of 4 direct
# LOG: Relaxed 1 of 2 covered sections

## The call to local is direct, so the branch over the calls is shorter.
# CHECK-LABEL: <.text>:
# CHECK-NEXT:  48 83 ec 28 subq $0x28, %rsp
# CHECK-NEXT:  85 c9 testl %ecx, %ecx
# CHECK-NEXT:  74 0b je 0x[[#%x,SKIP:]]
# CHECK-NEXT:  e8 {{.*}} callq 0x[[#%x,LOCAL:]]
# CHECK-NEXT:  ff 15 {{.*}} callq *{{.*}}(%rip)
## The loads are gone; the access and the add reach data directly.
# CHECK-NEXT:  [[#SKIP]]: 8b 0d {{.*}} movl {{.*}}(%rip), %ecx # 0x[[#%x,DATA8:]]
# CHECK-NEXT:  48 8d 15 {{.*}} leaq {{.*}}(%rip), %rdx # 0x[[#DATA8+8]]
# CHECK-NEXT:  48 89 d0 movq %rdx, %rax
## The loop is aligned again.
# CHECK-NEXT:  nopw
# CHECK-NEXT:  [[#%x,LOOP:]]: ff c9 decl %ecx
# CHECK-NEXT:  75 fc jne 0x[[#LOOP]]
## The access between the jump and its target is longer, so the jump takes a
## 32-bit displacement.
# CHECK-NEXT:  e9 {{.*}} jmp 0x[[#%x,FAR:]]
# CHECK-NEXT:  8b 0d {{.*}} movl {{.*}}(%rip), %ecx # 0x[[#DATA8-8]]
# CHECK:       [[#FAR]]: 48 83 c4 28 addq $0x28, %rsp
# CHECK-NEXT:  c3 retq
# CHECK-NEXT:  [[#LOCAL]]: c3 retq
## A byte in .rdata holds a distance that the longer access in fallback would
## take past 255, so its section keeps its size and its forms.
# CHECK-NEXT:  67 e8 {{.*}} addr32 callq 0x[[#LOCAL]]
# CHECK-NEXT:  48 8d 05 {{.*}} leaq {{.*}}(%rip), %rax # 0x[[#DATA8-8]]
# CHECK-NEXT:  8b 08 movl (%rax), %ecx

## The differences of places in .text, one through a relocation, and the
## LEB128 one, follow the new layout; the byte for fallback does not change.
# DATA:      140002000 30000000 30f0ffff bb01fe00

# UNWIND:      StartAddress: (0x140001000)
# UNWIND-NEXT: EndAddress: (0x1400010C0)

## VEX and EVEX accesses reach data directly too: the base register's high
## bit, EVEX's inverted B here, is cleared, and EVEX's 8-bit displacement,
## 1 scaled by 64, becomes 64. An access next to a load that an indexed access
## keeps reaches data directly, and the load computes data's address.
# VEC:      vmovups {{.*}}(%rip), %ymm0 # 0x[[#%x,VDATA:]]
# VEC-NEXT: 62 f1 7c 48 10 0d {{.*}} vmovups {{.*}}(%rip), %zmm1 # 0x[[#VDATA+64]]
# VEC-NEXT: 48 8d 0d {{.*}} leaq {{.*}}(%rip), %rcx # 0x[[#VDATA]]
# VEC-NEXT: 8b 05 {{.*}} movl {{.*}}(%rip), %eax # 0x[[#VDATA+4]]
# VEC-NEXT: 8b 14 91 movl (%rcx,%rdx,4), %edx

#--- main.s
  .text
  .def main; .scl 2; .type 32; .endef
  .globl main
main:
.seh_proc main
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  testl %ecx, %ecx
  je .Lskip
  callq *__imp_local(%rip)
  callq *__imp_ext(%rip)
.Lskip:
.Lload:
  .linkfusedread
  movq __imp_data(%rip), %rax
  .linkfusedaccess .Lload, %rax
  movl 8(%rax), %ecx
.Lload2:
  movq __imp_data(%rip), %rdx
  .linkfusedadd .Lload2, %rdx
  addq $16, %rdx
  movq %rdx, %rax
  .p2align 4
.Lloop:
  decl %ecx
  jne .Lloop
.Lfar0:
.Lload3:
  .linkfusedread
  movq __imp_data(%rip), %rsi
  jmp .Lfar
  .linkfusedaccess .Lload3, %rsi
  movl (%rsi), %ecx
  .fill 124, 1, 0x90
.Lfar:
  addq $40, %rsp
  retq
.seh_endproc

  .def local; .scl 2; .type 32; .endef
  .globl local
local:
  retq

  .section .text$fb,"xr"
  .def fallback; .scl 2; .type 32; .endef
  .globl fallback
fallback:
  callq *__imp_local(%rip)
.Lfb:
  .linkfusedread
  movq __imp_data(%rip), %rax
.Lfbstart:
  .linkfusedaccess .Lfb, %rax
  movl (%rax), %ecx
  .fill 252, 1, 0x90
.Lfbend:
  retq

  .data
  .globl data
data:
  .zero 128

  .section .rdata,"dr"
.Ltable:
  .long .Lloop - main
  .long .Lloop - .Ltable
  .uleb128 .Lfar - main
  .byte .Lfbend - .Lfbstart

#--- dll.s
  .text
  .globl ext
ext:
  retq
  .section .drectve,"yni"
  .ascii " -export:ext"

#--- vec.s
  .section .text$v,"xr"
  .globl vecs
vecs:
.Lv:
  .linkfusedread
  movq __imp_data(%rip), %rax
  .linkfusedaccess .Lv, %rax
  vmovups (%rax), %ymm0
.Lv2:
  .linkfusedread
  movq __imp_data(%rip), %r9
  .linkfusedaccess .Lv2, %r9
  vmovups 64(%r9), %zmm1
.Lp:
  movq __imp_data(%rip), %rcx
  .linkfusedaccess .Lp, %rcx
  movl 4(%rcx), %eax
  movl (%rcx,%rdx,4), %edx
  retq
