// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o %t.ll %s
// RUN: FileCheck --check-prefix=CHECK %s < %t.ll
// RUN: FileCheck --check-prefixes=NONE,NOC %s < %t.ll
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=CHECK %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -fabi-witness-c-types \
// RUN:   -emit-llvm -o %t.c.ll %s
// RUN: FileCheck --check-prefixes=CHECK,CTYPES %s < %t.c.ll
// RUN: FileCheck --check-prefix=NONE %s < %t.c.ll
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -fno-autolink \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=CHECK %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=ELF %s

// Debug information takes layouts of its own, which require nothing.
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm \
// RUN:   -debug-info-kind=standalone -o %t.g.ll %s
// RUN: FileCheck --check-prefix=CHECK %s < %t.g.ll
// RUN: FileCheck --check-prefixes=NONE,NOC %s < %t.g.ll

// On Windows Itanium and NT-POSIX a definition that the image may export or
// offer provides the ABI contracts of the classes it is about, as linker
// options, and a declaration in import form requires those whose layout the
// code uses, as the abi.requirements metadata. A pointer in a covered class's
// layout adds its pointee, one level deep, where the code uses its layout. A
// thread-local variable's contracts are its record's, through which another
// image reaches it. An alias of a variable with vague linkage provides and
// requires the contract of its own type, as the variable does.

#define API __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))

struct Impl { int a; };
struct Other { int o; };

// Imported members of a class the code uses by layout, and a pointee whose
// layout it uses too; Other's it never touches.
struct API Imported { int x; Impl *impl; Other *other; Imported(); void f(); };
// Named in a signature, never used by layout.
struct API Named { int y; };
API void takesNamed(Named *);
// Used only by sizeof.
struct API Sized { char c[12]; };
API void fill(Sized *);
// Exempt from contracts.
struct [[clang::no_abi_witness]] API Grows { unsigned size; int v; };
API void grow(Grows *);
// A class declared in an extern "C" context is a C struct.
extern "C" {
struct CStruct { int c; };
API void takesC(CStruct *);
}
// A class the code constructs: its vtable is copied, and the copy requires
// the class's contract through the imported functions it holds, or, for a
// final class, it is imported.
struct API Dyn { virtual void v(); int d; };
struct API DynFinal final { virtual void v(); int e; };

API extern int counter;
API extern int table[4];
extern "C" API int cdata;
API extern thread_local int tls;
inline int vague = 3;
extern int vaguealias __attribute__((alias("vague")));

// Exported definitions.
struct Exported { double d; Imported *p; API void g(); };
void Exported::g() {}
struct HIDDEN Hidden { int h; void k(); };
void Hidden::k() {}

int use(Named *n, Sized *s, Grows *gr, CStruct *cs) {
  Imported i;
  i.f();
  takesNamed(n);
  fill(s);
  grow(gr);
  takesC(cs);
  Dyn d;
  DynFinal df;
  return i.x + i.impl->a + counter + table[2] + vague + cdata + gr->v + cs->c +
         int(sizeof(Sized)) + tls + d.d + df.e + vaguealias;
}

// Exported's own layout and Imported's, which its pointer reaches and the code
// uses; the layouts of Dyn and DynFinal, whose inline constructors may be
// offered; the types of the inline variable and its alias.
// CHECK: !abi.requirements = !{
// CHECK: !llvm.linker.options = !{![[#P:]], ![[#PD:]], ![[#ABI:]]}
// CHECK-DAG: ![[#P]] = !{!"/abiprovide:{{[[:alnum:]_-]+,[[:alnum:]_-]+,[[:alnum:]_-]+,[[:alnum:]_-]+}}"}
// CHECK-DAG: ![[#PD]] = !{!"/abiprovidedata:{{[[:alnum:]_-]+,[[:alnum:]_-]+}}"}
// CHECK-DAG: ![[#ABI]] = !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_CXX_ABI=1\22"}

// CHECK-DAG: !{ptr @_ZN8ImportedC1Ev, ![[#IMPORTED:]], ![[#IMPL:]]}
// CHECK-DAG: !{ptr @_ZN8Imported1fEv, ![[#IMPORTED]], ![[#IMPL]]}
// CHECK-DAG: ![[#IMPORTED]] = !{!"8Imported", !"{{.+}}"}
// CHECK-DAG: ![[#IMPL]] = !{!"4Impl", !"{{.+}}"}
// CHECK-DAG: !{ptr @_Z4fillP5Sized, ![[#SIZED:]]}
// CHECK-DAG: ![[#SIZED]] = !{!"5Sized", !"{{.+}}"}
// CHECK-DAG: !{ptr @_ZN3Dyn1vEv, ![[#DYN:]]}
// CHECK-DAG: ![[#DYN]] = !{!"3Dyn", !"{{.+}}"}
// CHECK-DAG: !{ptr @_ZTV8DynFinal, ![[#DYNFINAL:]]}
// CHECK-DAG: ![[#DYNFINAL]] = !{!"8DynFinal", !"{{.+}}"}
// CHECK-DAG: !{ptr @counter, !{{[0-9]+}}}
// CHECK-DAG: !{ptr @table, !{{[0-9]+}}}
// CHECK-DAG: !{ptr @vague, !{{[0-9]+}}}
// CHECK-DAG: !{ptr @vaguealias, !{{[0-9]+}}}
// CHECK-DAG: !{ptr @"tls$tls", !{{[0-9]+}}}
// CTYPES-DAG: !{ptr @takesC, ![[#C:]]}
// CTYPES-DAG: ![[#C]] = !{!"7CStruct", !"{{.+}}"}
// CTYPES-DAG: !{ptr @cdata, !{{[0-9]+}}}

// NOC-NOT: !{!"7CStruct",
// NOC-NOT: !{ptr @cdata,
// NONE-NOT: !{ptr @_Z10takesNamedP5Named,
// NONE-NOT: !{ptr @_Z4growP5Grows,
// NONE-NOT: !{!"5Grows",
// NONE-NOT: !{!"5Other",
// NONE-NOT: !{ptr @tls,
// NONE-NOT: !{!"6Hidden",

// ELF-NOT: abiprovide
// ELF-NOT: abi.requirements
