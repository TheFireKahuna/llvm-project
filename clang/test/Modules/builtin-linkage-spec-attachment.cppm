// An implicitly declared builtin is attached to the global module, so a
// redeclaration in a linkage-specification in a module unit's purview, which
// [module.unit]p7.2 also attaches to the global module, agrees with it.
//
// RUN: rm -rf %t
// RUN: split-file %s %t
//
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fms-extensions %t/iface.cppm -emit-module-interface -o %t/m.pcm -verify
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fms-extensions %t/priv.cppm -fsyntax-only -verify
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fms-extensions %t/impl.cpp -fmodule-file=m=%t/m.pcm -fsyntax-only -verify

//--- iface.cppm
// expected-no-diagnostics
export module m;
void *use() { return _alloca(16); }
extern "C" void *_alloca(__SIZE_TYPE__);
extern "C" void __debugbreak();

//--- priv.cppm
// expected-no-diagnostics
export module m;
module :private;
extern "C" void *_alloca(__SIZE_TYPE__);

//--- impl.cpp
// expected-no-diagnostics
module m;
extern "C" void *_alloca(__SIZE_TYPE__);
