//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// REQUIRES: windows-dll
// UNSUPPORTED: no-exceptions, no-rtti, c++03

// objects point at them. Each copy is libc++abi's vtable word for word, which
// the DLL exports and the program finds by name at run time, since the name
// binds the copy at link time.

#include <cassert>
#include <cstddef>
#include <typeinfo>
#include <windows.h>

#define COPY(Var, Class) extern const void* const Var[] __asm__("_ZTVN10__cxxabiv1" Class "E")
COPY(fundamental, "23__fundamental_type_info");
COPY(array, "17__array_type_info");
COPY(function, "20__function_type_info");
COPY(enumeration, "16__enum_type_info");
COPY(pointer, "19__pointer_type_info");
COPY(pointer_to_member, "29__pointer_to_member_type_info");
COPY(class_, "17__class_type_info");
COPY(si_class, "20__si_class_type_info");
COPY(vmi_class, "21__vmi_class_type_info");

// The offset to top, the type_info, two destructors, two unused entries and
// can_catch; a class's has the three searches of a dynamic_cast as well.
const std::size_t leaf        = 7;
const std::size_t class_words = leaf + 3;

// The vtable that a type_info object points at, two words before its address
// point.
static const void* const* vtable(const std::type_info& ti) {
  return *reinterpret_cast<const void* const* const*>(&ti) - 2;
}

static void check(const void* const* copy, const char* name, std::size_t words) {
  HMODULE dll;
  BOOL found =
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<const char*>(&typeid(int)), &dll);
  assert(found);
  const void* const* owner = reinterpret_cast<const void* const*>(GetProcAddress(dll, name));
  assert(owner && owner != copy);
  for (std::size_t i = 0; i != words; ++i)
    assert(copy[i] == owner[i]);
}

struct Base {
  virtual ~Base() {}
};
struct Single : Base {};
struct Second {
  virtual ~Second() {}
};
struct Multiple : Base, Second {};
enum Enum {};

int main(int, char**) {
  check(fundamental, "_ZTVN10__cxxabiv123__fundamental_type_infoE", leaf);
  check(array, "_ZTVN10__cxxabiv117__array_type_infoE", leaf);
  check(function, "_ZTVN10__cxxabiv120__function_type_infoE", leaf);
  check(enumeration, "_ZTVN10__cxxabiv116__enum_type_infoE", leaf);
  check(pointer, "_ZTVN10__cxxabiv119__pointer_type_infoE", leaf);
  check(pointer_to_member, "_ZTVN10__cxxabiv129__pointer_to_member_type_infoE", leaf);
  check(class_, "_ZTVN10__cxxabiv117__class_type_infoE", class_words);
  check(si_class, "_ZTVN10__cxxabiv120__si_class_type_infoE", class_words);
  check(vmi_class, "_ZTVN10__cxxabiv121__vmi_class_type_infoE", class_words);

  // The type_info of a fundamental type is the DLL's, with the DLL's vtable.
  assert(vtable(typeid(int)) != fundamental);
  assert(vtable(typeid(Enum[2])) == array);
  assert(vtable(typeid(void())) == function);
  assert(vtable(typeid(Enum)) == enumeration);
  assert(vtable(typeid(Base*)) == pointer);
  assert(vtable(typeid(int Base::*)) == pointer_to_member);
  assert(vtable(typeid(Base)) == class_);
  assert(vtable(typeid(Single)) == si_class);
  assert(vtable(typeid(Multiple)) == vmi_class);

  // libc++abi calls through the copies.
  try {
    throw Single();
  } catch (Base&) {
  }
  try {
    throw static_cast<Single*>(nullptr);
  } catch (Base*) {
  }
  return 0;
}
