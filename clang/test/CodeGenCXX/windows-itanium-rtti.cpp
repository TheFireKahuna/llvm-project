// DEFINE: %{cc} = %clang_cc1 -fdeclspec -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s
// RUN: %{cc} -triple aarch64-unknown-windows-itanium | FileCheck %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix | FileCheck %s

// On Windows Itanium and NT-POSIX a type_info object with vague linkage and
// default or protected visibility sets the high bit of its name word, as on
// arm64 on Apple platforms, so that images that no link joins compare their
// copies by name. Unlike Apple's, a linkonce_odr one keeps its visibility, so
// that a link can bind the images it joins to one copy. A strong, hidden or
// internal type_info object is unique. Every name word holds the length of the
// name object in bits 48 to 62: the name, its terminator and, for a class whose
// type_info object is not unique, its layout shape.

namespace std {
class type_info;
} // namespace std

struct Key {
  virtual void f();
};
void Key::f() {}

struct NoKey {
  virtual void f() {}
};

template <class T> struct Tmpl {
  virtual void f() {}
};
template struct Tmpl<int>;

struct __attribute__((visibility("hidden"))) Hidden {
  virtual void f() {}
};

struct __attribute__((visibility("protected"))) Protected {
  virtual void f() {}
};

struct __attribute__((visibility("protected"))) ProtectedKey {
  virtual void f();
};
void ProtectedKey::f() {}

namespace {
struct Internal {
  virtual void f() {}
};
} // namespace

// 5 << 48.
// CHECK-DAG: @_ZTI3Key = dso_local constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS3Key to i64), i64 1407374883553280) to ptr) }
// CHECK-DAG: @_ZTS3Key = dso_local constant [5 x i8] c"3Key\00"

// 1 << 63 | 10 << 48.
// CHECK-DAG: @_ZTI5NoKey = linkonce_odr dso_local constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS5NoKey to i64), i64 -9220557287087669248) to ptr) }, comdat
// CHECK-DAG: @_ZTS5NoKey = linkonce_odr dso_local constant [10 x i8] c"5NoKey\00\08\03\09", comdat
// CHECK-DAG: @_ZTI4TmplIiE = weak_odr dso_local constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS4TmplIiE to i64), i64 -9219994337134247936) to ptr) }, comdat

// A type that is not a class has a name and no more.
// CHECK-DAG: @_ZTIP5NoKey = linkonce_odr dso_local constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSP5NoKey to i64), i64 -9221120237041090560) to ptr), i32 0, ptr @_ZTI5NoKey }, comdat
// CHECK-DAG: @_ZTSP5NoKey = linkonce_odr dso_local constant [8 x i8] c"P5NoKey\00", comdat

// CHECK-DAG: @_ZTI6Hidden = linkonce_odr hidden constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS6Hidden to i64), i64 2251799813685248) to ptr) }, comdat
// CHECK-DAG: @_ZTI9Protected = linkonce_odr protected constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS9Protected to i64), i64 -9219431387180826624) to ptr) }, comdat
// CHECK-DAG: @_ZTS9Protected = linkonce_odr protected constant [14 x i8] c"9Protected\00\08\03\09", comdat
// A protected one with a key function is defined once, and stays unique.
// CHECK-DAG: @_ZTI12ProtectedKey = protected constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS12ProtectedKey to i64), i64 4222124650659840) to ptr) }
// CHECK-DAG: @_ZTIN12_GLOBAL__N_18InternalE = internal constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSN12_GLOBAL__N_18InternalE to i64), i64 7318349394477056) to ptr) }

const std::type_info &key() { return typeid(Key); }
const std::type_info &noKey() { return typeid(NoKey); }
const std::type_info &pointer() { return typeid(NoKey *); }
const std::type_info &tmpl() { return typeid(Tmpl<int>); }
const std::type_info &hidden() { return typeid(Hidden); }
const std::type_info &protectedType() { return typeid(Protected); }
const std::type_info &protectedKey() { return typeid(ProtectedKey); }
const std::type_info &internal() { return typeid(Internal); }

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

// Size 48, alignment 8; the vtable pointer, an int, 32 bits of padding and a
// double, the array of three chars, 8 bits of padding and four ints as one run.
struct Leaves {
  virtual ~Leaves() {}
  int a;
  double d;
  char c[3];
  int x, y, z, w;
};
// CHECK-DAG: @_ZTS6Leaves = linkonce_odr dso_local constant [22 x i8] c"6Leaves\000\03\09\02\F6 \0C\03\00\0E\8C\04\02\0E"

// The same layout under other names is the same shape.
struct Renamed {
  virtual ~Renamed() {}
  int b;
  double e;
  char f[3];
  int p[4];
};
// CHECK-DAG: @_ZTS7Renamed = linkonce_odr dso_local constant [23 x i8] c"7Renamed\000\03\09\02\F6 \0C\03\00\0E\8C\04\02\0E"

