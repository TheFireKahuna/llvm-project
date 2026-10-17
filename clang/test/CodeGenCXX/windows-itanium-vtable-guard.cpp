// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -fno-auto-import -mdefault-visibility-export-mapping=explicit \
// RUN:   -fsanitize=kcfi -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fno-auto-import \
// RUN:   -mdefault-visibility-export-mapping=explicit -fsanitize=kcfi \
// RUN:   -emit-llvm -o - %s | FileCheck %s

/// A virtual call's slot load names the class's run and the slot's offset,
/// for its vtable guard, and whether another image may own vtables of the
/// class's run, in which case the guard also tests the owner's: a class of
/// default visibility may be another image's, one of hidden visibility or
/// internal linkage is the image's own.

#define API __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))

struct API Exported {
  virtual void f();
  virtual void g();
};
struct Plain {
  virtual void f();
};
struct HIDDEN Hidden {
  virtual void f();
};
namespace {
struct Internal {
  virtual void f() {}
};
} // namespace

void call(Exported *e, Plain *p, Hidden *h) {
  e->g();
  p->f();
  h->f();
}
Internal *internal() { static Internal i; return &i; }
void callInternal() { internal()->f(); }

// CHECK-DAG: !{!"8Exported", i64 8, i1 true}
// CHECK-DAG: !{!"5Plain", i64 0, i1 true}
// CHECK-DAG: !{!"6Hidden", i64 0, i1 false}
// CHECK-DAG: !{!"N12_GLOBAL__N_18InternalE{{.*}}", i64 0, i1 false}
