//===-- Definition of Windows sys/ioctl macros ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// NTPOSIX exposes the Linux/POSIX tty ioctl number space to user code. The
// Windows backend translates the supported requests onto modern ConDrv state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SYS_IOCTL_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SYS_IOCTL_MACROS_H

// NTPOSIX reuses the Linux tty ioctl request numbers for source
// compatibility, but we define them directly here so Windows builds do not
// depend on generating the Linux macro header subtree.
#define FIONREAD 0x541B
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCNOTTY 0x5422
#define TIOCGETD 0x5424

#endif // LLVM_LIBC_MACROS_WINDOWS_SYS_IOCTL_MACROS_H
