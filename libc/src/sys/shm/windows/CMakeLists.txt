# Windows implementations of SysV shared memory functions
#
# All entry points are thin wrappers that delegate to engine code in
# __support/OSUtil/windows/memory/ via windows_syscalls:: wrappers in
# __support/OSUtil/windows/syscall_wrappers/.

add_entrypoint_object(
  shmget
  SRCS
    shmget.cpp
  HDRS
    ../shmget.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.libc_errno
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.sysv_shm_ops
    libc.hdr.types.key_t
    libc.hdr.types.size_t
)

add_entrypoint_object(
  shmat
  SRCS
    shmat.cpp
  HDRS
    ../shmat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.libc_errno
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.sysv_shm_ops
)

add_entrypoint_object(
  shmdt
  SRCS
    shmdt.cpp
  HDRS
    ../shmdt.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.libc_errno
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.sysv_shm_ops
)

add_entrypoint_object(
  shmctl
  SRCS
    shmctl.cpp
  HDRS
    ../shmctl.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.libc_errno
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.sysv_shm_ops
    libc.hdr.types.struct_shmid_ds
)
