#ifndef LLVM_LIBC_MACROS_FCNTL_MACROS_H
#define LLVM_LIBC_MACROS_FCNTL_MACROS_H

#ifdef __linux__
#include "linux/fcntl-macros.h"
#elif defined(__NTPOSIX__)
#include "windows/fcntl-macros.h"
#endif

// POSIX file access advice constants for posix_fadvise().
#ifndef POSIX_FADV_NORMAL
#define POSIX_FADV_NORMAL 0
#endif
#ifndef POSIX_FADV_RANDOM
#define POSIX_FADV_RANDOM 1
#endif
#ifndef POSIX_FADV_SEQUENTIAL
#define POSIX_FADV_SEQUENTIAL 2
#endif
#ifndef POSIX_FADV_WILLNEED
#define POSIX_FADV_WILLNEED 3
#endif
#ifndef POSIX_FADV_DONTNEED
#define POSIX_FADV_DONTNEED 4
#endif
#ifndef POSIX_FADV_NOREUSE
#define POSIX_FADV_NOREUSE 5
#endif

#endif // LLVM_LIBC_MACROS_FCNTL_MACROS_H
