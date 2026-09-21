// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -DDEFINE -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,PRODUCER
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -DDEFINE -DEXPORT -emit-llvm -o - %s | FileCheck %s --check-prefix=EXPORT
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -emit-llvm -o - %s | FileCheck %s --check-prefix=MINGW

// A variable in another image is reached through an address the loader fills,
// and static data can hold that address but not an offset from it. The image
// that defines a variable of class type names each subobject whose offset the
// type fixes, and a constant pointer to one of them in an image that imports
// the variable uses the name. Nothing inside an array is named: its elements
// are indexed freely, so a pointer to one keeps the offset.

struct A { int a; };
struct B { int b; };
struct In { int x; int y; };
struct D : A, B {
  int m;
  In in;
  int arr[4];
  In inarr[2];
};
struct V { int v; };
struct W : virtual V { int w; };

#ifdef EXPORT
#define API __attribute__((dllexport))
#else
#define API
#endif

extern API D obj;
extern W wobj;
extern D objs[2];
extern __attribute__((visibility("hidden"))) D hidden_obj;

// A base, a member, a member of a member, an array member itself, and a
// virtual base in the complete object.
// CHECK: @pb = dso_local global ptr @"obj$so4"
// CHECK: @pm = dso_local global ptr @"obj$so8"
// CHECK: @py = dso_local global ptr @"obj$so16"
// CHECK: @parr = dso_local global ptr @"obj$so20"
// CHECK: @pv = dso_local global ptr @"wobj$so12"
B *pb = &obj;
int *pm = &obj.m;
int *py = &obj.in.y;
int *parr = &obj.arr[0];
V *pv = &wobj;

// Inside an array, and a variable this image reaches directly: the offset.
// CHECK: @pelem = dso_local global ptr getelementptr (i8, ptr @obj, i64 28)
// CHECK: @pinelem = dso_local global ptr getelementptr (i8, ptr @obj, i64 48)
// CHECK: @parray = dso_local global ptr getelementptr (i8, ptr @objs, i64 56)
// CHECK: @phidden = dso_local global ptr getelementptr (i8, ptr @hidden_obj, i64 4)
int *pelem = &obj.arr[2];
int *pinelem = &obj.inarr[1].y;
B *parray = &objs[1];
B *phidden = &hidden_obj;

// The definitions come after the uses, so each name replaces the declaration
// the uses left behind. Every distinct non-zero offset gets one, whether or
// not a use asked for it; an array, a hidden variable and an internal one get
// none.
#ifdef DEFINE
D obj;
W wobj;
D objs[2];
__attribute__((visibility("hidden"))) D hidden_obj;
static D local_obj;
D *use_local() { return &local_obj; }
#endif

// PRODUCER:      @"obj$so4" = alias i8, getelementptr inbounds (i8, ptr @obj, i64 4)
// PRODUCER-NEXT: @"obj$so8" = alias i8, getelementptr inbounds (i8, ptr @obj, i64 8)
// PRODUCER-NEXT: @"obj$so12" = alias i8, getelementptr inbounds (i8, ptr @obj, i64 12)
// PRODUCER-NEXT: @"obj$so16" = alias i8, getelementptr inbounds (i8, ptr @obj, i64 16)
// PRODUCER-NEXT: @"obj$so20" = alias i8, getelementptr inbounds (i8, ptr @obj, i64 20)
// PRODUCER-NEXT: @"obj$so36" = alias i8, getelementptr inbounds (i8, ptr @obj, i64 36)
// PRODUCER-NEXT: @"wobj$so8" = alias i8, getelementptr inbounds (i8, ptr @wobj, i64 8)
// PRODUCER-NEXT: @"wobj$so12" = alias i8, getelementptr inbounds (i8, ptr @wobj, i64 12)
// PRODUCER-NOT:  $so

// An exported variable exports its names.
// EXPORT: @"obj$so4" = dllexport alias i8, getelementptr inbounds (i8, ptr @obj, i64 4)

// MINGW-NOT: $so
