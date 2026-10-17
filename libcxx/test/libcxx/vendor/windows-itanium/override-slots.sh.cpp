//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// REQUIRES: has-filecheck

// The import library and the static library carry, for each overridable
// default, its slot in the executable's override table and, for a default that
// the standard specifies as calling another, that other's slot, with bit 16
// set when the forwarding drops the second argument. A program's link reads
// them to write the table. Slots never change.

// RUN: llvm-nm "%{lib-dir}/libc++.dll.lib" | FileCheck %s
// RUN: llvm-nm "%{lib-dir}/libc++.lib" | FileCheck %s

// CHECK-DAG: 00000001 A __ovr_version
// CHECK-DAG: 00000000 A __ovr_slot$_Znwy
// CHECK-DAG: 00000001 A __ovr_slot$_Znay
// CHECK-DAG: 00000000 A __ovr_fwd$_Znay
// CHECK-DAG: 00000002 A __ovr_slot$_ZdlPv
// CHECK-DAG: 00000003 A __ovr_slot$_ZdlPvy
// CHECK-DAG: 00000002 A __ovr_fwd$_ZdlPvy
// CHECK-DAG: 00000004 A __ovr_slot$_ZdaPv
// CHECK-DAG: 00000002 A __ovr_fwd$_ZdaPv
// CHECK-DAG: 00000005 A __ovr_slot$_ZdaPvy
// CHECK-DAG: 00000004 A __ovr_fwd$_ZdaPvy
// CHECK-DAG: 00000006 A __ovr_slot$_ZnwySt11align_val_t
// CHECK-DAG: 00000007 A __ovr_slot$_ZdlPvSt11align_val_t
// CHECK-DAG: 00000008 A __ovr_slot$_ZnaySt11align_val_t
// CHECK-DAG: 00000006 A __ovr_fwd$_ZnaySt11align_val_t
// CHECK-DAG: 00000009 A __ovr_slot$_ZdlPvySt11align_val_t
// CHECK-DAG: 00010007 A __ovr_fwd$_ZdlPvySt11align_val_t
// CHECK-DAG: 0000000a A __ovr_slot$_ZdaPvSt11align_val_t
// CHECK-DAG: 00000007 A __ovr_fwd$_ZdaPvSt11align_val_t
// CHECK-DAG: 0000000b A __ovr_slot$_ZdaPvySt11align_val_t
// CHECK-DAG: 0001000a A __ovr_fwd$_ZdaPvySt11align_val_t
// CHECK-DAG: 0000000c A __ovr_slot$_ZnwyRKSt9nothrow_t
// CHECK-DAG: 0000000d A __ovr_slot$_ZnayRKSt9nothrow_t
// CHECK-DAG: 0000000e A __ovr_slot$_ZnwySt11align_val_tRKSt9nothrow_t
// CHECK-DAG: 0000000f A __ovr_slot$_ZnaySt11align_val_tRKSt9nothrow_t
// CHECK-DAG: 00000010 A __ovr_slot$_ZdlPvRKSt9nothrow_t
// CHECK-DAG: 00000011 A __ovr_slot$_ZdaPvRKSt9nothrow_t
// CHECK-DAG: 00000012 A __ovr_slot$_ZdlPvSt11align_val_tRKSt9nothrow_t
// CHECK-DAG: 00000013 A __ovr_slot$_ZdaPvSt11align_val_tRKSt9nothrow_t
// CHECK-DAG: 00000014 A __ovr_slot$_ZNSt3__122__libcpp_verbose_abortEPKcz
