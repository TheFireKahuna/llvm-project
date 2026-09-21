// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -DDEFINE -emit-llvm -o - %s | FileCheck %s --check-prefix=PRODUCER

// A constant pointer to a member of a variable in another image names the
// member, which the defining image publishes; an array element keeps its
// offset.

struct S {
  int a;
  int b;
  struct { int x; int y; } in;
  int arr[4];
};

extern struct S s;

// CHECK: @pb = dso_local global ptr @"s$so4"
// CHECK: @py = dso_local global ptr @"s$so12"
// CHECK: @pelem = dso_local global ptr getelementptr (i8, ptr @s, i64 20)
int *pb = &s.b;
int *py = &s.in.y;
int *pelem = &s.arr[1];

#ifdef DEFINE
struct S s = {1, 2, {3, 4}, {5, 6, 7, 8}};
#endif

// PRODUCER:      @"s$so4" = alias i8, getelementptr inbounds (i8, ptr @s, i64 4)
// PRODUCER-NEXT: @"s$so8" = alias i8, getelementptr inbounds (i8, ptr @s, i64 8)
// PRODUCER-NEXT: @"s$so12" = alias i8, getelementptr inbounds (i8, ptr @s, i64 12)
// PRODUCER-NEXT: @"s$so16" = alias i8, getelementptr inbounds (i8, ptr @s, i64 16)
// PRODUCER-NOT:  $so
