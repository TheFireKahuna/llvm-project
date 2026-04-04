//===-- Definition of macros from sys/socket.h ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SYS_SOCKET_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SYS_SOCKET_MACROS_H

// Address families — same values as Linux for POSIX compatibility.
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL 1
#define AF_INET 2
#define AF_INET6 10

// Socket types.
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_RDM 4
#define SOCK_SEQPACKET 5

// Flags OR'd into type for socket()/socketpair()/accept4().
// Values match Linux — no conflict with the type values above.
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000

// Shutdown modes.
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

// Socket-level option namespace for setsockopt/getsockopt.
#define SOL_SOCKET 1

// Socket options (SOL_SOCKET level).
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_LINGER 13
#define SO_PEERCRED 17
#define SO_BROADCAST 6
#define SO_DONTROUTE 5
#define SO_OOBINLINE 10
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_ACCEPTCONN 30
#define SOMAXCONN 4096

// Message flags for send/recv/sendmsg/recvmsg.
#define MSG_OOB 0x01
#define MSG_PEEK 0x02
#define MSG_DONTROUTE 0x04
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define MSG_WAITALL 0x100
#define MSG_CTRUNC 0x08
#define MSG_TRUNC 0x20
#define MSG_EOR 0x80
#define MSG_CMSG_CLOEXEC 0x40000000

// SCM_RIGHTS for ancillary data.
#define SCM_RIGHTS 0x01

#endif // LLVM_LIBC_MACROS_WINDOWS_SYS_SOCKET_MACROS_H
