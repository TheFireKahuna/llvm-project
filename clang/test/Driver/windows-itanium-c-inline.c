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
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o a.x64.o a.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o b.x64.o b.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o c.x64.o c.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o d.x64.o d.c
// RUN: llvm-readobj --symbols a.x64.o | FileCheck a.c --check-prefix=OBJ
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:g -debug:symtab a.x64.o b.x64.o -out:ab.x64.dll
// RUN: llvm-objdump -d --disassemble-symbols=f$tp ab.x64.dll | FileCheck b.c --check-prefix=X64
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:g -export:k c.x64.o d.x64.o -out:cd.x64.dll
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:g -export:k a.x64.o d.x64.o -out:ad.x64.dll
// RUN: %clang --target=aarch64-unknown-windows-itanium -O2 -ffunction-sections -c -o a.a64.o a.c
// RUN: llvm-readobj --symbols a.a64.o | FileCheck a.c --check-prefix=OBJ

// None of C's inline, extern inline and static inline functions is a vague
// entity, though its address is taken: an image keeps its own, with no cell,
// a DLL as an executable. In C++ an inline function is one, so a DLL reaches
// one whose address it takes through a cell, which -nodefaultlib leaves no
// fill to write; one it only calls needs none, and a static one is its own.
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o ct.o ct.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o ce.o ce.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o cs.o cs.c
// RUN: llvm-readobj --coff-link-records ct.o ce.o cs.o | FileCheck ct.c --check-prefix=NONE
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:t ct.o -out:ct.dll
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:t ce.o -out:ce.dll
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:t cs.o -out:cs.dll
// RUN: lld-link -machine:x64 -nodefaultlib -entry:t -subsystem:console ct.o -out:ct.exe
// RUN: lld-link -machine:x64 -nodefaultlib -entry:t -subsystem:console ce.o -out:ce.exe
// RUN: lld-link -machine:x64 -nodefaultlib -entry:t -subsystem:console cs.o -out:cs.exe
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -x c++ -c -o ct.xo ct.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -x c++ -c -o ce.xo ce.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -x c++ -c -o cs.xo cs.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -x c++ -c -o d.xo d.c
// RUN: llvm-readobj --coff-link-records ct.xo | FileCheck ct.c --check-prefix=TAKEN
// RUN: llvm-readobj --coff-link-records ce.xo | FileCheck ct.c --check-prefix=TAKEN
// RUN: llvm-readobj --coff-link-records cs.xo d.xo | FileCheck ct.c --check-prefix=NONE
// RUN: not lld-link -machine:x64 -dll -noentry -nodefaultlib -export:_Z1tv ct.xo \
// RUN:   -out:ct.dll 2>&1 | FileCheck ct.c --check-prefix=CELL
// RUN: not lld-link -machine:x64 -dll -noentry -nodefaultlib -export:_Z1tv ce.xo \
// RUN:   -out:ce.dll 2>&1 | FileCheck ct.c --check-prefix=CELL
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:_Z1tv cs.xo -out:cs.dll
// RUN: lld-link -machine:x64 -dll -noentry -nodefaultlib -export:_Z1kv d.xo -out:d.dll
// RUN: lld-link -machine:x64 -nodefaultlib -entry:_Z1tv -subsystem:console ct.xo -out:ct.exe
// RUN: lld-link -machine:x64 -nodefaultlib -entry:_Z1tv -subsystem:console ce.xo -out:ce.exe

// An explicit instantiation definition whose unit only calls it is offered
// as one whose address another unit, seeing only its declaration, takes.
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o x.o x.cpp
// RUN: %clang --target=x86_64-unknown-windows-itanium -O2 -ffunction-sections -c -o y.o y.cpp
// RUN: llvm-readobj --coff-link-records x.o | FileCheck ct.c --check-prefix=TAKEN
// RUN: not lld-link -machine:x64 -dll -noentry -nodefaultlib -export:_Z1kv \
// RUN:   -export:_Z1tv x.o y.o -out:xy.dll 2>&1 | FileCheck ct.c --check-prefix=CELL

//--- h.h
inline int f(void) { return 1; }

//--- a.c
#include "h.h"
int g(void) { return f(); }
// WEAK: define weak_odr dso_local i32 @f()
// WEAK-NOT: comdat
// C99-NOT: define {{.*}}@f(
// OBJ:        Name: f$tp{{$}}
// OBJ:        StorageClass: WeakExternal
// OBJ-NEXT:   AuxSymbolCount: 1
// OBJ-NEXT:   AuxWeakExternal {
// OBJ-NEXT:     Linked: .weak.f$tp.default.g$tp
// OBJ-NEXT:     Search: NoLibrary (0x1)

//--- b.c
int f(void) { return 2; }
// X64: <f$tp>:
// X64: movl $0x2, %eax

//--- c.c
#include "h.h"
extern inline int f(void);
int g(void) { return f(); }
// CHECK: define dso_local i32 @f()

//--- d.c
#include "h.h"
int k(void) { return f(); }

//--- ct.c
#include "h.h"
int (*t(void))(void) { return f; }
// NONE-NOT: AddressSignificant
// TAKEN: Tag: AddressSignificant
// CELL: error: the image writes its cells as it starts, which needs __llvm_fill_dll_imports

//--- ce.c
#include "h.h"
extern inline int f(void);
int (*t(void))(void) { return f; }

//--- cs.c
static inline int f(void) { return 1; }
int (*t(void))(void) { return f; }

//--- x.cpp
template <class T> int f() { return 1; }
template int f<int>();
int k() { return f<int>(); }

//--- y.cpp
template <class T> int f();
extern template int f<int>();
int (*t())() { return f<int>; }
