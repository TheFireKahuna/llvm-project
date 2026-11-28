//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// This test makes sure that we use the implementation for comparing type_info
// objects that matches what the compiler emits on Windows Itanium and NT-POSIX:
// a type_info object that images which no link joins may each keep has the high
// bit of its name pointer set, and every name pointer holds the name's length.

// REQUIRES: target={{.+}}-windows-{{itanium|ntposix}}

#include <typeinfo>

#if _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION != 3 || !defined(_LIBCPP_TYPEINFO_NAME_HAS_LENGTH)
#  error "Windows Itanium and NT-POSIX should use NonUniqueARMRTTIBit (3) and read the length of a type name"
#endif
