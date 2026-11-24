// DEFINE: %{cc} = %clang_cc1 -std=c++17 -emit-llvm -o - %s
// DEFINE: %{explicit} = -mdefault-visibility-export-mapping=explicit
// DEFINE: %{all} = -mdefault-visibility-export-mapping=all
// RUN: %{cc} -triple x86_64-unknown-windows-itanium %{explicit} | FileCheck %s --check-prefixes=CHECK,ITANIUM,RECORD "-DE=dllexport " -DA= "-DI=dllimport " -DL=
// RUN: %{cc} -triple aarch64-pc-windows-ntposix %{explicit} | FileCheck %s --check-prefixes=CHECK,ITANIUM,RECORD "-DE=dllexport " -DA= "-DI=dllimport " -DL=
// RUN: %{cc} -triple x86_64-w64-mingw32 %{explicit} | FileCheck %s --check-prefixes=CHECK,TLS "-DE=dllexport " -DA= "-DI=dllimport " "-DL=dso_local "
// RUN: %{cc} -triple x86_64-unknown-windows-itanium %{all} | FileCheck %s --check-prefixes=CHECK,ITANIUM,RECORD "-DE=dllexport " "-DA=dllexport " "-DI=dllimport " -DL=
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,ITANIUM,TLS -DE= -DA= -DI= -DL=

/// On COFF the mapping exports a definition whose default or protected
/// visibility the source marks (under "all", implied too) unless every user
/// emits it itself, and imports a declaration whose default visibility is
/// marked until the unit defines it. A mark is an attribute or pragma on the
/// declaration, its template, or a class or namespace around it; a template
/// argument or a variable's type only restricts, as under -fvisibility=hidden.
/// A thread-local variable is reached through its record.

#define DEFAULT __attribute__((visibility("default")))
#define PROTECTED __attribute__((visibility("protected")))
#define HIDDEN __attribute__((visibility("hidden")))

int init();

DEFAULT void marked() {}
PROTECTED void bound() {}
HIDDEN void hidden() {}
void plain() {}
DEFAULT inline void inlined() {}
template <class T> DEFAULT void tmpl() {}
template void tmpl<int>();

DEFAULT int var = 1;
PROTECTED int boundVar = 1;
int plainVar = 1;
DEFAULT inline int inlineVar = init();
inline int plainInlineVar = 1;
DEFAULT thread_local int tls = 1;
DEFAULT inline int &local() {
  static int s = init();
  return s;
}

struct DEFAULT Key {
  virtual void key();
};
void Key::key() {}
struct DEFAULT Vague {
  virtual void f() {}
};
struct DEFAULT VBase : virtual Key {
  void key() override;
};
void VBase::key() {}
struct DEFAULT VagueVBase : virtual Key {};
template <class T> struct DEFAULT Tmpl {
  virtual void f() {}
  static int member;
};
template <class T> int Tmpl<T>::member = 1;
template struct Tmpl<int>;

struct DEFAULT Mark {};
struct HIDDEN Hid {};
template <class T> struct Unmarked {
  virtual void f() {}
  static int member;
};
template <class T> int Unmarked<T>::member = 1;
template struct Unmarked<Mark>;
template struct DEFAULT Unmarked<int>;
template struct Tmpl<Hid>;
template <class T> void unmarkedTmpl() {}
template void unmarkedTmpl<Mark>();
Mark markVar;
namespace __attribute__((type_visibility("default"))) lib {
struct View { const char *p; };
template <class C> struct Strings;
template <> struct Strings<char> { static constexpr View t{"t"}; };
} // namespace lib
struct Members {
  DEFAULT void marked();
  void plain();
};
void Members::marked() {}
void Members::plain() {}
struct __attribute__((type_visibility("default"))) TypeOnly {
  virtual void key();
};
void TypeOnly::key() {}

void *operator new(__SIZE_TYPE__) { return &var; }

