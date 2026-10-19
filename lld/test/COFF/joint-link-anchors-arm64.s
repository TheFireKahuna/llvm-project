# REQUIRES: aarch64
## On ARM64, a load of the import pointer of a member DLL's data whose pointer
## the register of a load of the pointer of other data of that DLL holds
## becomes an add of the distance to that register, or a sub, shifted by 12
## where the distance's low bits are zero; a distance no such immediate holds
## leaves the load. The adrp before it keeps the import.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=aarch64-windows-itanium app.s -o app.obj
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjointscan:a.jscan
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   app.obj a.lib -out:app.exe -lldjointscan:app.jscan
# RUN: lld-link -lldjointmerge -out:facts a.jscan app.jscan
# RUN: lld-link -machine:arm64 -import-slots -dll -noentry a.obj -out:a.dll \
# RUN:   -lldjoint:facts/a.dll.jfacts
# RUN: lld-link -machine:arm64 -import-slots -entry:main -subsystem:console \
# RUN:   app.obj a.lib -out:app.exe -lldjoint:facts/app.exe.jfacts
# RUN: llvm-objdump -d --no-show-raw-insn app.exe | FileCheck %s
# RUN: llvm-readobj --coff-imports app.exe | FileCheck --check-prefix=IMPORTS %s

# CHECK:      ldr x0, [x0, #0x{{[0-9a-f]+}}]
# CHECK-NEXT: add x1, x0, #0x18
# CHECK-NEXT: sub x2, x1, #0x8
# CHECK-NEXT: add x3, x0, #0x2, lsl #12
# CHECK-NEXT: ldr x4, [x4, #0x{{[0-9a-f]+}}]
# CHECK-NEXT: ret

# IMPORTS:      Symbol: __llvm_private${{[0-9a-f]+}} (0)
# IMPORTS-NEXT: Symbol: far (0)
# IMPORTS-NEXT: Symbol: odd (0)
# IMPORTS-NEXT: Symbol: x (0)
# IMPORTS-NEXT: Symbol: y (0)
# IMPORTS-NEXT: Symbol: z (0)

#--- a.s
  .data
  .globl x, y, z, far, odd
x:
  .quad 1
  .quad 0
z:
  .quad 3
y:
  .quad 2
  .fill 8160, 1, 0
far:
  .quad 4
  .fill 4096, 1, 0
odd:
  .quad 5

  .section .drectve,"yni"
  .ascii " -export:x,DATA -export:y,DATA -export:z,DATA -export:far,DATA"
  .ascii " -export:odd,DATA"

#--- app.s
  .text
  .globl main
main:
  adrp x0, __imp_x
  adrp x1, __imp_y
  adrp x2, __imp_z
  adrp x3, __imp_far
  adrp x4, __imp_odd
.Lx:
  ldr x0, [x0, :lo12:__imp_x]
.Ly:
  .linkanchor .Lx, x0
  ldr x1, [x1, :lo12:__imp_y]
  .linkanchor .Ly, x1
  ldr x2, [x2, :lo12:__imp_z]
  .linkanchor .Lx, x0
  ldr x3, [x3, :lo12:__imp_far]
  .linkanchor .Lx, x0
  ldr x4, [x4, :lo12:__imp_odd]
  ret
