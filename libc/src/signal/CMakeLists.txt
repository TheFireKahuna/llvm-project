if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  raise
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.raise
)

add_entrypoint_object(
  kill
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.kill
)

add_entrypoint_object(
  killpg
  SRCS
    killpg.cpp
  HDRS
    killpg.h
  DEPENDS
    libc.hdr.types.pid_t
    libc.src.errno.errno
    libc.src.signal.kill
)

add_entrypoint_object(
  psignal
  SRCS
    psignal.cpp
  HDRS
    psignal.h
    signal_print_utils.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.errno.errno
    libc.src.__support.StringUtil.signal_to_string
    libc.src.__support.integer_to_string
    libc.src.__support.File.file
    libc.src.__support.File.platform_file
    libc.src.stdio.stderr
)

add_entrypoint_object(
  psiginfo
  SRCS
    psiginfo.cpp
  HDRS
    psiginfo.h
    signal_print_utils.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.hdr.types.siginfo_t
    libc.src.errno.errno
    libc.src.__support.StringUtil.error_to_string
    libc.src.__support.StringUtil.signal_to_string
    libc.src.__support.integer_to_string
    libc.src.__support.File.file
    libc.src.__support.File.platform_file
    libc.src.stdio.stderr
)

add_entrypoint_object(
  sigaction
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigaction
)

add_entrypoint_object(
  sigaltstack
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigaltstack
)

add_entrypoint_object(
  sigpending
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigpending
)

add_entrypoint_object(
  sigprocmask
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigprocmask
)

add_entrypoint_object(
  sigsuspend
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigsuspend
)

add_entrypoint_object(
  sigtimedwait
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigtimedwait
)

add_entrypoint_object(
  sigwait
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigwait
)

add_entrypoint_object(
  sigwaitinfo
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigwaitinfo
)

add_entrypoint_object(
  sigemptyset
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigemptyset
)

add_entrypoint_object(
  sigaddset
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigaddset
)

add_entrypoint_object(
  signal
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.signal
)

add_entrypoint_object(
  sigfillset
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigfillset
)

add_entrypoint_object(
  sigdelset
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigdelset
)

add_entrypoint_object(
  sigismember
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigismember
)

add_entrypoint_object(
  sigqueue
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.sigqueue
)
