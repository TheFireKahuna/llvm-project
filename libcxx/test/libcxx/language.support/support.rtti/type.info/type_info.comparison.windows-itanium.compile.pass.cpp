//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// This test makes sure that we use the implementation for comparing type_info
// objects that matches what the compiler emits on Windows Itanium and NT-POSIX:
// a type_info object that images that no link joins may each keep has the
// high bit of its name pointer set.

// REQUIRES: target={{.+}}-windows-{{itanium|ntposix}}

#include <typeinfo>

#if _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION != 3
#  error "_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION should be 3 (NonUniqueARMRTTIBit) on Windows Itanium and NT-POSIX"
#endif
