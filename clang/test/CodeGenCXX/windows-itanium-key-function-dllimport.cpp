// DEFINE: %{cc} = %clang_cc1 -fdeclspec -std=c++17 -O1 -disable-llvm-passes -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -DPROVIDER | FileCheck --check-prefix=PROVIDER %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix -DPROVIDER | FileCheck --check-prefix=PROVIDER %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck --check-prefix=CONSUMER %s
// RUN: %{cc} -triple aarch64-unknown-windows-itanium | FileCheck --check-prefix=CONSUMER %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix | FileCheck --check-prefix=CONSUMER %s

/// A class whose key function carries a DLL attribute while the class does
/// not keeps its key function, so its vtable and type_info are emitted once,
/// in the image that defines the key function, which exports them with the
/// class's VTT. Another image imports all three and emits none, so an exact
/// dynamic_cast compares with the one vtable in every image.

#ifdef PROVIDER
#define API __declspec(dllexport)
#else
#define API __declspec(dllimport)
#endif

namespace std {
class type_info;
} // namespace std

struct Base {
  API virtual ~Base();
};
struct Final final : Base {
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
void Mid::mid() {}
void Deep::key() {}
#else
Base *deep() { return new Deep; }
const std::type_info &mid() { return typeid(Mid); }
#endif
Base *final() { return new Final; }
bool isFinal(Base *b) { return dynamic_cast<Final *>(b); }

// PROVIDER-DAG: @_ZTV5Final = dso_local dllexport constant
// PROVIDER-DAG: @_ZTI5Final = dso_local dllexport constant
// PROVIDER-DAG: @_ZTV4Deep = dso_local dllexport constant
// PROVIDER-DAG: @_ZTT4Deep = dso_local dllexport unnamed_addr constant
// PROVIDER-DAG: @_ZTI4Deep = dso_local dllexport constant

// CONSUMER-DAG: @_ZTV5Final = external dllimport constant
// CONSUMER-DAG: @_ZTV4Deep = external dllimport constant
// CONSUMER-DAG: @_ZTT4Deep = external dllimport unnamed_addr constant
// CONSUMER-DAG: @_ZTI3Mid = external dllimport constant ptr
// CONSUMER-NOT: @_ZT{{[VI]}}5Final = {{.*}}constant {

// PROVIDER-LABEL: define {{.*}} @_Z7isFinalP4Base(
// PROVIDER: icmp eq ptr %vtable, getelementptr inbounds inrange(-16, 24) (i8, ptr @_ZTV5Final, i64 16)
// CONSUMER-LABEL: define {{.*}} @_Z7isFinalP4Base(
// CONSUMER: icmp eq ptr %vtable, getelementptr inbounds inrange(-16, 24) (i8, ptr @_ZTV5Final, i64 16)
