//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium

// The shared library holds the process's termination registries, which every
// image that imports it registers its destructors with.

// RUN: llvm-readobj --coff-exports "%{install-prefix}/bin/libc++.dll" | FileCheck %s

// CHECK-DAG: Name: __cxa_atexit{{$}}
// CHECK-DAG: Name: __cxa_at_quick_exit{{$}}
// CHECK-DAG: Name: __cxa_finalize{{$}}
// CHECK-DAG: Name: __cxa_thread_atexit_impl{{$}}
// CHECK-DAG: Name: __cxa_thread_finalize{{$}}
// CHECK-DAG: Name: __llvm_kcfi_cxa_atexit{{$}}
// CHECK-DAG: Name: __llvm_kcfi_cxa_thread_atexit_impl{{$}}
// CHECK-DAG: Name: __wincrt_detach_image{{$}}
// CHECK-DAG: Name: __wincrt_register_executable{{$}}
