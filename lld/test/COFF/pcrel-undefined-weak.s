# REQUIRES: x86, aarch64

## A PC-relative reference to an undefined weak (address zero) is an error, as
## LNK2016; in a fork object an ARM64 branch to it goes to the next instruction.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -out:out.exe

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc x64.s -o x64.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-gnu x64.s -o x64-gnu.obj
# RUN: not %{link} x64.obj 2>&1 | FileCheck --check-prefix=X64 %s
# RUN: not %{link} x64-gnu.obj 2>&1 | FileCheck --check-prefix=X64 %s
# RUN: %{link} x64.obj -force:unresolved 2>&1 | FileCheck --check-prefix=FORCE %s
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium x64.s -o x64-fork.obj
# RUN: not %{link} x64-fork.obj 2>&1 | FileCheck --check-prefix=X64-FORK %s

# X64:      error: x64{{(-gnu)?}}.obj: IMAGE_REL_AMD64_REL32 relocation against weakfn is out of range: it resolves to address 0, as a weak external nothing defines does; reference it through a pointer, or define it
# X64-NEXT: error: {{.*}}: IMAGE_REL_AMD64_REL32 relocation against weakvar is out of range
# X64-NEXT: error: {{.*}}: IMAGE_REL_AMD64_REL32 relocation against weakfn is out of range
# FORCE:    warning: x64.obj: IMAGE_REL_AMD64_REL32 relocation against weakfn is out of range
# X64-FORK:      error: x64-fork.obj: IMAGE_REL_LLVM_BRANCH32 relocation against weakfn is out of range: it resolves to address 0
# X64-FORK-NEXT: error: {{.*}}: IMAGE_REL_LLVM_MEM_LOAD relocation against weakvar is out of range
# X64-FORK-NEXT: error: {{.*}}: IMAGE_REL_LLVM_BRANCH_TAIL relocation against weakfn is out of range

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-msvc a64.s -o a64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-gnu a64.s -o a64-gnu.obj
# RUN: not %{link} a64.obj 2>&1 | FileCheck --check-prefix=A64 %s
# RUN: not %{link} a64-gnu.obj 2>&1 | FileCheck --check-prefix=A64 %s

# A64:      error: a64{{(-gnu)?}}.obj: IMAGE_REL_ARM64_BRANCH26 relocation against weakfn is out of range: it resolves to address 0
# A64-NEXT: error: {{.*}}: IMAGE_REL_ARM64_BRANCH26 relocation against weakfn is out of range
# A64-NEXT: error: {{.*}}: IMAGE_REL_ARM64_BRANCH19 relocation against weakfn is out of range
# A64-NEXT: error: {{.*}}: IMAGE_REL_ARM64_BRANCH14 relocation against weakfn is out of range
# A64-NOT:  error

# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64.s -o a64-fork.obj
# RUN: %{link} a64-fork.obj
# RUN: llvm-objdump -d --no-show-raw-insn out.exe | FileCheck --check-prefix=A64-FORK %s

# A64-FORK:      140001000: bl 0x140001004
# A64-FORK-NEXT: 140001004: b 0x140001008
# A64-FORK-NEXT: 140001008: b.eq 0x14000100c
# A64-FORK-NEXT: 14000100c: tbz w0, #0x0, 0x140001010
# A64-FORK-NEXT: 140001010: ret
# A64-FORK-NOT:  br

## Such a branch reaches from anywhere, so its section, out of reach of zero,
## resizes as any other: its last jump goes.
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a64-far.s -o a64-far.obj
# RUN: %{link} a64-far.obj
# RUN: llvm-objdump -d --no-show-raw-insn out.exe | FileCheck --check-prefix=FAR %s

# FAR:      140101004: b.eq 0x140101008
# FAR-NEXT: 140101008: tbz w0, #0x0, 0x14010100c
# FAR-NEXT: 14010100c: ret

#--- x64.s
  .globl main
main:
  call weakfn
  movq weakvar(%rip), %rax
  jmp weakfn
  .weak weakfn
  .weak weakvar

#--- a64.s
  .globl main
main:
  bl weakfn
  b weakfn
  b.eq weakfn
  tbz w0, #0, weakfn
  ret
  .weak weakfn

#--- a64-far.s
  .globl main
main:
  ret
  .section .text$a,"xr"
  .space 0x100000
  .section .text$b,"xr"
  b.eq weakfn
  tbz w0, #0, weakfn
  b next
  .section .text$c,"xr"
next:
  ret
  .weak weakfn
