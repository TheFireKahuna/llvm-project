// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=DEFAULT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -mdefault-visibility-export-mapping=explicit -fabi-witness-c-types \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix \
// RUN:   -mdefault-visibility-export-mapping=explicit -fabi-witness-c-types \
// RUN:   -emit-llvm -o - %s | FileCheck %s

// The contracts of C structs and of the types of C data are checked between
// images only under -fabi-witness-c-types, since a versioned C struct that
// grows at its tail is an accepted pattern. A struct is named as C++ names it,
// a typedef name standing for an anonymous struct's, and a struct without a
// name is part of the layout of the struct that holds it.

#define API __attribute__((visibility("default")))

struct S { int a; struct { int x, y; } inner; };
typedef struct { long v; } Anon;
API void takeS(struct S *);
API void takeAnon(Anon *);
API extern int cdata;
API extern Anon anonData;
API struct S provided;
API void provide(struct S *s) {}

int use(struct S *s, Anon *a) {
  takeS(s);
  takeAnon(a);
  return s->a + a->v + cdata + (int)anonData.v;
}

// DEFAULT-NOT: abiprovide
// DEFAULT-NOT: abi.requirements

// CHECK: !llvm.linker.options = !{![[#P:]]}
// CHECK: !abi.requirements = !{![[#TAKES:]], ![[#TAKEANON:]], ![[#CDATA:]], ![[#ANONDATA:]]}
// CHECK: ![[#P]] = !{!"/abiprovide:{{[[:alnum:]_-]+}}"}
// CHECK: ![[#TAKES]] = !{ptr @takeS, ![[#S:]]}
// CHECK: ![[#S]] = !{!"1S", !"{{.+}}"}
// CHECK: ![[#TAKEANON]] = !{ptr @takeAnon, ![[#ANON:]]}
// CHECK: ![[#ANON]] = !{!"4Anon", !"{{.+}}"}
// CHECK: ![[#CDATA]] = !{ptr @cdata, ![[#INT:]]}
// CHECK: ![[#INT]] = !{!"{{.+}}"}
// CHECK: ![[#ANONDATA]] = !{ptr @anonData, ![[#ANON]]}
