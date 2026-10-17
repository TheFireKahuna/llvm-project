// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -O1 -disable-llvm-passes \
// RUN:   -emit-llvm -DPROVIDER -o - %s | FileCheck --check-prefix=PROVIDER %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fdeclspec -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -O1 -disable-llvm-passes \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=CONSUMER %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fdeclspec -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -O1 -disable-llvm-passes \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=CONSUMER %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fdeclspec -std=c++17 \
// RUN:   -mdefault-visibility-export-mapping=explicit -O1 -disable-llvm-passes \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=CONSUMER %s

/// A class whose key function carries a DLL attribute while the class does
/// not keeps its key function, so its vtable and type_info are emitted once,
/// in the image of the key function, which exports them with the vtable's
/// address-point names and its VTT. Another image imports a final class's
/// vtable and VTT and emits no copy, and constructs a class that is not final
/// through its own copy of the vtable, with the class's inline virtual
/// functions, whose type_info it imports. An exact dynamic_cast to a final
/// class compares with the one vtable in every image.

#ifdef PROVIDER
#define API __declspec(dllexport)
#else
#define API __declspec(dllimport)
#endif

struct Base {
  API virtual ~Base();
};
struct Final final : Base {
  API virtual void key();
};
struct Open : Base {
  API virtual void key();
};
struct Mid : virtual Base {
  API virtual void mid();
};
struct Deep final : Mid {
  API virtual void key();
};

#ifdef PROVIDER
Base::~Base() {}
void Final::key() {}
void Open::key() {}
void Mid::mid() {}
void Deep::key() {}
#else
Base *open() { return new Open; }
Base *deep() { return new Deep; }
#endif
Base *final() { return new Final; }
bool isFinal(Base *b) { return dynamic_cast<Final *>(b); }

// PROVIDER-DAG: @_ZTV5Final = dso_local dllexport constant
// PROVIDER-DAG: @_ZTI5Final = dso_local dllexport constant
// PROVIDER-DAG: @"_ZTV5Final$ap16" = dso_local dllexport alias
// PROVIDER-DAG: @_ZTV4Open = dso_local dllexport constant {{.*}}, comdat,
// PROVIDER-DAG: @_ZTI4Open = dso_local dllexport constant
// PROVIDER-DAG: @_ZTV4Deep = dso_local dllexport constant
// PROVIDER-DAG: @_ZTT4Deep = dso_local dllexport unnamed_addr constant
// PROVIDER-DAG: @_ZTI4Deep = dso_local dllexport constant

// CONSUMER-DAG: @_ZTV5Final = external dllimport constant
// CONSUMER-DAG: @_ZTV4Deep = external dllimport constant
// CONSUMER-DAG: @_ZTT4Deep = external dllimport unnamed_addr constant
// CONSUMER-DAG: @_ZTV4Open = linkonce_odr unnamed_addr constant {{.*}}, comdat,
// CONSUMER-DAG: @_ZTI4Open = external dllimport constant ptr
// CONSUMER-NOT: @_ZTV5Final = {{.*}}constant {
// CONSUMER-NOT: @_ZTI5Final = {{.*}}constant {

// PROVIDER-LABEL: define {{.*}} @_Z7isFinalP4Base(
// PROVIDER: icmp eq ptr %vtable, getelementptr inbounds inrange(-16, 24) (i8, ptr @_ZTV5Final, i64 16)
// CONSUMER-LABEL: define {{.*}} @_Z7isFinalP4Base(
// CONSUMER: icmp eq ptr %vtable, getelementptr inbounds inrange(-16, 24) (i8, ptr @_ZTV5Final, i64 16)
// CONSUMER: define linkonce_odr dso_local void @_ZN4OpenD0Ev(
