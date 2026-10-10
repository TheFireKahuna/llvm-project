/*===---- unistd.h - POSIX unistd.h for the UCRT ---------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __CLANG_UNISTD_H
#define __CLANG_UNISTD_H

/* The UCRT has no unistd.h. It declares the part of it that it implements, the
 * file descriptor, process and directory functions under their POSIX names, in
 * io.h, process.h and direct.h. The UCRT's access tests whether a file exists,
 * can be read and can be written, but not whether it can be executed, so X_OK
 * is not defined. */
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define W_OK 2
#define R_OK 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* __CLANG_UNISTD_H */
