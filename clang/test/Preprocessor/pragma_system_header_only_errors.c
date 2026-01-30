// RUN: %clang_cc1 -E -verify %s

// Test error cases for #pragma clang system_header_only

// Error: macro doesn't exist
#pragma clang system_header_only(NONEXISTENT_MACRO) // expected-error {{no macro named 'NONEXISTENT_MACRO'}}

// Error: missing parentheses
#define FOO 1
#pragma clang system_header_only FOO // expected-error {{expected '('}}

// Error: missing identifier
#define BAR 2
#pragma clang system_header_only() // expected-error {{expected identifier}}

// Error: missing closing paren
#define BAZ 3
#pragma clang system_header_only(BAZ // expected-error {{expected ')'}}
