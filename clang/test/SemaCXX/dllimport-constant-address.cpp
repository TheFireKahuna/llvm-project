// DEFINE: %{cc} = %clang_cc1 -fsyntax-only -fms-extensions -std=c++20
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -verify=constant %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix -verify=constant %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -verify=constant %s \
// RUN:   -fexperimental-new-constant-interpreter
// RUN: %{cc} -triple x86_64-pc-windows-msvc -verify=runtime %s
// RUN: %{cc} -triple x86_64-w64-windows-gnu -verify=runtime %s
// RUN: %{cc} -triple x86_64-pc-windows-msvc -verify=runtime %s \
// RUN:   -fexperimental-new-constant-interpreter
// RUN: %clang_cc1 -fsyntax-only -fms-extensions -x c -verify=constant %s \
// RUN:   -triple x86_64-unknown-windows-itanium
// RUN: %clang_cc1 -fsyntax-only -fms-extensions -x c -verify=constant %s \
// RUN:   -triple x86_64-unknown-windows-itanium \
// RUN:   -fexperimental-new-constant-interpreter
// RUN: %clang_cc1 -fsyntax-only -fms-extensions -x c -verify=runtime-c %s \
// RUN:   -triple x86_64-pc-windows-msvc

// On Windows Itanium and NT-POSIX the address of a dllimport function or
// variable is a constant expression, under either constant evaluator: the
// linker has the loader write it into the static data that holds it. On the
// other Windows targets it is known only at run time.

// constant-no-diagnostics

struct Agg {
  int Field;
  int Array[4];
};
__declspec(dllimport) extern int Var;
__declspec(dllimport) extern struct Agg Object;
__declspec(dllimport) void function(void);

#ifdef __cplusplus
struct Class {
  __declspec(dllimport) void method();
};

constinit int *VarAddress = &Var;
// runtime-error@-1 {{variable does not have a constant initializer}}
// runtime-note@-2 {{required by 'constinit' specifier here}}
constinit int &VarReference = Var;
// runtime-error@-1 {{variable does not have a constant initializer}}
// runtime-note@-2 {{required by 'constinit' specifier here}}
constinit int *Interior = &Object.Array[2];
// runtime-error@-1 {{variable does not have a constant initializer}}
// runtime-note@-2 {{required by 'constinit' specifier here}}
constinit void (*FunctionAddress)() = &function;
// runtime-error@-1 {{variable does not have a constant initializer}}
// runtime-note@-2 {{required by 'constinit' specifier here}}
constinit void (Class::*MemberPointer)() = &Class::method;
// runtime-error@-1 {{variable does not have a constant initializer}}
// runtime-note@-2 {{required by 'constinit' specifier here}}
constexpr void (*Table[])() = {function, nullptr};
// runtime-error@-1 {{constexpr variable 'Table' must be initialized by a constant expression}}
#else
int *VarAddress = &Var;
// runtime-c-error@-1 {{initializer element is not a compile-time constant}}
int *Interior = &Object.Array[2];
// runtime-c-error@-1 {{initializer element is not a compile-time constant}}
void (*FunctionAddress)(void) = &function;
#endif
