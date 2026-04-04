# Platform-specific dependencies.
set(_sem_os_semaphore_dep)
if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.semaphore)
  set(_sem_os_semaphore_dep
    libc.src.__support.threads.${LIBC_TARGET_OS}.semaphore)
endif()

if(${LIBC_TARGET_OS} STREQUAL "windows")
  set(named_sem_dep libc.src.__support.threads.windows.named_semaphore)
  # sem_wait/sem_timedwait are POSIX cancellation points.
  set(cancel_dep libc.src.__support.OSUtil.windows.signal_subsystem)
else()
  set(named_sem_dep)
  set(cancel_dep)
endif()

add_entrypoint_object(
  sem_init
  SRCS
    sem_init.cpp
  HDRS
    sem_init.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
)

add_entrypoint_object(
  sem_destroy
  SRCS
    sem_destroy.cpp
  HDRS
    sem_destroy.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
)

add_entrypoint_object(
  sem_post
  SRCS
    sem_post.cpp
  HDRS
    sem_post.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
    ${named_sem_dep}
)

add_entrypoint_object(
  sem_wait
  SRCS
    sem_wait.cpp
  HDRS
    sem_wait.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
    ${named_sem_dep}
    ${cancel_dep}
)

add_entrypoint_object(
  sem_trywait
  SRCS
    sem_trywait.cpp
  HDRS
    sem_trywait.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
    ${named_sem_dep}
)

add_entrypoint_object(
  sem_timedwait
  SRCS
    sem_timedwait.cpp
  HDRS
    sem_timedwait.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.hdr.types.struct_timespec
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
    libc.src.__support.time.abs_timeout
    ${named_sem_dep}
    ${cancel_dep}
)

add_entrypoint_object(
  sem_getvalue
  SRCS
    sem_getvalue.cpp
  HDRS
    sem_getvalue.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${_sem_os_semaphore_dep}
    ${named_sem_dep}
)

add_entrypoint_object(
  sem_open
  SRCS
    sem_open.cpp
  HDRS
    sem_open.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.types.mode_t
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${named_sem_dep}
)

add_entrypoint_object(
  sem_close
  SRCS
    sem_close.cpp
  HDRS
    sem_close.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sem_t
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${named_sem_dep}
)

add_entrypoint_object(
  sem_unlink
  SRCS
    sem_unlink.cpp
  HDRS
    sem_unlink.h
  DEPENDS
    libc.hdr.errno_macros
    libc.include.semaphore
    libc.src.__support.common
    libc.src.__support.libc_errno
    ${named_sem_dep}
)
