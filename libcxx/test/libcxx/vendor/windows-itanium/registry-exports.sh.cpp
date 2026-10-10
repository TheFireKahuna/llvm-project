//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// REQUIRES: has-filecheck

// The shared library registers its destructors with the process's
// termination registries, which it imports from clang_rt.wincrt_dynamic.dll
// as every image does, and exports none of them.

// RUN: llvm-readobj --coff-exports "%{install-prefix}/bin/libc++.dll" \
// RUN:   | FileCheck %s --check-prefix=EXPORTS --implicit-check-not=__wincrt_ \
// RUN:       --implicit-check-not=__cxa_finalize \
// RUN:       --implicit-check-not=__cxa_thread_atexit_impl
// RUN: llvm-readobj --coff-imports "%{install-prefix}/bin/libc++.dll" \
// RUN:   | FileCheck %s --check-prefix=IMPORTS

// EXPORTS:     Name: __cxa_thread_atexit{{$}}

// IMPORTS:     Name: clang_rt.wincrt_dynamic.dll
// IMPORTS-DAG: Symbol: __cxa_thread_atexit_impl
// IMPORTS-DAG: Symbol: __cxa_thread_finalize
// IMPORTS-DAG: Symbol: __wincrt_detach_image
