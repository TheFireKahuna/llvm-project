// REQUIRES: lld, x86-registered-target, aarch64-registered-target
// RUN: rm -rf %t && split-file %s %t && cd %t

// On Windows Itanium a C99 inline definition is a weak_odr definition, a
// NOLIBRARY weak external whose default is its body, and an extern inline
// definition, or one a non-inline declaration makes external, is the strong
// external definition.
// NT-POSIX keeps C99's rule.
// RUN: %clang --target=x86_64-unknown-windows-itanium -S -emit-llvm -o - a.c \
// RUN:   | FileCheck a.c --check-prefix=WEAK
// RUN: %clang --target=aarch64-unknown-windows-itanium -S -emit-llvm -o - a.c \
// RUN:   | FileCheck a.c --check-prefix=WEAK
// RUN: %clang --target=x86_64-pc-windows-ntposix -S -emit-llvm -o - a.c \
// RUN:   | FileCheck a.c --check-prefix=C99
// RUN: %clang --target=x86_64-unknown-windows-itanium -S -emit-llvm -o - c.c \
// RUN:   | FileCheck c.c

// The external definition wins over the inline body; the idiom of an inline
// definition in a header and its extern inline declaration in one unit
// links; and an inline function with no external definition, as the UCRT's
// header functions are, links once per image.
// RUN: %clang --target=x86_64-unknown-windows-itanium -ffunction-sections -c -o a.x64.o a.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -ffunction-sections -c -o b.x64.o b.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -ffunction-sections -c -o c.x64.o c.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -ffunction-sections -c -o d.x64.o d.c
// RUN: llvm-readobj --symbols a.x64.o | FileCheck a.c --check-prefix=OBJ
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:g -debug:symtab a.x64.o b.x64.o -out:ab.x64.dll
// RUN: llvm-objdump -d --disassemble-symbols=f ab.x64.dll | FileCheck b.c --check-prefix=X64
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:g -export:k c.x64.o d.x64.o -out:cd.x64.dll
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:g -export:k a.x64.o d.x64.o -out:ad.x64.dll
// RUN: %clang --target=aarch64-unknown-windows-itanium -ffunction-sections -c -o a.a64.o a.c
// RUN: llvm-readobj --symbols a.a64.o | FileCheck a.c --check-prefix=OBJ

//--- h.h
inline int f(void) { return 1; }

//--- a.c
#include "h.h"
int g(void) { return f(); }
// WEAK: define weak_odr dso_local i32 @f()
// WEAK-NOT: comdat
// C99-NOT: define {{.*}}@f(
// OBJ:        Name: f{{$}}
// OBJ:        StorageClass: WeakExternal
// OBJ-NEXT:   AuxSymbolCount: 1
// OBJ-NEXT:   AuxWeakExternal {
// OBJ-NEXT:     Linked: .weak.f.default.g
// OBJ-NEXT:     Search: NoLibrary (0x1)

//--- b.c
int f(void) { return 2; }
// X64: <f>:
// X64: movl $0x2, %eax

//--- c.c
#include "h.h"
extern inline int f(void);
int g(void) { return f(); }
// CHECK: define dso_local i32 @f()

//--- d.c
#include "h.h"
int k(void) { return f(); }
