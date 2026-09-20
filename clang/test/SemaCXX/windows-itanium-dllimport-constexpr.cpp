// RUN: %clang_cc1 -std=c++17 %s -verify -triple x86_64-unknown-windows-itanium -fdeclspec
// RUN: %clang_cc1 -std=c++17 %s -verify -triple x86_64-pc-windows-ntposix -fdeclspec
// RUN: %clang_cc1 -std=c++17 %s -verify -triple x86_64-windows-msvc -fms-extensions -DMSVC
// expected-no-diagnostics

// Static data that holds an imported address is filled in place by the
// loader on Windows Itanium and NT-POSIX, so the address of a dllimport
// entity is a constant expression, as on ELF. MSVC targets keep the thunk
// and import-table rules of dllimport-constexpr.cpp.

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
constexpr int *constexpr_import_element = &imported_array[2];
constexpr void (Foo::*constexpr_memptr)() = &Foo::imported_method;
static_assert(constexpr_import_int != nullptr);
static_assert(constexpr_import_element == imported_array + 2);

template <void (*FP)()> struct StaticConstexpr {
  static constexpr void (*g_fp)() = FP;
};
void instantiate() { StaticConstexpr<imported_func>::g_fp(); }
#endif

// A virtual dllimport member function is a constant member pointer everywhere.
constexpr void (Foo::*constexpr_virtual_memptr)() = &Foo::imported_virtual;
