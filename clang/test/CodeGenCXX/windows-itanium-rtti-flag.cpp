// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=FLAG %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=FLAG %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=FLAG %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm \
// RUN:   -fclang-abi-compat=23 -o - %s | FileCheck --check-prefix=COMPAT %s

// On Windows Itanium and NT-POSIX a type_info object with vague linkage and
// default visibility sets the high bit of its name pointer, as on arm64 on
// Apple platforms, so that images that no link joins compare their own by
// name. Unlike Apple's, a linkonce_odr one keeps default visibility, so that
// the linker can bind the images an import path joins to one owner's. A
// strong, hidden or internal type_info object is unique. Every name pointer
// holds the name's length with its NUL and what follows it in bits 48 to 62,
// so that comparing two names needs no scan. A non-unique class's name is
// followed by its layout shape, here its size, alignment and vtable pointer.
// Under -fclang-abi-compat=23 none of this is done.

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

namespace {
struct Internal {
  virtual void f() {}
};
} // namespace

// FLAG-DAG: @_ZTI3Key = dso_local constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS3Key to i64), i64 1407374883553280) to ptr) }
// COMPAT-DAG: @_ZTI3Key = dso_local constant { ptr, ptr } { ptr {{.*}}, ptr @_ZTS3Key }

// FLAG-DAG: @_ZTI5NoKey = linkonce_odr constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS5NoKey to i64), i64 -9220557287087669248) to ptr) }
// FLAG-DAG: @_ZTS5NoKey = linkonce_odr constant [10 x i8] c"5NoKey\00\08\03\09", comdat
// FLAG-DAG: @_ZTSP5NoKey = linkonce_odr constant [8 x i8] c"P5NoKey\00", comdat
// COMPAT-DAG: @_ZTS5NoKey = linkonce_odr constant [7 x i8] c"5NoKey\00", comdat
// FLAG-DAG: @_ZTI4TmplIiE = weak_odr constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS4TmplIiE to i64), i64 -9219994337134247936) to ptr) }
// FLAG-DAG: @_ZTIP5NoKey = linkonce_odr constant { ptr, ptr, i32, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSP5NoKey to i64), i64 -9221120237041090560) to ptr), i32 0, ptr @_ZTI5NoKey }

// COMPAT-DAG: @_ZTI5NoKey = linkonce_odr constant { ptr, ptr } { ptr {{.*}}, ptr @_ZTS5NoKey }
// COMPAT-DAG: @_ZTI4TmplIiE = weak_odr constant { ptr, ptr } { ptr {{.*}}, ptr @_ZTS4TmplIiE }

// FLAG-DAG: @_ZTI6Hidden = linkonce_odr hidden constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS6Hidden to i64), i64 2251799813685248) to ptr) }
// FLAG-DAG: @_ZTIN12_GLOBAL__N_18InternalE = internal constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTSN12_GLOBAL__N_18InternalE to i64), i64 7318349394477056) to ptr) }
// COMPAT-DAG: @_ZTI6Hidden = linkonce_odr hidden constant { ptr, ptr } { ptr {{.*}}, ptr @_ZTS6Hidden }
// COMPAT-DAG: @_ZTIN12_GLOBAL__N_18InternalE = internal constant { ptr, ptr } { ptr {{.*}}, ptr @_ZTSN12_GLOBAL__N_18InternalE }

const std::type_info &key() { return typeid(Key); }
const std::type_info &noKey() { return typeid(NoKey); }
const std::type_info &pointer() { return typeid(NoKey *); }
const std::type_info &tmpl() { return typeid(Tmpl<int>); }
const std::type_info &hidden() { return typeid(Hidden); }
const std::type_info &internal() { return typeid(Internal); }
