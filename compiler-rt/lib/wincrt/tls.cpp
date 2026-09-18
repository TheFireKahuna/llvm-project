//===-- tls.cpp - PE thread-local storage directory -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The linker merges .tls$* alphabetically between these bounds and .CRT$XL*
// into the callback array. The loader reads the directory and writes the
// image's TLS slot index. Itanium C++ initializes thread_local objects
// through their access wrappers, so no per-thread initializer callback is
// needed here.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#pragma section(".tls", long, read, write)
#pragma section(".tls$ZZZ", long, read, write)
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLZ", long, read)
#pragma section(".rdata$T", long, read)

extern "C" {

__declspec(allocate(".tls")) __declspec(selectany) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) __declspec(selectany) char _tls_end = 0;
__declspec(selectany) DWORD _tls_index = 0;

__declspec(allocate(".CRT$XLA"))
__declspec(selectany) PIMAGE_TLS_CALLBACK __xl_a = nullptr;
__declspec(allocate(".CRT$XLZ"))
__declspec(selectany) PIMAGE_TLS_CALLBACK __xl_z = nullptr;

// The callback array starts after the null sentinel that opens .CRT$XLA.
__declspec(allocate(".rdata$T"))
__declspec(selectany) IMAGE_TLS_DIRECTORY _tls_used = {
    reinterpret_cast<ULONG_PTR>(&_tls_start),
    reinterpret_cast<ULONG_PTR>(&_tls_end),
    reinterpret_cast<ULONG_PTR>(&_tls_index),
    reinterpret_cast<ULONG_PTR>(&__xl_a + 1),
    0,
    {0},
};

} // extern "C"

WINCRT_INCLUDE(_tls_used)
