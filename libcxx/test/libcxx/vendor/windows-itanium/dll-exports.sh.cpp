//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium

// Windows Itanium exports by visibility, as ELF does. The shared library
// exports the defaults that a program may replace under their ABI names, weak
// definitions included, so that a program that does not replace them links
// them from the library.

// RUN: llvm-readobj --coff-exports "%{install-prefix}/bin/libc++.dll" | FileCheck %s

// CHECK-DAG: Name: _ZdlPv{{$}}
// CHECK-DAG: Name: _Znwy{{$}}
// CHECK-DAG: Name: _Znay{{$}}
// CHECK-DAG: Name: _ZNSt3__122__libcpp_verbose_abortEPKcz{{$}}
