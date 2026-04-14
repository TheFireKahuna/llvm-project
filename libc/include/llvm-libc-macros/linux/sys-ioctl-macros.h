//===-- Definition of macros from sys/ioctl.h -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_LINUX_SYS_IOCTL_MACROS_H
#define LLVM_LIBC_MACROS_LINUX_SYS_IOCTL_MACROS_H

// TODO (michaelrj): Finish defining the broader ioctl macro set, including the
// _IO / _IOR / _IOW / _IOWR families. For now, keep the small set of concrete
// tty and file-status requests that llvm-libc actually uses.
#define TIOCGWINSZ 0x5413
#define TIOCGETD 0x5424
#define FIONREAD 0x541B

#endif // LLVM_LIBC_MACROS_LINUX_SYS_IOCTL_MACROS_H
