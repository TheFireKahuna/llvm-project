//===-- delayload_types.h - PE/COFF delay-load metadata ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Minimal PE delay-load structures required by libc's delay-load helper.
// Kept local to startup/ so they do not leak back into the OSUtil Windows
// support layer now that kernel32.h is gone.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_STARTUP_WINDOWS_DELAYLOAD_TYPES_H
#define LLVM_LIBC_STARTUP_WINDOWS_DELAYLOAD_TYPES_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"

struct IMAGE_DELAYLOAD_DESCRIPTOR {
  union {
    DWORD AllAttributes;
    struct {
      DWORD RvaBased : 1;
      DWORD ReservedAttributes : 31;
    };
  } Attributes;
  DWORD DllNameRVA;
  DWORD ModuleHandleRVA;
  DWORD ImportAddressTableRVA;
  DWORD ImportNameTableRVA;
  DWORD BoundImportAddressTableRVA;
  DWORD UnloadInformationTableRVA;
  DWORD TimeDateStamp;
};

struct IMAGE_THUNK_DATA {
#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64ec__)
  unsigned long long u1;
#else
  DWORD u1;
#endif
};

#endif // LLVM_LIBC_STARTUP_WINDOWS_DELAYLOAD_TYPES_H
