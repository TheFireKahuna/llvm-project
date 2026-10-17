//===-- tls.cpp - Thread-local storage directory --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Code that uses a thread-local variable names _tls_index, which links this
// file and with it the directory the loader reads. The linker merges the
// .tls$ sections, in name order, between _tls_start and _tls_end, and the
// .CRT$XL sections into the callback array between __xl_a and __xl_z.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLZ", read)
#pragma section(".rdata$T", read)

extern "C" __declspec(allocate(".tls")) char _tls_start = 0;
extern "C" __declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
// Written by the loader.
extern "C" ULONG _tls_index = 0;

extern "C" __declspec(allocate(".CRT$XLA")) const PIMAGE_TLS_CALLBACK __xl_a =
    nullptr;
extern "C" __declspec(allocate(".CRT$XLZ")) const PIMAGE_TLS_CALLBACK __xl_z =
    nullptr;

// The callback array starts after the null that opens .CRT$XLA. The linker
// writes the alignment of the template into Characteristics.
extern "C" __declspec(allocate(".rdata$T"))
const IMAGE_TLS_DIRECTORY _tls_used = {
    reinterpret_cast<ULONG_PTR>(&_tls_start),
    reinterpret_cast<ULONG_PTR>(&_tls_end),
    reinterpret_cast<ULONG_PTR>(&_tls_index),
    reinterpret_cast<ULONG_PTR>(&__xl_a + 1),
    0,
    {0},
};

WINCRT_INCLUDE(_tls_used)
