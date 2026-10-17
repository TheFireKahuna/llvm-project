//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-exceptions, no-rtti, c++03

// A type_info object with vague linkage has one instance among the images an
// import path joins, as on ELF: the executable takes owner.dll's for V, P<int>
// and D, and its own vtable of D holds owner.dll's type_info. sibling.dll and
// plugin.dll, which no link joins to owner.dll, keep their own, which compare
// equal by name, so that exceptions and casts work between them all.

// RUN: %{cxx} %{flags} %{compile_flags} -shared -DOWNER %s %{link_flags} -o %{temp}/owner.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DSIBLING %s %{link_flags} -o %{temp}/sibling.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DPLUGIN %s %{link_flags} -o %{temp}/plugin.dll
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} %{temp}/owner.dll.lib %{temp}/sibling.dll.lib -o %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe
// RUN: %{cxx} %{flags} %{compile_flags} -flto=thin %s %{link_flags} %{temp}/owner.dll.lib %{temp}/sibling.dll.lib -o %{temp}/lto.exe
// RUN: %{exec} %{temp}/lto.exe

#include <cassert>
#include <typeinfo>

#define API __attribute__((visibility("default")))

struct V {
  int x;
};
template <class T>
struct P {
  virtual ~P() {}
  T t;
};
struct D : P<int> {};

#if defined(OWNER)

API const std::type_info& ownerV() { return typeid(V); }
API const std::type_info& ownerD() { return typeid(D); }
API void ownerThrow() { throw V{1}; }
API P<int>* ownerMake() { return new D; }
API bool ownerIsD(P<int>* p) { return dynamic_cast<D*>(p) != nullptr; }

#elif defined(SIBLING)

API const std::type_info& siblingV() { return typeid(V); }
API void siblingThrow() { throw V{2}; }

#elif defined(PLUGIN)

extern "C" API const std::type_info* pluginV() { return &typeid(V); }
extern "C" API void pluginThrow() { throw V{3}; }

#else

#  include <windows.h>

const std::type_info& ownerV();
const std::type_info& ownerD();
void ownerThrow();
P<int>* ownerMake();
bool ownerIsD(P<int>* p);
const std::type_info& siblingV();
void siblingThrow();

int main(int, char**) {
  // Along the import path, one instance.
  assert(&typeid(V) == &ownerV());
  assert(&typeid(D) == &ownerD());
  P<int>* local = new D;
  assert(&typeid(*local) == &ownerD());
  assert(ownerIsD(local));
  assert(dynamic_cast<D*>(ownerMake()) != nullptr);
  try {
    ownerThrow();
    assert(false);
  } catch (V& v) {
    assert(v.x == 1);
  }

  // Between images that no link joins, equal by name.
  assert(&typeid(V) != &siblingV());
  assert(typeid(V) == siblingV());
  assert(typeid(V).hash_code() == siblingV().hash_code());
  assert(!typeid(V).before(siblingV()) && !siblingV().before(typeid(V)));
  try {
    siblingThrow();
    assert(false);
  } catch (V& v) {
    assert(v.x == 2);
  }

  HMODULE plugin = LoadLibraryA("plugin.dll");
  assert(plugin);
  auto pluginV     = reinterpret_cast<const std::type_info* (*)()>(GetProcAddress(plugin, "pluginV"));
  auto pluginThrow = reinterpret_cast<void (*)()>(GetProcAddress(plugin, "pluginThrow"));
  assert(pluginV && pluginThrow);
  assert(&typeid(V) != pluginV());
  assert(typeid(V) == *pluginV());
  try {
    pluginThrow();
    assert(false);
  } catch (V& v) {
    assert(v.x == 3);
  }
  assert(typeid(V) != typeid(D));
  return 0;
}

#endif
