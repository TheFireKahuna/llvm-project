// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s

// On Windows Itanium and NT-POSIX the name of a non-unique type_info object
// for a class is followed by the class's layout shape: its size and the log2
// of its alignment, then every leaf, by-value members and bases flattened in,
// each a byte whose low four bits are its kind and whose high four bits are
// the bits of padding ahead of it, or 15 ahead of a ULEB128 count of them. A
// run of equal leaves, as an array's elements are, is one array item: its
// count, the element's items and an end. The kinds: 0 to 4 integers of 1 to 16
// bytes, 5 float, 6 double, 8 pointer, 9 vtable pointer, 10 member function
// pointer, 11 bit-field and its width, 12 array, 13 union and its count of
// alternatives, 14 end. Where a leaf's kind leaves its type open, as a
// pointer's, a member pointer's or an enumeration's does, an end and the
// field-type signature follow: those types, mangled together as the
// parameters of a function type after the class, whose substitutions write a
// repeated name once and may refer to the parts of the class's name, which is
// left out.

namespace std {
class type_info;
} // namespace std

// Size 48, alignment 8; the vtable pointer, an int, 32 bits of padding and a
// double, the array of three chars, 8 bits of padding and four ints as one run.
struct Leaves {
  virtual ~Leaves() {}
  int a;
  double d;
  char c[3];
  int x, y, z, w;
};
// CHECK-DAG: @_ZTS6Leaves = linkonce_odr constant [22 x i8] c"6Leaves\000\03\09\02\F6 \0C\03\00\0E\8C\04\02\0E"

// The same layout under other names is the same shape.
struct Renamed {
  virtual ~Renamed() {}
  int b;
  double e;
  char f[3];
  int p[4];
};
// CHECK-DAG: @_ZTS7Renamed = linkonce_odr constant [23 x i8] c"7Renamed\000\03\09\02\F6 \0C\03\00\0E\8C\04\02\0E"

// A base's leaves at its offset, a pointer, then two bit-fields of 3 and 5
// bits.
struct Base {
  virtual ~Base() {}
};
struct Derived : Base {
  void *p;
  unsigned b1 : 3, b2 : 5;
};
// CHECK-DAG: @_ZTS7Derived = linkonce_odr constant [20 x i8] c"7Derived\00\18\03\09\08\0B\03\0B\05\0EPv"

// A union of an int and a float, 32 bits of padding, a member function
// pointer and an array of ten shorts.
union U {
  int i;
  float f;
};
struct Members {
  virtual ~Members() {}
  U u;
  int (Derived::*m)();
  short s[10];
};
// CHECK-DAG: @_ZTS7Members = linkonce_odr constant [38 x i8] c"7Members\008\03\09\0D\02\02\0E\05\0E\FA \0C\0A\01\0E\0EM7DerivedFivE"

// One layout, but a pointer to another class and an integer for an
// enumeration: the field-type signatures differ, and a repeated name is a
// substitution.
struct Foo;
struct Bar;
enum Color : int { Red };
struct Typed {
  virtual ~Typed() {}
  Foo *a, *b;
  Color c;
  const char *s;
};
struct Retyped {
  virtual ~Retyped() {}
  Foo *a;
  Bar *b;
  int c;
  const char *s;
};
// CHECK-DAG: @_ZTS5Typed = linkonce_odr constant [35 x i8] c"5Typed\00(\03\09\0C\02\08\0E\02\F8 \0EP3FooS1_5ColorPKc"
// CHECK-DAG: @_ZTS7Retyped = linkonce_odr constant [33 x i8] c"7Retyped\00(\03\09\0C\02\08\0E\02\F8 \0EP3FooP3BarPKc"

// A unique type_info object's name and a pointer's are followed by nothing.
struct Key {
  virtual void f();
};
void Key::f() {}
// CHECK-DAG: @_ZTS3Key = dso_local constant [5 x i8] c"3Key\00"
// CHECK-DAG: @_ZTSP6Leaves = linkonce_odr constant [9 x i8] c"P6Leaves\00"

const std::type_info &leaves() { return typeid(Leaves); }
const std::type_info &renamed() { return typeid(Renamed); }
const std::type_info &derived() { return typeid(Derived); }
const std::type_info &members() { return typeid(Members); }
const std::type_info &typed() { return typeid(Typed); }
const std::type_info &retyped() { return typeid(Retyped); }
const std::type_info &key() { return typeid(Key); }
const std::type_info &pointer() { return typeid(Leaves *); }
