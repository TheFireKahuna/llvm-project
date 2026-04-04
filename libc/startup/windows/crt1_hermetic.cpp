//===-- Hermetic crt1 aggregator for Windows -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Empty translation unit. The `libc.startup.windows.crt1` object library
// exists only as an aggregator whose DEPS list pulls the full hermetic-EXE
// startup set (mainCRTStartup, __libc_do_start, __libc_init, and the PE
// infrastructure objects) into `add_libc_hermetic` test links via the
// recursive DEPS walk in `get_object_files_for_test`.
//
// The upstream hermetic test framework names the per-OS startup target
// `libc.startup.<os>.crt1`; this TU exists solely so that Windows can
// honor that contract without inventing a new CMake code path.
//
//===----------------------------------------------------------------------===//
