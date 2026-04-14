//===-- Definition of NTSTATUS type ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_NTSTATUS_H
#define LLVM_LIBC_TYPES_NTSTATUS_H

typedef long NTSTATUS;

// Success
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_ALERTED ((NTSTATUS)0x00000101)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)

// Errors (high bit set)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)

// Helper macros
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#define NT_ERROR(Status) ((NTSTATUS)(Status) < 0)

#endif // LLVM_LIBC_TYPES_NTSTATUS_H
