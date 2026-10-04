// RUN: %clang_cc1 -std=c++17 %s -verify=slots -triple x86_64-unknown-windows-itanium -fdeclspec
// RUN: %clang_cc1 -std=c++17 %s -verify=slots -triple aarch64-unknown-windows-itanium -fdeclspec
// RUN: %clang_cc1 -std=c++17 %s -verify=slots -triple x86_64-pc-windows-ntposix -fdeclspec
// RUN: %clang_cc1 -std=c++17 %s -verify=slots -triple x86_64-unknown-windows-itanium -fdeclspec -fexperimental-new-constant-interpreter
// RUN: %clang_cc1 -std=c++17 %s -verify=msvc -triple x86_64-windows-msvc -fms-extensions -DMSVC
// msvc-no-diagnostics

// On Windows Itanium and NT-POSIX the loader writes an imported address into
// the static data that names it, so the address of a dllimport entity is a
// constant expression, as on ELF. The loader adds no offset, so an address
// inside the entity is not. MSVC targets keep the rules of
// dllimport-constexpr.cpp.

__declspec(dllimport) void imported_func();
__declspec(dllimport) int imported_int;
__declspec(dllimport) int imported_array[4];
struct Foo {
  void __declspec(dllimport) imported_method();
  virtual void __declspec(dllimport) imported_virtual();
};

#ifndef MSVC
constexpr void (*constexpr_import_func)() = &imported_func;
constexpr int *constexpr_import_int = &imported_int;
constexpr int *constexpr_import_first = &imported_array[0];
constexpr void (Foo::*constexpr_memptr)() = &Foo::imported_method;
static_assert(constexpr_import_int != nullptr);
static_assert(constexpr_import_first == imported_array);

template <void (*FP)()> struct StaticConstexpr {
  static constexpr void (*g_fp)() = FP;
};
void instantiate() { StaticConstexpr<imported_func>::g_fp(); }

constexpr int *constexpr_import_element = &imported_array[2]; // slots-error {{constexpr variable 'constexpr_import_element' must be initialized by a constant expression}}
#endif

// A virtual dllimport member function is a constant member pointer everywhere.
constexpr void (Foo::*constexpr_virtual_memptr)() = &Foo::imported_virtual;
