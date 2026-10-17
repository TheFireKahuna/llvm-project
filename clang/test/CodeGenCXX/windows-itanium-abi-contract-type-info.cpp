// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck %s

// On Windows Itanium and NT-POSIX a type_info object that the image may export
// or offer provides the contract of its class, so that every class another
// image can name by its type_info has one, although no function or datum the
// image exports is about it: here the bases of exported classes. A row starts
// with the locator of the class's identity, the first eight bytes of the xxh3
// hash of its mangling.

#define API __attribute__((visibility("default")))

struct API Base { int b; };
struct API Derived : Base { virtual void f(); };
void Derived::f() {}

template <class T> struct API TBase { T t; };
struct API Use : TBase<char> { virtual void g(); };
void Use::g() {}

// A class of hidden visibility is the image's own, and provides nothing.
struct __attribute__((visibility("hidden"))) Hidden { int h; };
struct API UsesHidden : Hidden { virtual void h(); };
void UsesHidden::h() {}

// CHECK: !{!"/abiprovide:
// Derived and Use, which the exported functions are about.
// CHECK-SAME: nIM5n0S22VI
// CHECK-SAME: wnI0MGV-M-E
// Base and TBase<char>, through their type_info objects.
// CHECK-SAME: YOk3vigTQyw
// CHECK-SAME: wJVIkjMNAqk
// CHECK-NOT: qVpI9GNpu6w
// CHECK-SAME: "}