void use() {
  inlined();
  tmpl<char>();
  local();
  Vague v;
  VagueVBase w;
  Tmpl<char> t;
  (void)(inlineVar + plainInlineVar + Tmpl<char>::member);
  (void)&lib::Strings<char>::t;
}

// CHECK-DAG: define dso_local [[E]]void @_Z6markedv()
// CHECK-DAG: define protected [[E]]void @_Z5boundv()
// CHECK-DAG: define hidden void @_Z6hiddenv()
// CHECK-DAG: define dso_local [[A]]void @_Z5plainv()
// CHECK-DAG: define linkonce_odr dso_local void @_Z7inlinedv()
// CHECK-DAG: define weak_odr dso_local [[E]]void @_Z4tmplIiEvv()
// CHECK-DAG: define linkonce_odr dso_local void @_Z4tmplIcEvv()
// CHECK-DAG: define dso_local [[E]]noundef nonnull ptr @_Znw{{[my]}}(

// CHECK-DAG: @var = dso_local [[E]]global
// CHECK-DAG: @boundVar = protected [[E]]global
// CHECK-DAG: @plainVar = dso_local [[A]]global
// CHECK-DAG: @inlineVar = linkonce_odr [[L]][[E]]global
// CHECK-DAG: @_ZGV9inlineVar = linkonce_odr [[L]][[E]]global
// CHECK-DAG: @plainInlineVar = linkonce_odr [[L]][[A]]global
// CHECK-DAG: @tls = dso_local thread_local global
// CHECK-DAG: define linkonce_odr dso_local noundef nonnull align 4 dereferenceable(4) ptr @_Z5localv()
// CHECK-DAG: @_ZZ5localvE1s = linkonce_odr [[L]][[E]]global
// CHECK-DAG: @_ZGVZ5localvE1s = linkonce_odr [[L]][[E]]global
// CHECK-DAG: @_ZN4TmplIiE6memberE = weak_odr [[L]][[E]]global
// CHECK-DAG: @_ZN4TmplIcE6memberE = linkonce_odr [[L]][[E]]global

// CHECK-DAG: @_ZTV3Key = dso_local [[E]]constant
// CHECK-DAG: @_ZTV5Vague = linkonce_odr dso_local {{(unnamed_addr )?}}constant
// CHECK-DAG: @_ZTT5VBase = dso_local [[E]]unnamed_addr constant
// CHECK-DAG: @_ZTT10VagueVBase = linkonce_odr dso_local unnamed_addr constant
// CHECK-DAG: @_ZTV4TmplIiE = weak_odr dso_local [[E]]{{(unnamed_addr )?}}constant
// CHECK-DAG: @_ZTV4TmplIcE = linkonce_odr dso_local {{(unnamed_addr )?}}constant
// ITANIUM-DAG: @_ZTI3Key = dso_local [[E]]constant
// ITANIUM-DAG: @_ZTS3Key = dso_local [[E]]constant
// ITANIUM-DAG: @_ZTI5Vague = linkonce_odr constant
// ITANIUM-DAG: @_ZTI4TmplIiE = weak_odr [[E]]constant

