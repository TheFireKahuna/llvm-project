// On Windows Itanium and NT-POSIX, X's import pointer __imp_X is its GOT entry,
// which X@GOTPCREL (typed as the compiler's own loads) and :got:X/:got_lo12:X
// reach. Other COFF targets have no relocation for them, nor does a GOT
// spelling with an offset.

// RUN: rm -rf %t && split-file %s %t && cd %t
// RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium x64.s -o x64.o
// RUN: llvm-objdump -dr x64.o | FileCheck %s --check-prefix=X64
// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-windows-ntposix x64.s -o x64n.o
// RUN: llvm-objdump -dr x64n.o | FileCheck %s --check-prefix=X64
// RUN: %if aarch64-registered-target %{ llvm-mc -filetype=obj -triple=aarch64-unknown-windows-itanium a64.s -o a64.o %}
// RUN: %if aarch64-registered-target %{ llvm-objdump -dr a64.o | FileCheck %s --check-prefix=A64 %}
// RUN: %if aarch64-registered-target %{ llvm-mc -filetype=obj -triple=aarch64-pc-windows-ntposix a64.s -o a64n.o %}
// RUN: %if aarch64-registered-target %{ llvm-objdump -dr a64n.o | FileCheck %s --check-prefix=A64 %}
// RUN: not llvm-mc -filetype=obj -triple=x86_64-pc-windows-msvc x64.s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=X64-ERR
// RUN: not llvm-mc -filetype=obj -triple=x86_64-unknown-windows-itanium \
// RUN:   offset.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=X64-ERR
// RUN: %if aarch64-registered-target %{ not llvm-mc -filetype=obj -triple=aarch64-pc-windows-msvc a64.s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=A64-ERR %}

// X64:      movq (%rip), %rax
// X64-NEXT:   IMAGE_REL_LLVM_MEM_LOAD __imp_v
// X64-NEXT: callq *(%rip)
// X64-NEXT:   IMAGE_REL_LLVM_MEM_CALL __imp_f
// X64-NEXT: jmpq *(%rip)
// X64-NEXT:   IMAGE_REL_LLVM_MEM_JUMP_TAIL __imp_f
// X64-NEXT: movq (%rip), %r20
// X64-NEXT:   IMAGE_REL_LLVM_MEM_LOAD_REX2 __imp_v

// A64:      adrp x8, 0x0
// A64-NEXT:   IMAGE_REL_ARM64_PAGEBASE_REL21 __imp_v
// A64-NEXT: ldr x8, [x8]
// A64-NEXT:   IMAGE_REL_ARM64_PAGEOFFSET_12L __imp_v

// X64-ERR: error: relocation specifier @GOTPCREL unsupported on COFF targets
// A64-ERR: error: relocation specifier :got: unsupported on COFF targets

//--- x64.s
  movq v@GOTPCREL(%rip), %rax
  callq *f@GOTPCREL(%rip)
  jmpq *f@GOTPCREL(%rip)
  movq v@GOTPCREL(%rip), %r20

//--- offset.s
  movq v@GOTPCREL+8(%rip), %rax

//--- a64.s
  adrp x8, :got:v
  ldr x8, [x8, :got_lo12:v]
