# ---------------------------------------------------------------------------
# Signal entry points (Windows)
#
# Engine code (signal_subsystem, signal_pool, signal_sigqueue_pool,
# signal_transports, signal_dispatch_engine, signal_process_control, etc.)
# lives in __support/OSUtil/windows/signal/ — targets are defined in
# __support/OSUtil/windows/CMakeLists.txt.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Sigset manipulation entrypoints (stateless, no signal subsystem dependency)
# ---------------------------------------------------------------------------

add_entrypoint_object(
  sigemptyset
  SRCS
    sigemptyset.cpp
  HDRS
    ../sigemptyset.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sigset_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigfillset
  SRCS
    sigfillset.cpp
  HDRS
    ../sigfillset.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.sigset_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigaddset
  SRCS
    sigaddset.cpp
  HDRS
    ../sigaddset.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.sigset_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigdelset
  SRCS
    sigdelset.cpp
  HDRS
    ../sigdelset.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.sigset_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigismember
  SRCS
    sigismember.cpp
  HDRS
    ../sigismember.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.sigset_t
    libc.src.errno.errno
)

# ---------------------------------------------------------------------------
# Signal entrypoints (all depend on signal_subsystem)
# ---------------------------------------------------------------------------

add_entrypoint_object(
  sigaction
  SRCS
    sigaction.cpp
  HDRS
    ../sigaction.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.hdr.types.struct_sigaction
    libc.src.errno.errno
)

add_entrypoint_object(
  signal
  SRCS
    signal.cpp
  HDRS
    ../signal.h
  DEPENDS
    libc.hdr.signal_macros
    libc.src.signal.sigaction
)

add_entrypoint_object(
  raise
  SRCS
    raise.cpp
  HDRS
    ../raise.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.src.errno.errno
)

add_entrypoint_object(
  kill
  SRCS
    kill.cpp
  HDRS
    ../kill.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.hdr.types.pid_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigpending
  SRCS
    sigpending.cpp
  HDRS
    ../sigpending.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.src.errno.errno
)

add_entrypoint_object(
  sigprocmask
  SRCS
    sigprocmask.cpp
  HDRS
    ../sigprocmask.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.hdr.types.sigset_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigaltstack
  SRCS
    sigaltstack.cpp
  HDRS
    ../sigaltstack.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.hdr.types.stack_t
    libc.src.errno.errno
)

add_entrypoint_object(
  sigtimedwait
  SRCS
    sigtimedwait.cpp
  HDRS
    ../sigtimedwait.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.src.errno.errno
    libc.src.pthread.cancel_internal
)

add_entrypoint_object(
  sigwait
  SRCS
    sigwait.cpp
  HDRS
    ../sigwait.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.sigset_t
    libc.src.errno.errno
    libc.src.pthread.cancel_internal
    libc.src.signal.sigtimedwait
)

add_entrypoint_object(
  sigwaitinfo
  SRCS
    sigwaitinfo.cpp
  HDRS
    ../sigwaitinfo.h
  DEPENDS
    libc.hdr.types.siginfo_t
    libc.hdr.types.sigset_t
    libc.src.pthread.cancel_internal
    libc.src.signal.sigtimedwait
)

add_entrypoint_object(
  sigsuspend
  SRCS
    sigsuspend.cpp
  HDRS
    ../sigsuspend.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.src.errno.errno
    libc.src.pthread.cancel_internal
)

add_entrypoint_object(
  sigqueue
  SRCS
    sigqueue.cpp
  HDRS
    ../sigqueue.h
  DEPENDS
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.__support.OSUtil.windows.signal_syscalls
    libc.hdr.types.pid_t
    libc.hdr.types.union_sigval
    libc.src.errno.errno
)
