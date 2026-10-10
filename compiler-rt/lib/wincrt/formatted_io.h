//===-- formatted_io.h - Formatted I/O ntdll.dll also exports -----*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Universal CRT's formatted I/O functions that ntdll.dll exports as well,
// in reduced forms, which the Windows SDK's private ntdllp.lib imports. Every
// entry object wraps them, so that a program that links ntdllp.lib still calls
// the runtime's definitions, and every image has the same &sprintf.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_FORMATTED_IO_H
#define COMPILER_RT_LIB_WINCRT_FORMATTED_IO_H

#define WINCRT_NTDLL_FORMATTED_IO(X)                                           \
  X(_snprintf)                                                                 \
  X(_snprintf_s)                                                               \
  X(_snscanf_s)                                                                \
  X(_snwprintf)                                                                \
  X(_snwprintf_s)                                                              \
  X(_snwscanf_s)                                                               \
  X(_swprintf)                                                                 \
  X(_vscprintf)                                                                \
  X(_vscwprintf)                                                               \
  X(_vsnprintf)                                                                \
  X(_vsnprintf_s)                                                              \
  X(_vsnwprintf)                                                               \
  X(_vsnwprintf_s)                                                             \
  X(_vswprintf)                                                                \
  X(sprintf)                                                                   \
  X(sprintf_s)                                                                 \
  X(sscanf)                                                                    \
  X(sscanf_s)                                                                  \
  X(swprintf)                                                                  \
  X(swprintf_s)                                                                \
  X(swscanf_s)                                                                 \
  X(vsprintf)                                                                  \
  X(vsprintf_s)                                                                \
  X(vswprintf_s)

#endif // COMPILER_RT_LIB_WINCRT_FORMATTED_IO_H
