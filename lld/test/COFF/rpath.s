# REQUIRES: x86, aarch64

## ELF's spellings of -rpath are accepted and ignored with a warning: a PE image
## has no run-time library search path.

# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %s -o %t.x64.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows %s -o %t.arm64.obj

# RUN: lld-link -entry:main -out:%t.exe %t.x64.obj -rpath /usr/lib 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=warning:
# RUN: lld-link -entry:main -out:%t.exe %t.arm64.obj --rpath /usr/lib 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=warning:
# RUN: lld-link -entry:main -out:%t.exe %t.x64.obj -rpath=/usr/lib \
# RUN:   --rpath=/opt/lib 2>&1 | FileCheck %s --implicit-check-not=warning:

# CHECK: warning: ignoring '-rpath', a PE image has no run-time library search path

.globl main
main:
  ret
