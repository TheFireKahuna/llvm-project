//===-- BCrypt primitives declarations --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// bcryptprimitives.dll declarations without a Windows SDK dependency.
//
// Kept standalone so early-startup code and ProcessPrng users do not need to
// pull in the broader Win32 kernel32 surface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_BCRYPTPRIMITIVES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_BCRYPTPRIMITIVES_H

#include "include/__llvm-libc-common.h"
#include "hdr/types/size_t.h"

#ifdef __cplusplus
extern "C" {
#endif

// ProcessPrng: always-succeeding CSPRNG (Win10 20H2+). This is a direct call
// into the CNG primitive layer with no failure path. LIBC_MSABI pins the
// MS x64 calling convention; without it a SysV-default TU would miscall
// bcryptprimitives.dll.
LIBC_MSABI __LIBC_EXTERN_DLLIMPORT_ATTR int ProcessPrng(unsigned char *pbData,
                                                        size_t cbData);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_BCRYPTPRIMITIVES_H