// A base's leaves at its offset, a pointer, then two bit-fields of 3 and 5
// bits.
struct Base {
  virtual ~Base() {}
};
struct Derived : Base {
  void *p;
  unsigned b1 : 3, b2 : 5;
};
// CHECK-DAG: @_ZTS7Derived = linkonce_odr dso_local constant [20 x i8] c"7Derived\00\18\03\09\08\0B\03\0B\05\0EPv"

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
// CHECK-DAG: @_ZTS7Members = linkonce_odr dso_local constant [38 x i8] c"7Members\008\03\09\0D\02\02\0E\05\0E\FA \0C\0A\01\0E\0EM7DerivedFivE"

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
// CHECK-DAG: @_ZTS5Typed = linkonce_odr dso_local constant [35 x i8] c"5Typed\00(\03\09\0C\02\08\0E\02\F8 \0EP3FooS1_5ColorPKc"
// CHECK-DAG: @_ZTS7Retyped = linkonce_odr dso_local constant [33 x i8] c"7Retyped\00(\03\09\0C\02\08\0E\02\F8 \0EP3FooP3BarPKc"

// A virtual base's leaves at its offset in the complete object. The kinds
// behind 15: 3 a member data pointer, 4 a vector, its count, its element's
// items and an end.
struct VBase {
  virtual ~VBase() {}
  int v;
};
struct Extended : virtual VBase {
  int VBase::*m;
  __attribute__((vector_size(16))) float f;
};
// CHECK-DAG: @_ZTS8Extended = linkonce_odr dso_local constant [31 x i8] c"8Extended\000\04\09\0F\03\0F\04\04\05\0E\09\02\0EM5VBasei"

const std::type_info *all[] = {&typeid(Leaves),  &typeid(Renamed),
                                &typeid(Derived), &typeid(Members),
                                &typeid(Typed),   &typeid(Retyped),
                                &typeid(Extended)};

// On Windows Itanium and NT-POSIX a type_info object that reaches a class
// incomplete in the translation unit is a weak alias of its own name, whose
// default is a placeholder under a name of its own with the ABI's incomplete
// flags, kept once in an image, so that the link binds it to the complete
// class's type_info object wherever an input defines that. A pointer's name
// object holds no layout, so the placeholder shares it; the placeholder for
// the class has a name object of its own, since the complete class's may be
// followed by its layout.

struct Foo;
struct __declspec(dllexport) Exported;

// CHECK-DAG: @"__llvm_incomplete$_ZTIPP3Foo" = linkonce_odr dso_local constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSPP3Foo to i64), i64 {{.*}}) to ptr), i32 8, ptr @_ZTIP3Foo }, comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTIP3Foo" = linkonce_odr dso_local constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSP3Foo to i64), i64 {{.*}}) to ptr), i32 8, ptr @_ZTI3Foo }, comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTI3Foo" = linkonce_odr dso_local constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @"__llvm_incomplete$_ZTS3Foo" to i64), i64 -9221964661971222528) to ptr) }, comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTS3Foo" = linkonce_odr dso_local constant [5 x i8] c"3Foo\00", comdat
// CHECK-DAG: @_ZTSPP3Foo = linkonce_odr dso_local constant [7 x i8] c"PP3Foo\00", comdat
// CHECK-DAG: @"__llvm_incomplete$_ZTIM3Fooi" = linkonce_odr dso_local constant { ptr, ptr, i32, ptr, ptr } { ptr {{.*}}, ptr {{.*}}, i32 16, ptr @_ZTIi, ptr @_ZTI3Foo }, comdat
// CHECK-DAG: @_ZTI3Foo = weak alias { ptr, ptr }, ptr @"__llvm_incomplete$_ZTI3Foo"
// CHECK-DAG: @_ZTIP3Foo = weak alias { ptr, ptr, i32, ptr }, ptr @"__llvm_incomplete$_ZTIP3Foo"
// CHECK-DAG: @_ZTIPP3Foo = weak alias { ptr, ptr, i32, ptr }, ptr @"__llvm_incomplete$_ZTIPP3Foo"
// CHECK-DAG: @_ZTIM3Fooi = weak alias { ptr, ptr, i32, ptr, ptr }, ptr @"__llvm_incomplete$_ZTIM3Fooi"

// A placeholder is not exported.
// CHECK-DAG: @"__llvm_incomplete$_ZTI8Exported" = linkonce_odr dso_local constant

// CHECK: ret ptr @_ZTIPP3Foo
const std::type_info &pp() { return typeid(Foo **); }
const std::type_info &mp() { return typeid(int Foo::*); }
const std::type_info &exported() { return typeid(Exported *); }
