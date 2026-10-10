//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium

// This file checks that the built static libraries don't contain dllexport
// directives in Windows Itanium builds, which export by visibility.

// RUN: llvm-readobj --coff-directives "%{lib-dir}/libc++.lib" | not grep -i "export:" > /dev/null

// RUN: llvm-readobj --coff-directives "%{lib-dir}/libc++experimental.lib" | not grep -i "export:" > /dev/null
