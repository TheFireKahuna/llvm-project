// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fsyntax-only -verify %s

// On Windows Itanium a member function that spells __stdcall, __thiscall or
// ms_abi and returns a class takes Microsoft's calling convention, which an
// override that spells none inherits and an out-of-line definition may spell
// again. That convention has the callee destroy a class argument, so one that
// is not trivial for calls cannot be passed by value.

struct Size { float w, h; };
struct NonTrivial { NonTrivial(const NonTrivial &); int x; };

struct Interface {
  virtual Size __stdcall size() = 0; // expected-note {{overridden virtual function is here}}
  virtual long __stdcall release() = 0;
  virtual Size __stdcall bad(NonTrivial); // expected-error {{'NonTrivial', which is not trivial for calls, cannot be passed by value to a member function with Microsoft's calling convention}}
  virtual void __stdcall fine(NonTrivial);
  Size __thiscall outOfLine();
};

struct Impl : Interface {
  Size size() override;
  long release() override;
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

static_assert(!__is_same(Size (__stdcall Interface::*)(), Size (Interface::*)()), "");
static_assert(__is_same(long (__stdcall Interface::*)(), long (Interface::*)()), "");
