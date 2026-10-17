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

// A program linked against the shared library takes its own copies of
// libc++abi's type_info vtables from the import library, and its type_info
// objects point at them. Each copy is libc++abi's vtable word for word: the
// same offset-to-top, the same type_info, which the copy imports, and the same
// entries, the library's exported functions. The library's own vtable is
// reached through the name of its address point, which it still exports, since
// a reference to the vtable's own name binds the copy.

#include <cassert>
#include <cstddef>
#include <typeinfo>

#define VTABLE(Var, Class)                                                     \
  extern "C++" const void* const Var[] __asm__("_ZTVN10__cxxabiv1" Class "E"); \
  extern "C++" const void* const* const Var##_dll __asm__("__imp__ZTVN10__cxxabiv1" Class "E$ap16");

VTABLE(fundamental, "23__fundamental_type_info")
VTABLE(array, "17__array_type_info")
VTABLE(function, "20__function_type_info")
VTABLE(enumeration, "16__enum_type_info")
VTABLE(class_, "17__class_type_info")
VTABLE(si_class, "20__si_class_type_info")
VTABLE(vmi_class, "21__vmi_class_type_info")
VTABLE(pointer, "19__pointer_type_info")
VTABLE(pointer_to_member, "29__pointer_to_member_type_info")

static void check(const void* const* copy, const void* const* dll_point, std::size_t words) {
  const void* const* dll = dll_point - 2;
  assert(copy != dll);
  for (std::size_t i = 0; i != words; ++i)
    assert(copy[i] == dll[i]);
}

// The vtable pointer of a type_info object, which points at the address point,
// two words into the vtable.
static const void* const* vptr(const std::type_info& ti) {
  return *reinterpret_cast<const void* const* const*>(&ti) - 2;
}

struct Base {
  virtual ~Base();
};
Base::~Base() {}
struct Single : Base {};
struct Second {
  virtual ~Second();
};
Second::~Second() {}
struct Multiple : Base, Second {};
enum E { e };

int main(int, char**) {
  // Offset-to-top, the type_info and the five entries every kind has.
  const std::size_t leaf = 7;
  // The class kinds add the three searches of a dynamic_cast.
  const std::size_t class_kind = leaf + 3;
  check(fundamental, fundamental_dll, leaf);
  check(array, array_dll, leaf);
  check(function, function_dll, leaf);
  check(enumeration, enumeration_dll, leaf);
  check(class_, class__dll, class_kind);
  check(si_class, si_class_dll, class_kind);
  check(vmi_class, vmi_class_dll, class_kind);
  check(pointer, pointer_dll, leaf);
  check(pointer_to_member, pointer_to_member_dll, leaf);

  assert(vptr(typeid(Base)) == class_);
  assert(vptr(typeid(Single)) == si_class);
  assert(vptr(typeid(Multiple)) == vmi_class);
  assert(vptr(typeid(E)) == enumeration);
  assert(vptr(typeid(Base*)) == pointer);
  assert(vptr(typeid(int Base::*)) == pointer_to_member);
  assert(vptr(typeid(int[2])) == array);
  assert(vptr(typeid(void())) == function);

  // A thrown object is still caught through the copies, which the library
  // reaches by virtual calls.
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
