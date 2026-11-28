# REQUIRES: x86

## X$tp is the entry of X that is passed its thread pointer. A fork object's
## reference to it follows X: it binds the X$tp that X's provider defines,
## another image's by import, and X itself where the provider has none, its
## pointer __imp_X$tp too; an image's own X is what it names even where a DLL
## offers X$tp. An export of
## X carries the X$tp defined with it. A wrap of X renames the references to
## both: X$tp to __wrap_X$tp, which is __wrap_X where the wrapper has no such
## entry, and __real_X$tp to X$tp.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=x86_64-windows-ntposix
# RUN: %{mc} prov.s -o prov.obj && %{mc} use.s -o use.obj
# RUN: %{mc} local.s -o local.obj && %{mc} missing.s -o missing.obj
# RUN: lld-link -dll -noentry prov.obj -export:both -export:plain_only \
# RUN:   -export:shadow -out:prov.dll -implib:prov.lib
# RUN: llvm-readobj --coff-exports prov.dll | FileCheck --check-prefix=EXPORTS %s

# EXPORTS:     Name: both
# EXPORTS:     Name: both$tp
# EXPORTS:     Name: plain_only
# EXPORTS-NOT: Name: plain_only$tp
# EXPORTS:     Name: shadow
# EXPORTS:     Name: shadow$tp

# RUN: lld-link -dll -noentry use.obj local.obj prov.lib -out:use.dll
# RUN: llvm-readobj --coff-imports use.dll | FileCheck --check-prefix=IMPORTS %s

# IMPORTS:     Symbol: both$tp
# IMPORTS:     Symbol: plain_only
# IMPORTS-NOT: Symbol: shadow

# RUN: not lld-link -dll -noentry missing.obj prov.lib -out:missing.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=MISSING %s

# MISSING: undefined symbol: missing$tp

# RUN: %{mc} wrap-use.s -o wrap-use.obj && %{mc} both.s -o both.obj
# RUN: %{mc} plain.s -o plain.obj
# RUN: lld-link -dll -noentry wrap-use.obj both.obj -wrap:f -out:both.dll
# RUN: llvm-objdump -d both.dll | FileCheck --check-prefix=BOTH %s
# RUN: lld-link -dll -noentry wrap-use.obj plain.obj -wrap:f -out:plain.dll
# RUN: llvm-objdump -d plain.dll | FileCheck --check-prefix=PLAIN %s

## The caller that passes its thread pointer reaches the wrapper's entry that
## takes it, and each of the wrapper's entries the function's own.
# BOTH:      180001000: {{.*}} movl $0x1, %eax
# BOTH:      180001006: {{.*}} movl $0x2, %eax
# BOTH:      18000100c: {{.*}} movl $0x3, %eax
# BOTH-NEXT: jmp 0x180001000
# BOTH:      180001013: {{.*}} movl $0x4, %eax
# BOTH-NEXT: jmp 0x180001006
# BOTH:      callq 0x180001013
# BOTH-NEXT: callq 0x18000100c

## A wrapper with the one entry takes both.
# PLAIN:      180001000: {{.*}} movl $0x3, %eax
# PLAIN:      callq 0x180001000
# PLAIN-NEXT: callq 0x180001000

#--- prov.s
  .text
  .globl both, "both$tp", plain_only, shadow, "shadow$tp"
both:
"both$tp":
plain_only:
shadow:
"shadow$tp":
  ret

#--- use.s
  .text
  .globl use
use:
  callq "both$tp"
  callq "plain_only$tp"
  callq "local$tp"
  callq *"__imp_byptr$tp"(%rip)
  callq "shadow$tp"
  ret

#--- local.s
  .text
  .globl local, shadow, byptr
local:
shadow:
byptr:
  ret

#--- missing.s
  .text
  .globl use
use:
  callq "missing$tp"
  ret

#--- wrap-use.s
  .text
  .globl f, "f$tp"
f:
  movl $1, %eax
  ret
"f$tp":
  movl $2, %eax
  ret
  .section .text$use,"xr"
  .globl use
use:
  callq "f$tp"
  callq f
  ret

#--- both.s
  .text
  .globl __wrap_f, "__wrap_f$tp"
__wrap_f:
  movl $3, %eax
  jmp __real_f
"__wrap_f$tp":
  movl $4, %eax
  jmp "__real_f$tp"

#--- plain.s
  .text
  .globl __wrap_f
__wrap_f:
  movl $3, %eax
  jmp __real_f
