//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-rtti, c++03

// A DLL exports a type_info object it owns under the name that its type's
// layout qualifies, which an executable's import of the object binds: the
// executable loads beside the DLL rebuilt with other code, but not beside one
// rebuilt with another layout of the type. An executable that delay-loads the
// DLL, and so imports no data from it, imports the name as a witness, which
// fails its first call into the DLL rebuilt with another layout.

// RUN: mkdir -p %{temp}/same %{temp}/wide
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DOWNER %s %{link_flags} -o %{temp}/owner.dll
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} %{temp}/owner.dll.lib -o %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe
// RUN: %{cxx} %{flags} %{compile_flags} -DDELAY %s %{link_flags} %{temp}/owner.dll.lib -Wl,-delayload:owner.dll -o %{temp}/delay.exe
// RUN: %{exec} %{temp}/delay.exe
// RUN: cp %{temp}/main.exe %{temp}/delay.exe %{temp}/same/
// RUN: cp %{temp}/main.exe %{temp}/delay.exe %{temp}/wide/
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DOWNER -DOTHER_CODE %s %{link_flags} -o %{temp}/same/owner.dll
// RUN: %{cxx} %{flags} %{compile_flags} -shared -DOWNER -DWIDE %s %{link_flags} -o %{temp}/wide/owner.dll
// RUN: %{exec} %{temp}/same/main.exe
// RUN: %{exec} %{temp}/same/delay.exe
// RUN: not %{exec} %{temp}/wide/main.exe
// RUN: not %{exec} %{temp}/wide/delay.exe

#include <cassert>
#include <typeinfo>

#define API __attribute__((visibility("default")))

struct API K {
  virtual int key() const;
  int x = 1;
#ifdef WIDE
  int extra = 0;
#endif
};

API K* make();

#ifdef OWNER

#  ifdef OTHER_CODE
int K::key() const { return x + 1; }
#  else
int K::key() const { return x; }
#  endif
K* make() { return new K; }

#else

int main(int, char**) {
  K* k = make();
#  ifndef DELAY
  assert(typeid(*k) == typeid(K));
  assert(&typeid(*k) == &typeid(K));
#  endif
  assert(k->key() > 0);
  return 0;
}

#endif
