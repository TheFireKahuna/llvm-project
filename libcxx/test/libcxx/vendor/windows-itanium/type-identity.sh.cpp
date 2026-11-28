//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-exceptions, no-rtti, c++03

// A class's type_info objects in images that no link joins, here a plugin that
// the executable loads, are equal when the class has one name and one layout,
// and unequal when its layouts differ, whichever of catch, dynamic_cast and
// typeid compares them. A handler that converts a pointer compares the classes
// by name where the thrower saw the class incomplete, and a class of a name
// and no layout, which an image that never completes it holds, matches the
// class of that name. Two layouts of one class in one image fail its link,
// with and without LTO.

// RUN: %{cxx} %{flags} %{compile_flags} -DPLUGIN -c %s -o %{temp}/plugin.o
// RUN: %{cxx} %{flags} %{compile_flags} -DTHROWER -c %s -o %{temp}/thrower.o
// RUN: %{cxx} %{flags} -shared %{temp}/plugin.o %{temp}/thrower.o %{link_flags} -o %{temp}/plugin.dll
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} -o %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe plugin.dll
// RUN: %{cxx} %{flags} %{compile_flags} -flto=thin %s %{link_flags} -o %{temp}/thin.exe
// RUN: %{exec} %{temp}/thin.exe plugin.dll

// RUN: %{cxx} %{flags} %{compile_flags} -DPLUGIN -DSECOND -c %s -o %{temp}/two.o
// RUN: not %{cxx} %{flags} -shared %{temp}/plugin.o %{temp}/two.o \
// RUN:   %{link_flags} -o %{temp}/two.dll 2>&1 | grep "typeinfo name for Same\|COMDAT ._ZTS4Same. differs"
// RUN: %{cxx} %{flags} %{compile_flags} -DPLUGIN -flto -c %s -o %{temp}/plugin.lto.o
// RUN: %{cxx} %{flags} %{compile_flags} -DPLUGIN -DSECOND -flto -c %s -o %{temp}/two.lto.o
// RUN: not %{cxx} %{flags} -shared -flto %{temp}/plugin.lto.o %{temp}/two.lto.o \
// RUN:   %{link_flags} -o %{temp}/two.dll 2>&1 | grep "typeinfo name for Same\|COMDAT ._ZTS4Same. differs"
// RUN: %{cxx} %{flags} %{compile_flags} -DPLUGIN -flto=thin -c %s -o %{temp}/plugin.thin.o
// RUN: %{cxx} %{flags} %{compile_flags} -DPLUGIN -DSECOND -flto=thin -c %s -o %{temp}/two.thin.o
// RUN: not %{cxx} %{flags} -shared -flto=thin %{temp}/plugin.thin.o %{temp}/two.thin.o \
// RUN:   %{link_flags} -o %{temp}/two.dll 2>&1 | grep "typeinfo name for Same\|COMDAT ._ZTS4Same. differs"
// RUN: not %{cxx} %{flags} -shared -flto=thin %{temp}/plugin.o %{temp}/two.thin.o \
// RUN:   %{link_flags} -o %{temp}/two.dll 2>&1 | grep "typeinfo name for Same\|COMDAT ._ZTS4Same. differs"

#include <typeinfo>

#define EXPORT extern "C" __declspec(dllexport)

#if defined(THROWER)

// This unit of the plugin sees every class incomplete, as one that only passes
// pointers on does, so its type_info objects for pointers carry the ABI's
// incomplete flags.
struct Same;
struct Prefix;
struct Trail;
struct Bare;
extern void* objects[];
template <class T>
static void raise(int i) {
  static T* p = static_cast<T*>(objects[i]);
  throw &p;
}
EXPORT void throwSame() { raise<Same>(0); }
EXPORT void throwPrefix() { raise<Prefix>(1); }
EXPORT void throwTrail() { raise<Trail>(2); }
EXPORT void throwBare() { raise<Bare>(3); }

#else

// Same has one layout in both images. The plugin's Prefix and Trail add a
// member at the end, so that the executable's shape starts the plugin's, and
// Trail's is one zero byte, its alignment and a char. The second unit of a
// plugin gives Same another layout.
struct Same {
#  if defined(SECOND)
  int a;
#  else
  long long a;
  int b;
#  endif
};
struct Bare {
  int x;
};
struct Prefix {
  int a;
  short b;
  char c;
#  if defined(PLUGIN)
  int d;
#  endif
};
struct Trail {
  char a;
#  if defined(PLUGIN)
  char b[2];
#  endif
};
struct Poly {
  virtual ~Poly() {}
  long long a;
  int b;
#  if defined(PLUGIN)
  short c;
#  endif
};
struct PolyFinal final : Poly {
  int c;
};
struct PolyOther : Poly {
  int c;
};
struct PolySame {
  virtual ~PolySame() {}
  int a;
};
struct PolySameFinal final : PolySame {
  int c;
};

#  if defined(PLUGIN) && defined(SECOND)

EXPORT const std::type_info* tiSecond() { return &typeid(Same); }

#  elif defined(PLUGIN)

static Same same;
static Prefix prefix;
static Trail trail;
void* objects[] = {&same, &prefix, &trail, &same};
EXPORT const std::type_info* tiSame() { return &typeid(Same); }
EXPORT const std::type_info* tiPrefix() { return &typeid(Prefix); }
EXPORT const std::type_info* tiTrail() { return &typeid(Trail); }
EXPORT Poly* makePolyFinal() { return new PolyFinal; }
EXPORT Poly* makePolyOther() { return new PolyOther; }
EXPORT PolySame* makePolySameFinal() { return new PolySameFinal; }

#  else

#    include <cassert>
#    include <windows.h>

static HMODULE plugin;
template <class T>
static T* get(const char* name) {
  return reinterpret_cast<T*>(GetProcAddress(plugin, name));
}

// A handler that adds const at both levels of a pointer to a pointer compares
// the classes.
template <class T>
static bool caught(const char* name) {
  try {
    get<void()>(name)();
  } catch (const T* const*) {
    return true;
  } catch (...) {
  }
  return false;
}

// Equal, with one hash and no order between the two, or unequal with an
// order.
template <class T>
static bool same(const char* name) {
  const std::type_info &a = *get<const std::type_info*()>(name)(), &b = typeid(T);
  if (a != b) {
    assert(a.before(b) != b.before(a));
    return false;
  }
  assert(a.hash_code() == b.hash_code() && !a.before(b) && !b.before(a));
  return true;
}

int main(int, char** argv) {
  plugin = LoadLibraryA(argv[1]);
  assert(plugin);
  assert(caught<Same>("throwSame"));
  assert(!caught<Prefix>("throwPrefix"));
  assert(!caught<Trail>("throwTrail"));
  assert(caught<Bare>("throwBare"));
  assert(same<Same>("tiSame"));
  assert(!same<Prefix>("tiPrefix"));
  assert(!same<Trail>("tiTrail"));
  assert(!dynamic_cast<PolyFinal*>(get<Poly*()>("makePolyFinal")()));
  assert(!dynamic_cast<PolyOther*>(get<Poly*()>("makePolyOther")()));
  assert(dynamic_cast<PolySameFinal*>(get<PolySame*()>("makePolySameFinal")()));
  return 0;
}

#  endif
#endif
