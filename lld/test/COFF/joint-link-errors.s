# REQUIRES: x86
## The diagnostics of a joint link: members whose link lines differ in an
## option of the whole program, but not in one that changes nothing written; a
## member given twice; an entity that no member can keep for the others without
## an initialization cycle; link lines that name each other's import libraries;
## a final link whose inputs changed since its scan, or given another member's
## facts, or that writes ThinLTO index files for separate backends; and a member
## linked without -import-slots.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium b.s -o b.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium app.s -o app.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium extra.s -o extra.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -lldjointscan:b.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj -out:b.dll -opt:noref \
# RUN:   -lldjointscan:b-noref.jscan
# RUN: lld-link -import-slots -entry:main -subsystem:console app.obj \
# RUN:   -out:app.exe -lldjointscan:app.jscan

# RUN: not lld-link -lldjointmerge -out:facts a.jscan b-noref.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=OPTION %s
# OPTION: error: b.dll's link line has /opt:noref, which a.dll's lacks; a joint link's members share every option but those of their own inputs and outputs

## How ThinLTO's backends run and how many changes nothing a link writes.
# RUN: lld-link -import-slots -dll -noentry extra.obj -out:x.dll \
# RUN:   -opt:lldltojobs=2 -thinlto-distributor:dist.exe \
# RUN:   -thinlto-remote-compiler:clang.exe -lto-obj-path:x.lto.obj \
# RUN:   -lldjointscan:x.jscan
# RUN: lld-link -lldjointmerge -out:dist a.jscan x.jscan
# RUN: lld-link -import-slots -dll -noentry extra.obj -out:x.dll \
# RUN:   -opt:lldltojobs=4 -lldjoint:dist/x.dll.jfacts

# RUN: not lld-link -lldjointmerge -out:facts a.jscan a.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=TWICE %s
# TWICE: error: a.dll is a member of the joint link twice

## a.dll and b.dll import from the executable, which keeps no entity for the
## others, so neither can keep v for it.
# RUN: not lld-link -lldjointmerge -out:facts a.jscan b.jscan app.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=CYCLE %s
# CYCLE: error: no member of the joint link can keep v for the others without an initialization cycle: a.dll, b.dll

# RUN: llvm-lib -machine:x64 -def:a.def -out:a.lib
# RUN: llvm-lib -machine:x64 -def:b.def -out:b.lib
# RUN: lld-link -import-slots -dll -noentry a.obj b.lib -out:a.dll \
# RUN:   -lldjointscan:a-lib.jscan
# RUN: lld-link -import-slots -dll -noentry b.obj a.lib -out:b.dll \
# RUN:   -lldjointscan:b-lib.jscan
# RUN: not lld-link -lldjointmerge -out:facts a-lib.jscan b-lib.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=LIBS %s
# LIBS: error: the link lines of a.dll, b.dll name each other's import libraries; name none of a member's in another's, and the joint link binds them

# RUN: lld-link -lldjointmerge -out:facts a.jscan b.jscan
# RUN: not lld-link -import-slots -dll -noentry b.obj extra.obj -out:b.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts 2>&1 | FileCheck --check-prefix=STALE %s
# STALE: error: b.dll: joint-link facts are stale (inputs changed since the scan); rerun the scan and the merge
# RUN: not lld-link -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/b.dll.jfacts 2>&1 | FileCheck --check-prefix=OTHER %s
# OTHER: error: facts/b.dll.jfacts: holds no joint-link facts for a.dll
# RUN: not lld-link -import-slots -dll -noentry b.obj -out:b.dll \
# RUN:   -thinlto-index-only -lldjoint:facts/b.dll.jfacts 2>&1 | \
# RUN:   FileCheck --check-prefix=INDEX %s
# INDEX: error: -thinlto-index-only: a joint link's member runs its ThinLTO backends in its final link; distribute them with -thinlto-distributor

# RUN: not lld-link -dll -noentry a.obj -out:a.dll -lldjointscan:a.jscan 2>&1 | \
# RUN:   FileCheck --check-prefix=SLOTS %s
# SLOTS: error: a joint link's members link with -import-slots

#--- a.s
  .section .data,"dw",discard,v
  .globl v
v:
  .long 1

  .text
  .globl af
af:
  movq __imp_v(%rip), %rax
  callq *__imp_hostf(%rip)
  callq *__imp_bf(%rip)
  retq

  .section .drectve,"yni"
  .ascii " -export:af"

#--- b.s
  .section .data,"dw",discard,v
  .globl v
v:
  .long 1

  .text
  .globl bf
bf:
  movq __imp_v(%rip), %rax
  callq *__imp_hostf(%rip)
  callq *__imp_af(%rip)
  retq

  .section .drectve,"yni"
  .ascii " -export:bf"

#--- app.s
  .section .data,"dw",discard,v
  .globl v
v:
  .long 1

  .text
  .globl main, hostf
main:
  movq __imp_v(%rip), %rax
hostf:
  retq

  .section .drectve,"yni"
  .ascii " -export:hostf"

#--- extra.s
  .text
  .globl extra
extra:
  retq

  .section .drectve,"yni"
  .ascii " -export:extra"

#--- a.def
LIBRARY a.dll
EXPORTS
  af

#--- b.def
LIBRARY b.dll
EXPORTS
  bf
