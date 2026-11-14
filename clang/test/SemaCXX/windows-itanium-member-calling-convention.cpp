// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fsyntax-only -x c -verify=c %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fsyntax-only -x c -verify=c %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -DSYSV -fsyntax-only -verify=sysv %s

// On Windows Itanium __stdcall, __thiscall and ms_abi name Microsoft's calling
// convention, which a function type keeps where it returns a class. An
// override that spells no convention inherits it, and a redeclaration may
// spell it another way. Under it the callee destroys a class argument, so one
// that is not trivial for calls cannot be passed by value to a member
// function.

#if defined(__cplusplus) && !defined(SYSV)
struct Size { float w, h; };
struct NonTrivial { NonTrivial(const NonTrivial &); int x; };
template <class T> struct Late { Late(const Late &); T x; };

struct Interface {
  virtual Size __stdcall size() = 0; // expected-note {{overridden virtual function is here}}
  virtual long __stdcall release() = 0;
  virtual Size __stdcall bad(NonTrivial); // expected-error {{'NonTrivial', which is not trivial for calls, cannot be passed by value to a member function with Microsoft's calling convention}}
  virtual void __stdcall fine(NonTrivial);
  Size __thiscall outOfLine();
  Size __stdcall late(Late<int>);
};

struct Impl : Interface {
  Size size() override;
  long release() override;
  Size bad(NonTrivial) override; // expected-error {{cannot be passed by value}}
};

struct Deeper : Impl {
  Size size() override;
};

struct WrongConvention : Interface {
  Size __cdecl size() override; // expected-error {{virtual function 'size' has different calling convention attributes}}
};

Size __stdcall Interface::outOfLine() { return {}; }
Size Impl::size() { return {}; }
Size __stdcall Deeper::size() { return {}; }
// The parameter's class is complete only in the definition.
Size Interface::late(Late<int> l) { return {}; } // expected-error {{'Late<int>', which is not trivial for calls, cannot be passed by value}}

static_assert(!__is_same(Size (__stdcall Interface::*)(), Size (Interface::*)()), "");
static_assert(__is_same(Size (__stdcall Interface::*)(), Size (__attribute__((ms_abi)) Interface::*)()), "");
static_assert(__is_same(long (__stdcall Interface::*)(), long (Interface::*)()), "");
static_assert(__is_same(decltype(&Deeper::size), Size (__thiscall Deeper::*)()), "");

// A free function's type keeps the convention too, and only where it returns
// a class.
Size __stdcall freeSize();
long __stdcall freeLong();
static_assert(!__is_same(decltype(freeSize), Size()), "");
static_assert(__is_same(decltype(freeLong), long()), "");
#endif

// A function that is not a member has one ABI whether or not its type spells
// the convention. C, which has no member functions, never keeps it. In C++ a
// pointer or reference to such a function converts between the two types,
// which stay distinct for overloading and templates, and a pointer to member
// does not. On NT-POSIX for x86-64, where the default convention is System
// V's, the two are different ABIs and nothing converts.
// c-no-diagnostics

#ifdef SYSV
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI __stdcall
#endif
typedef void *HANDLE;
typedef struct { short X, Y; } COORD;
COORD WINAPI GetLargestConsoleWindowSize(HANDLE);
COORD plain(HANDLE);
COORD (*p)(HANDLE) = GetLargestConsoleWindowSize; // sysv-error {{cannot initialize a variable of type 'COORD (*)(HANDLE)'}}
COORD (WINAPI *q)(HANDLE) = plain; // sysv-error {{cannot initialize a variable of type 'WINAPI COORD (*)(HANDLE)'}}

#ifndef SYSV
void takeP(COORD (*)(HANDLE));
void takeQ(COORD (WINAPI *)(HANDLE));
void use(void) {
  p = q;
  q = p;
  takeP(GetLargestConsoleWindowSize);
  takeQ(plain);
}

#ifndef __cplusplus
_Static_assert(__builtin_types_compatible_p(__typeof__(GetLargestConsoleWindowSize), __typeof__(plain)), "");
#else
COORD (WINAPI &r1)(HANDLE) = plain;
COORD (&r2)(HANDLE) = GetLargestConsoleWindowSize;
bool same = p == q;
COORD (*either)(HANDLE) = same ? p : q;

COORD WINAPI overloaded(HANDLE);
int overloaded(int);
COORD (*po)(HANDLE) = &overloaded;

int select(COORD (*)(HANDLE));
char select(COORD (WINAPI *)(HANDLE));
static_assert(sizeof(select(plain)) == sizeof(int), "");
static_assert(sizeof(select(GetLargestConsoleWindowSize)) == sizeof(char), "");
template <class T, class U> constexpr bool Same = false;
template <class T> constexpr bool Same<T, T> = true;
static_assert(!Same<decltype(p), decltype(q)>, "");

struct Methods {
  COORD WINAPI spelled();
  COORD unspelled();
  COORD WINAPI both();
  COORD WINAPI both(int);
};
COORD (Methods::*pm)() = &Methods::spelled; // expected-error {{cannot initialize a variable of type 'COORD (Methods::*)()'}}
COORD (WINAPI Methods::*qm)() = &Methods::unspelled; // expected-error {{cannot initialize a variable of type 'COORD (Methods::*)() __attribute__((stdcall))'}}
COORD (Methods::*po2)() = &Methods::both; // expected-error {{cannot initialize a variable of type 'COORD (Methods::*)()'}}
#endif
#endif
