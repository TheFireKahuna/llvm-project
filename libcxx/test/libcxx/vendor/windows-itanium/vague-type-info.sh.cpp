//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-exceptions, no-rtti, c++03

// A type_info object of vague linkage has one instance among the images that
// an import path joins: the executable takes owner.dll's for V and D, and its
// own vtable of D holds owner.dll's. sibling.dll, which no link joins to
// owner.dll, keeps its own, whose name word its fill binds to owner.dll's, as
// does plugin.dll's, loaded at run time: equality, hashing and ordering then
// stop at the name, and exceptions and casts work between them all. Two
// plugins that keep a type of no initial image compare equal by name, and one
// that keeps another layout of V compares unequal. A link that joins two
// layouts of V along an import path fails.

// RUN: %{cxx} %{flags} %{compile_flags} -shared -DOWNER %s %{link_flags} -o %{temp}/owner.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DSIBLING %s %{link_flags} -o %{temp}/sibling.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DPLUGIN=1 %s %{link_flags} -o %{temp}/plugin1.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DPLUGIN=2 %s %{link_flags} -o %{temp}/plugin2.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DPLUGIN=3 -DWIDE %s %{link_flags} -o %{temp}/wide.dll
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} %{temp}/owner.dll.lib %{temp}/sibling.dll.lib -o %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe
// RUN: %{cxx} %{flags} %{compile_flags} -flto=thin %s %{link_flags} %{temp}/owner.dll.lib %{temp}/sibling.dll.lib -o %{temp}/thin.exe
// RUN: %{exec} %{temp}/thin.exe
// RUN: %{cxx} %{flags} %{compile_flags} -DWIDE -c %s -o %{temp}/wide.o
// RUN: not %{cxx} %{flags} %{temp}/wide.o %{link_flags} %{temp}/owner.dll.lib %{temp}/sibling.dll.lib \
// RUN:   -o %{temp}/wide.exe 2>&1 | grep "typeinfo for V is a type whose layout hashes to"

#include <cassert>
#include <typeinfo>

#define API __attribute__((visibility("default")))

struct V {
  int x;
#ifdef WIDE
  int y;
#endif
};
template <class T>
struct P {
  virtual ~P() {}
  T t;
};
struct D : P<int> {};
struct Q {};

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
extern "C" API const std::type_info* pluginQ() { return &typeid(Q); }
extern "C" API void pluginThrow() {
  V v = {};
  v.x = PLUGIN;
  throw v;
}

#else

#  include <windows.h>

const std::type_info& ownerV();
const std::type_info& ownerD();
void ownerThrow();
P<int>* ownerMake();
bool ownerIsD(P<int>* p);
const std::type_info& siblingV();
void siblingThrow();

struct Plugin {
  const std::type_info* (*v)();
  const std::type_info* (*q)();
  void (*raise)();
};

static Plugin load(const char* name) {
  HMODULE m = LoadLibraryA(name);
  assert(m);
  Plugin p = {reinterpret_cast<const std::type_info* (*)()>(GetProcAddress(m, "pluginV")),
              reinterpret_cast<const std::type_info* (*)()>(GetProcAddress(m, "pluginQ")),
              reinterpret_cast<void (*)()>(GetProcAddress(m, "pluginThrow"))};
  assert(p.v && p.q && p.raise);
  return p;
}

// Two descriptors of one type in images that no link joins: equal, by the
// name word that the later image's fill bound.
static void same(const std::type_info& a, const std::type_info& b) {
  assert(&a != &b);
  assert(a.name() == b.name());
  assert(a == b);
  assert(a.hash_code() == b.hash_code());
  assert(!a.before(b) && !b.before(a));
}

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

  // A true sibling, which the executable's link sees.
  same(typeid(V), siblingV());
  try {
    siblingThrow();
    assert(false);
  } catch (V& v) {
    assert(v.x == 2);
  }

  // Plugins, which bind to the initial images' name words where those keep
  // the type, and otherwise compare equal by name.
  Plugin p1 = load("plugin1.dll"), p2 = load("plugin2.dll");
  same(typeid(V), *p1.v());
  same(*p1.v(), *p2.v());
  assert(p1.q()->name() != p2.q()->name());
  assert(*p1.q() == *p2.q());
  assert(p1.q()->hash_code() == p2.q()->hash_code());
  assert(!p1.q()->before(*p2.q()) && !p2.q()->before(*p1.q()));
  try {
    p1.raise();
    assert(false);
  } catch (V& v) {
    assert(v.x == 1);
  }
  try {
    p2.raise();
    assert(false);
  } catch (V& v) {
    assert(v.x == 2);
  }

  // Another layout of V under its name is another type.
  Plugin wide = load("wide.dll");
  assert(wide.v()->name() != typeid(V).name());
  assert(*wide.v() != typeid(V));
  try {
    wide.raise();
    assert(false);
  } catch (V&) {
    assert(false);
  } catch (...) {
  }
  assert(typeid(V) != typeid(D));
  return 0;
}

#endif
