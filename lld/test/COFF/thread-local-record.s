# REQUIRES: x86
## A DLL exports a record of a thread-local variable, never the variable, and
## an image that reaches the variable by its section-relative offset cannot
## link against it: the undefined symbol's note names the record the import
## library provides, and the reference to a DLL that did export the variable
## cannot name a section of another image. A thread-local variable has an
## instance per image, so a link never offers its record or init function.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium lib.s -o lib.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium use.s -o use.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium vague.s -o vague.obj
# RUN: lld-link -dll -noentry lib.obj -export:'tls_var$tls',DATA -out:lib.dll
# RUN: lld-link -dll -noentry lib.obj -export:tls_var,DATA -out:old.dll

# RUN: not lld-link -import-slots -entry:main -subsystem:console use.obj \
# RUN:   lib.lib -out:use.exe 2>&1 | FileCheck --check-prefix=UNDEF %s
# UNDEF:      error: undefined symbol: tls_var
# UNDEF-NEXT: >>> referenced by use.obj:(main)
# UNDEF-NEXT: NOTE: lib.lib provides tls_var$tls, the record of thread-local variable tls_var; a declaration of tls_var with default visibility reaches it

# RUN: not lld-link -import-slots -entry:main -subsystem:console use.obj \
# RUN:   old.lib -out:use.exe 2>&1 | FileCheck --check-prefix=SECREL %s
# SECREL: error: use.obj: tls_var is imported from old.dll, but .text refers to it with relocation type IMAGE_REL_AMD64_SECREL, which names a section of this image

# RUN: lld-link -import-slots -dll -noentry vague.obj -out:vague.dll
# RUN: llvm-readobj --coff-exports vague.dll | FileCheck --check-prefix=OFFERS %s
# OFFERS-NOT: Name: _ZN1SIiE1xE$tls
# OFFERS-NOT: Name: _ZTHN1SIiE1xE
# OFFERS:     Name: _ZN1SIiE1yE
# OFFERS-NOT: Name: _ZN1SIiE1xE$tls
# OFFERS-NOT: Name: _ZTHN1SIiE1xE

#--- lib.s
  .section .tls$,"dw"
  .globl tls_var
tls_var:
  .long 1

  .section .rdata,"dr"
  .globl "tls_var$tls"
  .p2align 3
"tls_var$tls":
  .quad _tls_index
  .secrel32 tls_var
  .secrel32 tls_var

  .data
  .globl _tls_index
_tls_index:
  .long 0

#--- use.s
  .text
  .globl main
main:
  movl _tls_index(%rip), %eax
  movq %gs:88, %rcx
  movq (%rcx,%rax,8), %rax
  movl tls_var@SECREL32(%rax), %eax
  retq

  .data
_tls_index:
  .long 0

#--- vague.s
## An explicit instantiation of a template's thread-local member, beside one
## of an ordinary member.
  .linkbindings
  .section .tls$,"dw",discard,_ZN1SIiE1xE
  .globl _ZN1SIiE1xE
_ZN1SIiE1xE:
  .linkbinding _ZN1SIiE1xE, default
  .long 1

  .section .rdata,"dr",associative,_ZN1SIiE1xE
  .globl "_ZN1SIiE1xE$tls"
  .p2align 3
"_ZN1SIiE1xE$tls":
  .linkbinding "_ZN1SIiE1xE$tls", default
  .quad _tls_index
  .secrel32 _ZN1SIiE1xE
  .secrel32 _ZN1SIiE1xE

  .section .text,"xr",discard,_ZTHN1SIiE1xE
  .globl _ZTHN1SIiE1xE
_ZTHN1SIiE1xE:
  .linkbinding _ZTHN1SIiE1xE, default
  retq

  .section .data,"dw",discard,_ZN1SIiE1yE
  .globl _ZN1SIiE1yE
_ZN1SIiE1yE:
  .linkbinding _ZN1SIiE1yE, default
  .long 2

  .data
  .globl _tls_index
_tls_index:
  .long 0
  .globl keep
keep:
  .quad "_ZN1SIiE1xE$tls"
  .quad _ZTHN1SIiE1xE
  .quad _ZN1SIiE1yE
