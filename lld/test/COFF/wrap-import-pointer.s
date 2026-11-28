# REQUIRES: x86, aarch64

## Under -wrap:foo, __imp_foo points to __wrap_foo and __imp___real_foo is
## foo's own pointer: its import address, or a local pointer that typed sites
## reach past; a missing foo is reported under its own name.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab \
# DEFINE:   -wrap:foo

# RUN: %{mc} -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium foo.s -o foo-x64.obj
# RUN: %{mc} -triple=x86_64-windows-msvc x64.s -o x64-msvc.obj
# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium foo.s -o foo-a64.obj
# RUN: lld-link -dll -noentry -export:foo -out:foo-x64.dll \
# RUN:   -implib:foo-x64.lib foo-x64.obj
# RUN: lld-link -dll -noentry -export:foo -out:foo-a64.dll \
# RUN:   -implib:foo-a64.lib foo-a64.obj

## foo in the image: no pointer is left, and nothing is reported.
# RUN: %{link} -out:x64.exe foo-x64.obj x64.obj 2>&1 | count 0
# RUN: llvm-objdump -d -s -j .text -j .data x64.exe | \
# RUN:   FileCheck --check-prefixes=X64,X64-LOCAL %s
# RUN: llvm-readobj --sections x64.exe | FileCheck --check-prefix=NOPTR %s
# RUN: %{link} -out:a64.exe foo-a64.obj a64.obj 2>&1 | count 0
# RUN: llvm-objdump -d -s -j .text -j .data a64.exe | \
# RUN:   FileCheck --check-prefixes=A64,A64-LOCAL %s
# RUN: llvm-readobj --sections a64.exe | FileCheck --check-prefix=NOPTR %s

## foo imported, which only the pointers name: its import address is read
## where it is, and the data word that holds its address is a second one.
# RUN: %{link} -out:x64-dll.exe x64.obj foo-x64.lib 2>&1 | count 0
# RUN: llvm-objdump -d x64-dll.exe | FileCheck --check-prefixes=X64,X64-DLL %s
# RUN: llvm-readobj --coff-imports x64-dll.exe | \
# RUN:   FileCheck --check-prefix=IMPORT --implicit-check-not=Symbol: %s
# RUN: %{link} -out:a64-dll.exe a64.obj foo-a64.lib 2>&1 | count 0
# RUN: llvm-objdump -d a64-dll.exe | FileCheck --check-prefixes=A64,A64-DLL %s
# RUN: llvm-readobj --coff-imports a64-dll.exe | \
# RUN:   FileCheck --check-prefix=IMPORT --implicit-check-not=Symbol: %s

## An object of another environment reads the pointers it names.
# RUN: %{link} -out:x64-msvc.exe foo-x64.obj x64-msvc.obj
# RUN: llvm-objdump -d -s -j .text -j .rdata x64-msvc.exe | \
# RUN:   FileCheck --check-prefix=MSVC %s

## foo absent: the wrapped references link, and the real ones name foo.
# RUN: %{mc} -triple=x86_64-windows-itanium wrapped.s -o wrapped.obj
# RUN: %{link} -out:wrapped.exe wrapped.obj
# RUN: llvm-objdump -d wrapped.exe | FileCheck --check-prefix=WRAPPED %s
# RUN: not %{link} -out:absent.exe x64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=ABSENT --implicit-check-not=__real_foo %s
# RUN: not %{link} -out:absent.exe a64.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=ABSENT --implicit-check-not=__real_foo %s

## The pointer to the real foo alone applies the wrap and loads the foo that
## an archive offers.
# RUN: %{mc} -triple=x86_64-windows-itanium wrapped.s --defsym REAL=1 -o real.obj
# RUN: llvm-lib -out:foo.lib foo-x64.obj
# RUN: %{link} -out:real.exe real.obj foo.lib
# RUN: llvm-objdump -d real.exe | FileCheck --check-prefix=REAL %s
# RUN: lld-link -entry:main -subsystem:console -wrap:foo -out:real.exe \
# RUN:   real.obj foo.lib -verbose 2>&1 | FileCheck --check-prefix=LOAD %s
## The image's foo stands where an import library offers one too.
# RUN: %{link} -out:both.exe real.obj foo-x64.obj foo-x64.lib
# RUN: llvm-objdump -d both.exe | FileCheck --check-prefix=REAL %s
# RUN: llvm-readobj --coff-imports both.exe | FileCheck --check-prefix=NONE %s

## Bitcode that named __real_foo names the pointer.
# RUN: llvm-as lto.ll -o lto.bc
# RUN: %{link} -out:lto.exe lto.bc foo-x64.obj
# RUN: llvm-objdump -d lto.exe | FileCheck --check-prefix=LTO %s
# RUN: %{link} -out:lto-dll.exe lto.bc foo-x64.lib
# RUN: llvm-objdump -d lto-dll.exe | FileCheck --check-prefix=LTO-DLL %s

