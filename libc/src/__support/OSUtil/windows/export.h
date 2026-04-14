//===-- PE export attribute macro ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines LIBC_EXPORTED for symbols that must appear in the PE export table.
// Used by cross-process discovery (e.g., signal APC entry point resolution).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_EXPORT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_EXPORT_H

#define LIBC_EXPORTED __declspec(dllexport)

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_EXPORT_H
