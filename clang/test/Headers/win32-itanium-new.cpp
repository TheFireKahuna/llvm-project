// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/cxx \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/cxx \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify -std=c++03 %s
// expected-no-diagnostics

// The UCRT's new.h takes std::set_new_handler from the C++ library, the
// debug allocation forms forward to the ordinary ones, and eh.h declares the
// structured exception translator. The headers new.h includes still see
// _MSC_EXTENSIONS when new.h is the first to include them.

#include <new.h>
#include <eh.h>

#if !__UCRT_CORECRT_SAW_MSC_EXTENSIONS
#error "corecrt.h must see _MSC_EXTENSIONS"
#endif

std::new_handler handler = std::set_new_handler(0);

int *allocate() { return new (1, __FILE__, __LINE__) int[4]; }

void deallocate(int *p) { operator delete[](p, 1, __FILE__, __LINE__); }

_se_translator_function translator = _set_se_translator(0);
terminate_handler previous_terminate = get_terminate();
