// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | sed -E 's/(abcd)+/LONG/g' | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | sed -E 's/(abcd)+/LONG/g' | FileCheck %s

// On Windows Itanium and NT-POSIX bits 48 to 62 of a type_info object's name
// word hold the length of the name object. A length they cannot hold sets all
// of them, and is the 64-bit word ahead of the name, in one object with it;
// the name's symbol is that object's alias at the name and the key of its
// COMDAT, so that it still labels the name.

namespace std {
class type_info;
} // namespace std

#define PASTE(a, b) a##b
#define TWICE_(x) PASTE(x, x)
#define TWICE(x) TWICE_(x)
#define T4(x) TWICE(TWICE(x))
#define T16(x) T4(T4(x))
#define T8192(x) T16(T16(T16(TWICE(x))))
// 4 * 8192 = 32768 characters, which the mangling prefixes with 5 digits.
#define LONG T8192(abcd)

struct LONG {
  virtual void f() {}
};

// The length, 5 + 32768 + 1 = 32774 and a layout shape of 3 bytes, ahead of
// the name. The name word's bits are 0xFFFF << 48. The RUN lines shorten the
// name for FileCheck.
// CHECK: $_ZTS32768LONG = comdat any
// CHECK: @_ZTI32768LONG = linkonce_odr dso_local constant { ptr, ptr } { ptr {{.*}}, ptr inttoptr (i64 add (i64 ptrtoint (ptr @_ZTS32768LONG to i64), i64 -281474976710656) to ptr) }
// CHECK: @_ZTS32768LONG.length = private constant <{ i64, [32777 x i8] }> <{ i64 32777, [32777 x i8] c"32768LONG\00\08\03\09" }>, comdat($_ZTS32768LONG), align 8
// CHECK: @_ZTS32768LONG = dso_local alias i8, getelementptr inbounds (i8, ptr @_ZTS32768LONG.length, i64 8)

const std::type_info &get() { return typeid(LONG); }