# X64-LOCAL:      Contents of section .data:
# X64-LOCAL-NEXT: 00100040 01000000
# X64-LABEL:      <main>:
# X64-NEXT:       callq {{.*}} <__wrap_foo>
# X64-NEXT:       callq {{.*}} <__wrap_foo>
# X64-NEXT:       callq {{.*}} <__wrap_foo>
# X64-LOCAL-NEXT: callq {{.*}} <foo>
# X64-LOCAL-NEXT: callq {{.*}} <foo>
# X64-LOCAL-NEXT: leaq {{.*}} <foo>
# X64-DLL-NEXT:   callq *{{.*}} <__imp_foo>
# X64-DLL-NEXT:   callq {{.*}} <foo>
# X64-DLL-NEXT:   movq {{.*}} <__imp_foo>
# X64-NEXT:       retq
# X64-LABEL:      <__wrap_foo>:
# X64-NEXT:       incl
# X64-LOCAL-NEXT: jmp {{.*}} <foo>
# X64-DLL-NEXT:   jmpq *{{.*}} <__imp_foo>

# A64-LOCAL:      Contents of section .data:
# A64-LOCAL-NEXT: 00100040 01000000
# A64-LABEL:      <main>:
# A64-NEXT:       adrp x8, 0x140001000
# A64-NEXT:       add x8, x8, #0x[[#%x,WRAP:]]
# A64-NEXT:       blr x8
# A64-NEXT:       adrp x8, 0x140001000
# A64-NEXT:       add x8, x8, #0x[[#WRAP]]
# A64-NEXT:       blr x8
# A64-NEXT:       bl {{.*}} <__wrap_foo>
# A64-LOCAL-NEXT: adrp x8, 0x140001000
# A64-LOCAL-NEXT: add x8, x8, #0x0
# A64-DLL-NEXT:   ldr x8, {{.*}} <__imp_foo>
# A64-NEXT:       blr x8
# A64-NEXT:       bl {{.*}} <foo>
# A64-NEXT:       ret
# A64-EMPTY:
# A64-NEXT:       {{0*}}[[#%x,0x140001000+WRAP]] <__wrap_foo>:
# A64-NEXT:       add w0, w0, #0x1
# A64-LOCAL-NEXT: adrp x8, 0x140001000
# A64-LOCAL-NEXT: add x8, x8, #0x0
# A64-DLL-NEXT:   ldr x8, {{.*}} <__imp_foo>
# A64-NEXT:       br x8

# NOPTR-NOT: Name: .rdata

# IMPORT: Symbol: foo (0)
# IMPORT: Symbol: foo (0)

# MSVC:       Contents of section .rdata:
# MSVC-NEXT:  140002000 [[#%x,]] 01000000 00100040 01000000
# MSVC-LABEL: <main>:
# MSVC-NEXT:  callq *{{.*}} # 0x140002000
# MSVC-NEXT:  callq *{{.*}} # 0x140002000
# MSVC-NEXT:  callq {{.*}} <__wrap_foo>
# MSVC-NEXT:  callq *{{.*}} # 0x140002008
# MSVC-NEXT:  callq {{.*}} <foo>
# MSVC-NEXT:  movq {{.*}} # 0x140002008

# WRAPPED-LABEL: <main>:
# WRAPPED-NEXT:  callq {{.*}} <__wrap_foo>

# REAL-LABEL: <main>:
# REAL-NEXT:  : e8 {{.*}} callq

# NONE-NOT: Import

# LOAD: Loaded foo.lib(foo-x64.obj) for foo

# ABSENT:     error: undefined symbol: __declspec(dllimport) foo
# ABSENT:     error: undefined symbol: foo
# ABSENT-NOT: error:

# LTO-LABEL: <__wrap_foo>:
# LTO:       jmp {{.*}} <foo>
# LTO-DLL-LABEL: <__wrap_foo>:
# LTO-DLL:       jmpq *{{.*}} <__imp_foo>

#--- x64.s
  .text
  .globl main
main:
  callq *__imp_foo(%rip)
  callq *__imp___wrap_foo(%rip)
  callq __wrap_foo
  callq *__imp___real_foo(%rip)
  callq __real_foo
  movq __imp___real_foo(%rip), %rax
  retq
  .globl __wrap_foo
__wrap_foo:
  incl %eax
  jmpq *__imp___real_foo(%rip)
  .data
  .quad __real_foo

#--- a64.s
  .text
  .globl main
main:
  adrp x8, __imp_foo
  ldr x8, [x8, :lo12:__imp_foo]
  blr x8
  adrp x8, __imp___wrap_foo
  ldr x8, [x8, :lo12:__imp___wrap_foo]
  blr x8
  bl __wrap_foo
  adrp x8, __imp___real_foo
  ldr x8, [x8, :lo12:__imp___real_foo]
  blr x8
  bl __real_foo
  ret
  .globl __wrap_foo
__wrap_foo:
  add w0, w0, #1
  adrp x8, __imp___real_foo
  ldr x8, [x8, :lo12:__imp___real_foo]
  br x8
  .data
  .xword __real_foo

#--- foo.s
  .text
  .globl foo
foo:
  ret

## With REAL, only the pointer to the real foo names it.
#--- wrapped.s
  .text
  .globl main
main:
.ifdef REAL
  callq *__imp___real_foo(%rip)
.else
  callq *__imp_foo(%rip)
.endif
  retq
  .globl __wrap_foo
__wrap_foo:
  retq

#--- lto.ll
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-windows-itanium"

declare void @foo()
declare i32 @__real_foo(i32)

define void @main() {
  call void @foo()
  ret void
}

define i32 @__wrap_foo(i32 %x) {
  %y = add i32 %x, 1
  %r = tail call i32 @__real_foo(i32 %y)
  ret i32 %r
}
