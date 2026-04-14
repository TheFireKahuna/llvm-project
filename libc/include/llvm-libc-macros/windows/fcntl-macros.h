#ifndef LLVM_LIBC_MACROS_WINDOWS_FCNTL_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_FCNTL_MACROS_H

// File access mode mask
#define O_ACCMODE 00000003

// File access mode flags (match Linux values — internal convention)
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR 00000002

// File creation flags
#define O_CREAT 00000100
#define O_EXCL 00000200
#define O_TRUNC 00001000
#define O_APPEND 00002000
#define O_NONBLOCK 00004000
#define O_CLOEXEC 02000000
#define O_DIRECTORY 00200000
#define O_NOFOLLOW 00400000
#define O_PATH 010000000
#define O_TMPFILE 020200000
#define O_DSYNC 00010000
#define O_SYNC 04010000
#define O_DIRECT 00040000
#define O_NOCTTY 00000400
#define O_NDELAY O_NONBLOCK
#define O_TTY_INIT 0

// POSIX execute/search access modes (map to O_PATH on this platform)
#define O_EXEC O_PATH
#define O_SEARCH O_PATH

// Special directory fd (not meaningful on Windows, but defined for compat)
#define AT_FDCWD -100
#define AT_EACCESS 0x200
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

// fcntl commands
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_GETOWN 9
#define F_SETOWN 8
#define F_DUPFD_CLOEXEC 1030

// fcntl fd flags
#define FD_CLOEXEC 1

// flock l_type values
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

// POSIX requires SEEK_* to be defined in <fcntl.h>
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#endif // LLVM_LIBC_MACROS_WINDOWS_FCNTL_MACROS_H
