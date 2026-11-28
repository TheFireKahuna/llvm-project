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
// type_info object is not unique, the length of its layout shape and the
// shape. Every copy of a name object selects by exact match, so that a link
// whose inputs hold two layouts of one class fails; a type_info object selects
// any copy.

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

// CHECK-DAG: $_ZTI5NoKey = comdat any
// CHECK-DAG: $_ZTS5NoKey = comdat exactmatch
// CHECK-DAG: $_ZTSP5NoKey = comdat exactmatch
// CHECK-DAG: $_ZTS6Hidden = comdat exactmatch

// 1 << 63 | 9 << 48.
// CHECK-DAG: @_ZTI5NoKey = linkonce_odr constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS5NoKey to i64), i64 -9220838762064379904) to ptr) }, comdat
// CHECK-DAG: @_ZTS5NoKey = linkonce_odr constant [9 x i8] c"5NoKey\00\01\83", comdat
// CHECK-DAG: @_ZTI4TmplIiE = weak_odr constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS4TmplIiE to i64), i64 -9220275812110958592) to ptr) }, comdat

// A type that is not a class has a name and no more.
// CHECK-DAG: @_ZTIP5NoKey = linkonce_odr constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSP5NoKey to i64), i64 -9221120237041090560) to ptr), i32 0, ptr @_ZTI5NoKey }, comdat
// CHECK-DAG: @_ZTSP5NoKey = linkonce_odr constant [8 x i8] c"P5NoKey\00", comdat

// CHECK-DAG: @_ZTI6Hidden = linkonce_odr hidden constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS6Hidden to i64), i64 2251799813685248) to ptr) }, comdat
// CHECK-DAG: @_ZTI9Protected = linkonce_odr protected constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS9Protected to i64), i64 -9219712862157537280) to ptr) }, comdat
// CHECK-DAG: @_ZTS9Protected = linkonce_odr protected constant [13 x i8] c"9Protected\00\01\83", comdat
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

// The link chooses the image that owns a type_info object or name of default
// visibility that the translation unit does not define strongly, so none is
// dso_local, as a strong, hidden or internal one is.
struct External {
  virtual void f();
};
// CHECK-DAG: @_ZTI8External = external constant ptr
// CHECK-DAG: @_ZTIi = external constant ptr
const std::type_info &external() { return typeid(External); }
const std::type_info &fundamental() { return typeid(int); }

// On Windows Itanium and NT-POSIX the name of a non-unique type_info object
// for a class is followed by the length of the class's layout shape, a ULEB128
// count of its bytes, and the shape, so that no name object that holds a shape
// starts another. The shape is 4-bit codes, two to a byte, the low half first:
// the log2 of the class's alignment, then every leaf, by-value members and
// bases flattened in, in the order of their offsets, each its kind and the
// kind's operands. A count is 3-bit digits, the lowest first, each with 8 added
// where another follows. Leaves are contiguous unless a gap comes between them,
// and a gap fills the shape to its size. A run of equal contiguous leaves, as
// an array's elements are, is one array item: its count, the element's items
// and an end. The kinds: 0 to 4 integers of 1 to 16 bytes, 5 float, 6 double,
// 7 pointer, 8 vtable pointer, 9 bit-field and its width, 10 array, 11 union
// and its count of alternatives, each one's items and an end, 12 a gap and its
// bytes, 13 end, 14 a rarer kind in the next code, 15 member function pointer.
// Where a leaf's kind leaves its type open, as a pointer's, a member pointer's
// or an enumeration's does, an end and, from the next byte, the field-type
// signature follow: those types, mangled together as the parameters of a
// function type after the class, whose substitutions write a repeated name
// once and may refer to the parts of the class's name, which is left out. An
// end also fills an odd last byte.

// Alignment 8; the vtable pointer, an int, a gap of 4 bytes and a double, the
// array of three chars, a gap of a byte, four ints as one run and a gap of 4
// bytes to the size, 48.
struct Leaves {
  virtual ~Leaves() {}
  int a;
  double d;
  char c[3];
  int x, y, z, w;
};
// CHECK-DAG: @_ZTS6Leaves = linkonce_odr constant [18 x i8] c"6Leaves\00\09\83\C2d:\D0\1CJ\D2L"

// The same layout under other names is the same shape.
struct Renamed {
  virtual ~Renamed() {}
  int b;
  double e;
  char f[3];
  int p[4];
};
// CHECK-DAG: @_ZTS7Renamed = linkonce_odr constant [19 x i8] c"7Renamed\00\09\83\C2d:\D0\1CJ\D2L"

// A base's leaves at its offset, a pointer, bit-fields of 3 and 4 bits, a gap
// of 25 bits, a bit-field of 30 bits and a gap of 2 bits. A gap that is not
// whole bytes counts bits, as the rarer kind 8.
struct Base {
  virtual ~Base() {}
};
struct Derived : Base {
  void *p;
  unsigned b1 : 3, b2 : 4, b3 : 30;
};
// CHECK-DAG: @_ZTS7Derived = linkonce_odr constant [21 x i8] c"7Derived\00\0B\83\97\93\E4\98\93>\8E\D2Pv"

// A union of an int and a float, a gap of 4 bytes, a member function pointer,
// an array of ten shorts and a gap of 4 bytes.
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
// CHECK-DAG: @_ZTS7Members = linkonce_odr constant [33 x i8] c"7Members\00\17\83+\D2\D5L\AF\1A\D1L\DDM7DerivedFivE"

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
// CHECK-DAG: @_ZTS5Typed = linkonce_odr constant [31 x i8] c"5Typed\00\17\83*\D7\C2t\DDP3FooS1_5ColorPKc"
// CHECK-DAG: @_ZTS7Retyped = linkonce_odr constant [29 x i8] c"7Retyped\00\13\83*\D7\C2t\DDP3FooP3BarPKc"

// A virtual base's leaves at its offset in the complete object. The kinds
// behind 14: 4 a member data pointer, 5 a vector, its count, its element's
// items and an end.
struct VBase {
  virtual ~VBase() {}
  int v;
};
struct Extended : virtual VBase {
  int VBase::*m;
  __attribute__((vector_size(16))) float f;
};
// CHECK-DAG: @_ZTS8Extended = linkonce_odr constant [26 x i8] c"8Extended\00\0F\84N^T\8D\C2\D4M5VBasei"

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
// CHECK-DAG: $"__llvm_incomplete$_ZTS3Foo" = comdat exactmatch
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
