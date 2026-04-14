//===-- Macros defined in dlfcn.h header file ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_DLFCN_MACROS_H
#define LLVM_LIBC_MACROS_DLFCN_MACROS_H

// POSIX flags for dlopen mode argument.
#define RTLD_LAZY 0x00001
#define RTLD_NOW 0x00002
#define RTLD_GLOBAL 0x00100
#define RTLD_LOCAL 0

// GNU extensions.
#define RTLD_NOLOAD 0x00004
#define RTLD_NODELETE 0x01000
#define RTLD_NEXT ((void *)-1l)
#define RTLD_DEFAULT ((void *)0)

// dlinfo request codes (GNU extension).
#define RTLD_DI_LMID 1
#define RTLD_DI_LINKMAP 2
#define RTLD_DI_CONFIGADDR 3
#define RTLD_DI_SERINFO 4
#define RTLD_DI_SERINFOSIZE 5
#define RTLD_DI_ORIGIN 6
#define RTLD_DI_PROFILENAME 7
#define RTLD_DI_PROFILEOUT 8
#define RTLD_DI_TLS_MODID 9
#define RTLD_DI_TLS_DATA 10
#define RTLD_DI_PHDR 11
#define RTLD_DI_MAX 11

#endif // LLVM_LIBC_MACROS_DLFCN_MACROS_H
