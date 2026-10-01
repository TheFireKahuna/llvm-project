// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fcxx-exceptions \
// RUN:   -fexceptions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fcxx-exceptions \
// RUN:   -fexceptions -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-w64-mingw32 -fcxx-exceptions -fexceptions \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=MINGW %s

// On Windows Itanium and NT-POSIX the image that owns a type_info object is
// decided when the program is linked, so a reference to one that this
// translation unit does not define strongly is not dso_local: it takes the
// import form, which the linker makes direct when the owner is in the image.
// A strong definition, a hidden type's and an internal one's stay dso_local.

namespace std {
class type_info {
public:
  bool operator==(const type_info &) const;
};
} // namespace std

struct Key {
  virtual void f();
};
void Key::f() {}

struct NoKey {
  virtual void f() {}
};

struct External {
  virtual void f();
};

struct __attribute__((visibility("hidden"))) Hidden {
  virtual void f() {}
};

namespace {
struct Internal {
  virtual void f() {}
};
} // namespace

// CHECK-DAG: @_ZTI3Key = dso_local constant
// CHECK-DAG: @_ZTS3Key = dso_local constant
// CHECK-DAG: @_ZTI5NoKey = linkonce_odr constant
// CHECK-DAG: @_ZTS5NoKey = linkonce_odr constant
// CHECK-DAG: @_ZTI8External = external constant ptr
// CHECK-DAG: @_ZTI6Hidden = linkonce_odr hidden constant
// CHECK-DAG: @_ZTIN12_GLOBAL__N_18InternalE = internal constant
// CHECK-DAG: @_ZTIi = external constant ptr
// CHECK-DAG: @_ZTIPi = external constant ptr

// MINGW-DAG: @_ZTI5NoKey = linkonce_odr dso_local constant
// MINGW-DAG: @_ZTI8External = linkonce_odr dso_local constant

const std::type_info &key() { return typeid(Key); }
const std::type_info &noKey() { return typeid(NoKey); }
const std::type_info &external() { return typeid(External); }
const std::type_info &hidden() { return typeid(Hidden); }
const std::type_info &internal() { return typeid(Internal); }
const std::type_info &fundamental() { return typeid(int); }
const std::type_info &pointer() { return typeid(int *); }
void raise() { throw NoKey(); }