// CHECK-DAG: define weak_odr dso_local [[A]]void @_ZN8UnmarkedI4MarkE1fEv(
// CHECK-DAG: @_ZN8UnmarkedI4MarkE6memberE = weak_odr [[L]][[A]]global
// CHECK-DAG: @_ZTV8UnmarkedI4MarkE = weak_odr dso_local [[A]]{{(unnamed_addr )?}}constant
// ITANIUM-DAG: @_ZTI8UnmarkedI4MarkE = weak_odr [[A]]constant
// CHECK-DAG: define weak_odr dso_local [[E]]void @_ZN8UnmarkedIiE1fEv(
// CHECK-DAG: @_ZN8UnmarkedIiE6memberE = weak_odr [[L]][[E]]global
// CHECK-DAG: @_ZTV8UnmarkedIiE = weak_odr dso_local [[E]]{{(unnamed_addr )?}}constant
// CHECK-DAG: define weak_odr hidden void @_ZN4TmplI3HidE1fEv(
// CHECK-DAG: @_ZN4TmplI3HidE6memberE = weak_odr hidden global
// CHECK-DAG: define weak_odr dso_local [[A]]void @_Z12unmarkedTmplI4MarkEvv()
// CHECK-DAG: @markVar = dso_local [[A]]global
// CHECK-DAG: @_ZN3lib7StringsIcE1tE = linkonce_odr [[L]][[A]]constant
// CHECK-DAG: define dso_local [[E]]void @_ZN7Members6markedEv(
// CHECK-DAG: define dso_local [[A]]void @_ZN7Members5plainEv(
// CHECK-DAG: define dso_local [[A]]void @_ZN8TypeOnly3keyEv(
// CHECK-DAG: @_ZTV8TypeOnly = dso_local [[E]]constant
// ITANIUM-DAG: @_ZTI8TypeOnly = dso_local [[E]]constant

DEFAULT void importedFn();
DEFAULT extern int importedVar;
PROTECTED void boundDecl();
void plainDecl();
extern int plainDeclVar;
DEFAULT extern thread_local int importedTls;
DEFAULT __attribute__((weak)) void weakDecl();
DEFAULT __attribute__((dllexport)) void exportedDecl();
DEFAULT void definedLater();
DEFAULT extern int varDefinedLater;
struct DEFAULT Imported final {
  virtual void key();
  void declared();
  void inlineMember() {}
  static int member;
};
extern Mark markDecl;
extern template struct DEFAULT Unmarked<long>;
template <class T> struct UnmarkedDecl {
  void declared();
};
int *address = &importedVar;
void *markAddress = &markDecl;

void useImports() {
  importedFn();
  boundDecl();
  plainDecl();
  weakDecl();
  exportedDecl();
  definedLater();
  Imported i;
  i.declared();
  i.inlineMember();
  delete new int;
  UnmarkedDecl<Mark>().declared();
  importedTls = plainDeclVar + Imported::member + varDefinedLater +
                Unmarked<long>::member;
}
void definedLater() {}
int varDefinedLater = 1;

// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}[[I]]void @_Z10importedFnv()
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}protected void @_Z9boundDeclv()
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}{{(dso_local )?}}void @_Z9plainDeclv()
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}extern_weak void @_Z8weakDeclv()
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}dso_local void @_Z12exportedDeclv()
// CHECK-DAG: define dso_local [[E]]void @_Z12definedLaterv()
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}[[I]]void @_ZN8Imported8declaredEv(
// CHECK-DAG: define linkonce_odr dso_local void @_ZN8Imported12inlineMemberEv(
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}[[I]]void @_ZdlPv{{[my]}}(

// CHECK-DAG: @importedVar = external {{(dso_local )?}}[[I]]global
// CHECK-DAG: @address = dso_local [[A]]global ptr @importedVar
// CHECK-DAG: @plainDeclVar = external {{(dso_local )?}}global
// TLS-DAG: @importedTls = external {{(dso_local )?}}thread_local global
// RECORD-DAG: @"importedTls$tls" = external dllimport constant { ptr, i32, i32 }
// CHECK-DAG: @varDefinedLater = dso_local [[E]]global
// CHECK-DAG: @_ZN8Imported6memberE = external {{(dso_local )?}}[[I]]global
// CHECK-DAG: @_ZTV8Imported = external {{(dso_local )?}}constant
// CHECK-DAG: @markDecl = external {{(dso_local )?}}global
// CHECK-DAG: declare {{(!kcfi_type ![0-9]+ )?}}{{(dso_local )?}}void @_ZN12UnmarkedDeclI4MarkE8declaredEv(
// CHECK-DAG: @_ZN8UnmarkedIlE6memberE = external {{(dso_local )?}}[[I]]global
