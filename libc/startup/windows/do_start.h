//===-- Implementation header for Windows startup --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_STARTUP_WINDOWS_DO_START_H
#define LLVM_LIBC_STARTUP_WINDOWS_DO_START_H

// Exported from c.dll — performs internal libc initialization (allocator,
// TLS, argv/environ parsing, subsystem registration). Called by
// crt_do_start.obj before CRT section walk and main().
extern "C" void __libc_init(int *argc, char ***argv, char ***env);

// EXE entry — called by mainCRTStartup (crt1.obj). Calls __libc_init,
// walks .CRT$X* sections, calls main() via SEH-guarded wrapper, exits.
extern "C" [[noreturn]] void __libc_do_start();

#endif // LLVM_LIBC_STARTUP_WINDOWS_DO_START_H
