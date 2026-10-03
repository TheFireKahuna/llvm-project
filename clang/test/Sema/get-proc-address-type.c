// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify -Wget-proc-address-type %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify -Wget-proc-address-type -x c++ %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fsyntax-only -verify -Wget-proc-address-type %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fsyntax-only -verify=off %s

// off-no-diagnostics

# 1 "sdk.h" 1 3
typedef long long (*FARPROC)();
typedef void *HMODULE;
#ifdef __cplusplus
extern "C" {
#endif
FARPROC GetProcAddress(HMODULE, const char *);
int SdkFunc(void *, unsigned long); // expected-note 1+ {{'SdkFunc' declared here}}
int SdkPrintf(const char *, ...); // expected-note {{'SdkPrintf' declared here}}
#ifdef __cplusplus
}
#endif
# 20 "get-proc-address-type.c" 2

int UserFunc(void *, unsigned long);

typedef int (*SdkFuncTy)(void *, unsigned long);
typedef int (*WrongTy)(void *, unsigned int);

void f(HMODULE m, const char *name) {
  SdkFuncTy a = (SdkFuncTy)GetProcAddress(m, "SdkFunc");
  WrongTy b = (WrongTy)GetProcAddress(m, "SdkFunc"); // expected-warning {{'GetProcAddress' result for 'SdkFunc' is cast to 'WrongTy' (aka 'int (*)(void *, unsigned int)'), which does not match its declared type 'int (void *, unsigned long)'}}
  int (*c)(void *) = (int (*)(void *))GetProcAddress(m, "SdkFunc"); // expected-warning {{does not match its declared type}}
  long (*d)(void *, unsigned long) = (long (*)(void *, unsigned long))GetProcAddress(m, "SdkFunc"); // expected-warning {{does not match its declared type}}
  WrongTy e = (WrongTy)(void *)GetProcAddress(m, "SdkFunc"); // expected-warning {{does not match its declared type}}
  int (*g)(const char *) = (int (*)(const char *))GetProcAddress(m, "SdkPrintf"); // expected-warning {{does not match its declared type}}
  int (*h)(const char *, ...) = (int (*)(const char *, ...))GetProcAddress(m, "SdkPrintf");

  // No system header declares these, or the name is not a literal.
  WrongTy i = (WrongTy)GetProcAddress(m, "UserFunc");
  WrongTy j = (WrongTy)GetProcAddress(m, "NotDeclared");
  WrongTy k = (WrongTy)GetProcAddress(m, name);
}

#ifdef __cplusplus
void cxx(HMODULE m) {
  auto a = reinterpret_cast<WrongTy>(GetProcAddress(m, "SdkFunc")); // expected-warning {{does not match its declared type}}
  auto b = reinterpret_cast<int (*)(void *, unsigned long) noexcept>(GetProcAddress(m, "SdkFunc"));
  auto c = reinterpret_cast<decltype(&SdkFunc)>(GetProcAddress(m, "SdkFunc"));
}
#endif
