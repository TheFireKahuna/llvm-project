// The visibility-to-DLL-storage mapping in both directions: an explicit
// default visibility exports a definition and imports a declaration. An
// implicit visibility says nothing about a declaration. On COFF a discardable
// function is not exported by the mapping; weak data is.

// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -fno-auto-import -emit-llvm -o - %s | FileCheck %s -check-prefixes=CHECK,MAPPED
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=all -fno-auto-import -emit-llvm -o - %s | FileCheck %s -check-prefixes=CHECK,MAPPED
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -emit-llvm -o - %s | FileCheck %s -check-prefixes=CHECK,NONE

#define VIS __attribute__((visibility("default")))

VIS extern int marked_data;
extern int plain_data;
VIS extern thread_local int marked_tls;
VIS void marked_fn();
void plain_fn();
__attribute__((visibility("hidden"))) void hidden_fn();
VIS void defined_later();
VIS inline void marked_inline() {}
VIS void marked_def() {}
template <class T> VIS void tmpl(T) {}
extern template void tmpl<int>(int);
template void tmpl<long long>(long long);

struct VIS Poly {
  virtual void key();
};
struct VIS Inline {
  virtual ~Inline() {}
};

void use() {
  marked_fn();
  plain_fn();
  hidden_fn();
  defined_later();
  marked_inline();
  tmpl(1);
  tmpl(1LL);
  Poly p;
  p.key();
  Inline i;
  marked_data = plain_data = marked_tls;
}

VIS void defined_later() {}

// MAPPED-DAG: @marked_data = external dllimport global i32
// NONE-DAG:   @marked_data = external dso_local global i32
// CHECK-DAG:  @plain_data = external dso_local global i32
// A thread-local variable is never imported; a marked one is reached through
// the record its image exports.
// MAPPED-DAG: @"marked_tls$tls" = external dllimport constant { ptr, i32, i32 }
// NONE-DAG:   @marked_tls = external dso_local thread_local global i32
// MAPPED-DAG: @_ZTV4Poly = external dllimport unnamed_addr constant
// NONE-DAG:   @_ZTV4Poly = external dso_local unnamed_addr constant
// MAPPED-DAG: @_ZTV6Inline = linkonce_odr dso_local dllexport unnamed_addr constant
// NONE-DAG:   @_ZTV6Inline = linkonce_odr dso_local unnamed_addr constant

// MAPPED-DAG: declare dllimport void @_Z9marked_fnv()
// NONE-DAG:   declare dso_local void @_Z9marked_fnv()
// CHECK-DAG:  declare dso_local void @_Z8plain_fnv()
// CHECK-DAG:  declare hidden void @_Z9hidden_fnv()
// MAPPED-DAG: define dso_local dllexport void @_Z13defined_laterv()
// NONE-DAG:   define dso_local void @_Z13defined_laterv()
// CHECK-DAG:  define linkonce_odr dso_local void @_Z13marked_inlinev()
// MAPPED-DAG: define dso_local dllexport void @_Z10marked_defv()
// MAPPED-DAG: declare dllimport void @_Z4tmplIiEvT_(i32
// NONE-DAG:   declare dso_local void @_Z4tmplIiEvT_(i32
// MAPPED-DAG: define weak_odr dso_local dllexport void @_Z4tmplIxEvT_(i64
// NONE-DAG:   define weak_odr dso_local void @_Z4tmplIxEvT_(i64
// MAPPED-DAG: declare dllimport void @_ZN4Poly3keyEv(
// NONE-DAG:   declare dso_local void @_ZN4Poly3keyEv(
// CHECK-DAG:  define linkonce_odr dso_local void @_ZN6InlineD2Ev(
