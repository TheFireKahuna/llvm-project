add_header_library(
  mutex_common
  HDRS
    mutex_common.h
)

add_header_library(
  sleep
  HDRS
    sleep.h
)

add_header_library(
  spin_lock
  HDRS
    spin_lock.h
  DEPENDS
    .sleep
    libc.src.__support.CPP.atomic
)

add_header_library(
  cndvar_queue
  HDRS
    cndvar_queue.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.macros.attributes
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${LIBC_TARGET_OS})
endif()

if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.futex_utils)

  add_header_library(
    futex
    HDRS
      futex.h
    DEPENDS
      .${LIBC_TARGET_OS}.futex_utils
  )

  libc_set_definition(rwlock_default_spin_count "LIBC_COPT_RWLOCK_DEFAULT_SPIN_COUNT=${LIBC_CONF_RWLOCK_DEFAULT_SPIN_COUNT}")

  add_header_library(
    raw_mutex
    HDRS
      raw_mutex.h
    COMPILE_OPTIONS
      ${monotonicity_flags}
    DEPENDS
      .${LIBC_TARGET_OS}.futex_utils
      libc.src.__support.threads.sleep
      libc.src.__support.time.abs_timeout
      libc.src.__support.time.monotonicity
      libc.src.__support.CPP.optional
      libc.hdr.types.pid_t
  )

  add_header_library(
    raw_rwlock
    HDRS
      raw_rwlock.h
    COMPILE_OPTIONS
      ${rwlock_default_spin_count}
      ${monotonicity_flags}
    DEPENDS
      .raw_mutex
      libc.src.__support.common
      libc.src.__support.CPP.limits
  )

  add_header_library(
    unix_rwlock
    HDRS
      unix_rwlock.h
    DEPENDS
      .raw_rwlock
      libc.hdr.types.pid_t
      libc.src.__support.threads.identifier
  )

  # On Windows, mutex.h includes windows/unix_mutex.h (which provides the full
  # POSIX Mutex with robust support). On other platforms, it includes the shared
  # unix_mutex.h. The windows/mutex object library provides the platform
  # "syscalls" (death detection, robust list management).
  set(_mutex_deps .raw_mutex)
  if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.mutex)
    list(APPEND _mutex_deps .${LIBC_TARGET_OS}.mutex)
  endif()

  add_header_library(
    unix_mutex
    HDRS
      unix_mutex.h
    DEPENDS
      .raw_mutex
      .identifier
      .mutex_common
      libc.hdr.types.pid_t
      libc.src.__support.CPP.optional
      libc.src.__support.macros.optimization
  )

  add_header_library(
    mutex
    HDRS
      mutex.h
    DEPENDS
      .unix_mutex
      ${_mutex_deps}
  )

  add_object_library(
    fork_callbacks
    SRCS
      fork_callbacks.cpp
    HDRS
      fork_callbacks.h
    DEPENDS
      .mutex
      libc.src.__support.CPP.mutex
  )
elseif(NOT (LIBC_CONF_THREAD_MODE STREQUAL LIBC_THREAD_MODE_PLATFORM))
  add_header_library(
    mutex
    HDRS
      mutex.h
    DEPENDS
      .mutex_common
  )
endif()

add_header_library(
  thread_common
  HDRS
    thread.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.common
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.optional
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.stringstream
)

if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.thread)
  add_object_library(
    thread
    SRCS
      thread.cpp
    DEPENDS
      .mutex
      .${LIBC_TARGET_OS}.thread
      libc.hdr.stdint_proxy
      libc.src.__support.common
      libc.src.__support.fixedvector
      libc.src.__support.CPP.array
      libc.src.__support.CPP.mutex
      libc.src.__support.CPP.optional
  )
endif()

if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.callonce)
  add_object_library(
    callonce
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.callonce
  )
endif()

if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.CndVar)
  add_object_library(
    CndVar
    ALIAS
    DEPENDS
    .${LIBC_TARGET_OS}.CndVar
  )
endif()

add_object_library(
  barrier
  HDRS
    barrier.h
  SRCS
    barrier.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.pthread_macros
    libc.include.llvm-libc-types.pthread_barrier_t
    libc.include.llvm-libc-types.pthread_barrierattr_t
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.futex
)

if(TARGET libc.src.__support.threads.${LIBC_TARGET_OS}.semaphore)
  add_object_library(
    semaphore
    ALIAS
    DEPENDS
    .${LIBC_TARGET_OS}.semaphore
  )
endif()

if (LLVM_LIBC_FULL_BUILD)
  # identifier.h only needs the inline Thread/self definitions from thread.h.
  # Depending on the full thread object library creates a strong cycle:
  # thread -> mutex -> identifier -> thread.
  set(identifier_dependency_on_thread libc.src.__support.threads.thread_common)
endif()

add_header_library(
  identifier
  HDRS
    identifier.h
  DEPENDS
    libc.src.__support.OSUtil.osutil
    libc.src.__support.common
    libc.include.sys_syscall
    libc.hdr.types.pid_t
    ${identifier_dependency_on_thread}
)

add_header_library(
  cancel_support
  HDRS
    cancel_support.h
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.optimization
)

add_object_library(
  cancel_unwind
  SRCS
    cancel_unwind.cpp
  DEPENDS
    .cancel_support
    .thread
    .thread_common
    .${LIBC_TARGET_OS}.cancel_support
    libc.include.pthread
)

add_header_library(
  sched_support
  HDRS
    sched_support.h
)

add_header_library(
  cpuclock_support
  HDRS
    cpuclock_support.h
  DEPENDS
    libc.hdr.types.clockid_t
)
