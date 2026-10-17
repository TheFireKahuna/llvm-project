// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import \
// RUN:   -fdeclspec -mdefault-visibility-export-mapping=explicit \
// RUN:   -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,COFF %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import \
// RUN:   -fdeclspec -mdefault-visibility-export-mapping=all \
// RUN:   -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,COFF %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-auto-import \
// RUN:   -fdeclspec -mdefault-visibility-export-mapping=explicit \
// RUN:   -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,COFF %s
// RUN: %clang_cc1 -triple powerpc64-ibm-aix \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,AIX %s

/// On COFF the visibility mapping does not export a discardable definition
/// other than a variable: an image has one only if it used it, and every
/// image that uses it emits its own copy. Variables the source declared, with
/// their guards, explicit instantiations and non-discardable definitions are
/// exported as before, and so is an inline function marked dllexport, which
/// clang makes weak_odr.

#define API __attribute__((visibility("default")))

namespace std { class type_info; }

/// A key function: the vtable and RTTI are strong.
struct API K { virtual ~K(); };
K::~K() {}

/// No key function: the vtable, RTTI, members and the VTT of a class with a
/// virtual base are linkonce_odr, as are the construction vtables of a class
/// deriving from it.
struct API C { virtual void g() {} };
struct API VB : virtual C { virtual void h() {} };
struct API D : VB {};

template <class T> struct API Tm {
  static int sm;
  static inline int si = 1;
  T v() { return T(); }
};
template <class T> int Tm<T>::sm = 3;
template struct Tm<long>;

API inline int inl() { return 7; }
API inline int sloc() { static int s = inl(); return s; }
API inline int iv = 5;
#ifdef _WIN32
__declspec(dllexport) inline int marked() { return 9; }
#else
inline int marked() { return 9; }
#endif

API const std::type_info &use() {
  D d;
  (void)(inl() + sloc() + iv + Tm<int>::sm + Tm<int>::si + Tm<int>().v() +
         marked());
  return typeid(C);
}

// COFF-DAG: @_ZTV1C = linkonce_odr dso_local unnamed_addr constant
// COFF-DAG: @_ZTI1C = linkonce_odr constant
// COFF-DAG: @_ZTS1C = linkonce_odr constant
// COFF-DAG: @_ZTV1D = linkonce_odr dso_local unnamed_addr constant
// COFF-DAG: @_ZTT1D = linkonce_odr dso_local unnamed_addr constant
// COFF-DAG: @_ZTC1D0_2VB = linkonce_odr dso_local unnamed_addr constant
// COFF-DAG: define linkonce_odr dso_local noundef i32 @_Z3inlv()
// COFF-DAG: define linkonce_odr dso_local noundef i32 @_Z4slocv()
// COFF-DAG: define linkonce_odr dso_local noundef i32 @_ZN2TmIiE1vEv(
// COFF-DAG: define linkonce_odr dso_local void @_ZN1C1gEv(
// AIX-DAG:  @_ZTV1C = linkonce_odr dllexport constant
// AIX-DAG:  define linkonce_odr dllexport noundef signext i32 @_Z3inlv()

// CHECK-DAG: @_ZTV1K = {{.*}}dllexport constant
// CHECK-DAG: @_ZTI1K = {{.*}}dllexport constant
// CHECK-DAG: @_ZN2TmIlE2smE = weak_odr {{.*}}dllexport global
// CHECK-DAG: @_ZN2TmIiE2smE = linkonce_odr {{.*}}dllexport global
// CHECK-DAG: @_ZN2TmIiE2siE = linkonce_odr {{.*}}dllexport global
// CHECK-DAG: @iv = linkonce_odr {{.*}}dllexport global
// CHECK-DAG: @_ZZ4slocvE1s = linkonce_odr {{.*}}dllexport global
// CHECK-DAG: @_ZGVZ4slocvE1s = linkonce_odr {{.*}}dllexport global
// CHECK-DAG: define weak_odr {{.*}}dllexport {{.*}}@_ZN2TmIlE1vEv(
// COFF-DAG:  define weak_odr {{.*}}dllexport {{.*}}@_Z6markedv()
