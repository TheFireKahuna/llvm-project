# REQUIRES: x86, aarch64

## A local import pointer's target is a call target, kept pointer or not, as
## is a bound .refptr cell's; a function only called through its pointer is a
## DLL export alone, suppressed.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{triple} = x86_64
# DEFINE: %{use} = x64.s
# DEFINE: %{build} = llvm-mc -filetype=obj -triple=%{triple}-windows-itanium \
# DEFINE:     -I %S/Inputs defs.s -o defs.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{triple}-windows-itanium %{use} \
# DEFINE:     -o use.obj && \
# DEFINE:   llvm-mc -filetype=obj -triple=%{triple}-windows-gnu cell.s \
# DEFINE:     -o cell.obj && \
# DEFINE:   lld-link -dll -noentry -guard:cf -out:use.dll -map:use.map \
# DEFINE:     defs.obj use.obj cell.obj -export:taken -export:kept \
# DEFINE:     -export:called -export:celled && \
# DEFINE:   llvm-readobj --coff-load-config use.dll
# DEFINE: %{map} = FileCheck %s --check-prefix=MAP --input-file=use.map \
# DEFINE:   --implicit-check-not=__imp_taken --implicit-check-not=.refptr

# RUN: %{build} | FileCheck %s
# RUN: %{map}
# REDEFINE: %{triple} = aarch64
# REDEFINE: %{use} = a64.s
# RUN: %{build} | FileCheck %s
# RUN: %{map}

# CHECK:      GuardFidTable [
# CHECK-NEXT:   0x180001000{{$}}
# CHECK-NEXT:   0x180001010{{$}}
# CHECK-NEXT:   0x180001020 flags 2
# CHECK-NEXT:   0x180001030{{$}}
# CHECK-NEXT: ]

## Of the pointers whose address is taken, the image holds only kept's.
# MAP: __imp_kept

#--- defs.s
.macro fn name
  .def \name; .scl 2; .type 32; .endef
  .globl \name
  .p2align 4
\name:
  ret
.endm
fn taken
fn kept
fn called
fn celled

.include "loadconfig-cfg-iat.s"

#--- x64.s
.globl use
use:
  movq __imp_taken(%rip), %rax
  movq __imp_kept(%rip), %rcx
  movq .refptr.celled(%rip), %rdx
  callq *__imp_called(%rip)
  ret
.data
.quad __imp_kept

#--- a64.s
.globl use
use:
  adrp x0, __imp_taken
  ldr x0, [x0, :lo12:__imp_taken]
  adrp x1, __imp_kept
  ldr x1, [x1, :lo12:__imp_kept]
  adrp x2, .refptr.celled
  ldr x2, [x2, :lo12:.refptr.celled]
  adrp x8, __imp_called
  ldr x8, [x8, :fused_lo12:__imp_called]
  .reloc ., IMAGE_REL_LLVM_FUSED_CALL, __imp_called
  blr x8
  ret
.data
.xword __imp_kept

#--- cell.s
.section .rdata$.refptr.celled,"dr",discard,.refptr.celled
.globl .refptr.celled
.refptr.celled:
  .quad celled
