// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple i686-pc-windows-msvc -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple arm64ec-pc-windows-msvc -fsyntax-only -verify %s

int unsupported(void **buffer) {
  return __builtin_experimental_nt_recovery(buffer); // expected-error {{NT recovery requires a Windows x86-64 or AArch64 target}}
}
