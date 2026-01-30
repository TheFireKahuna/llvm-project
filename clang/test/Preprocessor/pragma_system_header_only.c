// RUN: %clang_cc1 -E -verify %s -isystem %S/Inputs 2>&1 | FileCheck %s

// Test that #pragma clang system_header_only makes a macro invisible in user
// code but visible in system headers.

// Define a test macro and mark it as system_header_only
#define _TEST_MACRO 123
#pragma clang system_header_only(_TEST_MACRO)

// In user code (this file), the macro should be invisible
#ifdef _TEST_MACRO
USER_CODE_SAW_MACRO
#else
USER_CODE_DID_NOT_SEE_MACRO
#endif

// CHECK: USER_CODE_DID_NOT_SEE_MACRO

// Now include a system header that checks for the macro
#include <system_header_only_macro.h>

// The system header should have seen the macro
#if IN_SYSTEM_HEADER_VALUE == 1
SYSTEM_HEADER_SAW_MACRO
#else
SYSTEM_HEADER_DID_NOT_SEE_MACRO
#endif

// CHECK: SYSTEM_HEADER_SAW_MACRO

// expected-no-diagnostics
