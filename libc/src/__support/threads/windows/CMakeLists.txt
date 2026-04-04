add_header_library(
  futex_word_type
  HDRS
    futex_word.h
  DEPENDS
    libc.hdr.stdint_proxy
)

add_header_library(
  spin_wait
  HDRS
    spin_wait.h
  DEPENDS
    .futex_word_type
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
)

add_object_library(
  wait_slot
  HDRS
    wait_slot.h
  SRCS
    wait_slot.cpp
  DEPENDS
    .futex_instrument
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.libc_fini_registry
    libc.src.__support.OSUtil.windows.lock_free_linkage
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.teb_tls
    libc.src.__support.OSUtil.windows.tls_cleanup
)

# Temporary diagnostic instrumentation for Futex / wait_slot. Provides
# process-global cache-line-separated atomic counters, per-worker phase
# words, and a closure-assertion dump used by futex_bench to diagnose
# the 8T/16T Phase [K] mutex hang. Always compiled into libc_shared so
# the bench (which defines LIBC_FUTEX_INSTRUMENT) can link against the
# shared symbols; when LIBC_FUTEX_INSTRUMENT is NOT defined in a user
# TU, the bump helpers collapse to empty inline functions — zero
# runtime cost for production paths.
add_object_library(
  futex_instrument
  HDRS
    futex_instrument.h
  SRCS
    futex_instrument.cpp
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.lock_free_linkage
)

# Registers the subsystem-specific thread-exit unlinkers with wait_slot.
# Split from wait_slot's object library so wait_slot.cpp needn't include
# futex_{addr,utils}.h. Called once at startup from wait_slot_startup_init.
add_object_library(
  futex_subsystem_register
  SRCS
    futex_subsystem_register.cpp
  DEPENDS
    .wait_slot
    .futex_utils
    libc.src.__support.OSUtil.windows.page_alloc
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.nt_capabilities
)

add_header_library(
  futex_utils
  HDRS
    futex_utils.h
  DEPENDS
    .futex_word_type
    .spin_wait
    .wait_slot
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.optional
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.time.abs_timeout
    libc.src.__support.time.units
)

add_header_library(
  thread_local_word
  HDRS
    thread_local_word.h
  DEPENDS
    .spin_wait
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.optional
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.src.__support.time.abs_timeout
)

add_header_library(
  thread_lifecycle_h
  HDRS
    thread_lifecycle.h
  DEPENDS
    .futex_utils
    .futex_word_type
    .thread_local_word
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
)

add_object_library(
  thread_registry
  HDRS
    thread_registry.h
  SRCS
    thread_registry.cpp
    # Crystalline FreeFn dispatch — must be linked into the same
    # archive so the domain template's slow-path retire chain can
    # resolve `free_thread_registry_node`.
    thread_registry_node.cpp
  DEPENDS
    .thread_lifecycle_h
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
)

add_object_library(
  thread_lifecycle
  SRCS
    thread_lifecycle.cpp
  DEPENDS
    .thread_lifecycle_h
    .robust_list_cleanup
    # futex_subsystem_register defines install_subsystem_unlinkers(),
    # called once from wait_slot_startup_init. Listed here so the object
    # library is always linked — there is no other consumer of its
    # symbols, and the linker would otherwise drop it, leaving
    # wait_slot_startup_init with an unresolved reference.
    .futex_subsystem_register
    libc.src.__support.OSUtil.windows.libc_fini_registry
    libc.src.__support.OSUtil.windows.windows_util
    libc.src.__support.OSUtil.windows.slab_pool
    libc.src.__support.OSUtil.windows.teb_tls
    libc.src.__support.OSUtil.windows.tls_cleanup
    libc.src.__support.OSUtil.windows.signal_h
)

add_object_library(
  robust_list_cleanup
  HDRS
    robust_list_cleanup.h
  SRCS
    robust_list_cleanup.cpp
  DEPENDS
    .futex_utils
    .thread_lifecycle_h
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.slab_pool
)

add_header_library(
  callonce
  HDRS
    ../callonce.h
    callonce.h
  DEPENDS
    .futex_utils
    libc.src.__support.macros.optimization
)

set(monotonicity_flags)
if (LIBC_CONF_TIMEOUT_ENSURE_MONOTONICITY)
  set(monotonicity_flags -DLIBC_COPT_TIMEOUT_ENSURE_MONOTONICITY=1)
else()
  set(monotonicity_flags -DLIBC_COPT_TIMEOUT_ENSURE_MONOTONICITY=0)
endif()

add_header_library(
  rwlock
  HDRS
    # The implementation is shared with the Unix path; the futex plumbing is
    # selected transitively by the Windows thread support targets below.
    ../unix_rwlock.h
  DEPENDS
    .futex_utils
    libc.src.__support.threads.raw_mutex
    libc.src.__support.common
    libc.src.__support.CPP.limits
    libc.src.__support.threads.identifier
  COMPILE_OPTIONS
    -DLIBC_COPT_RWLOCK_DEFAULT_SPIN_COUNT=${LIBC_CONF_RWLOCK_DEFAULT_SPIN_COUNT}
    ${monotonicity_flags}
)

add_header_library(
  timer_manager
  HDRS
    timer_manager.h
  DEPENDS
    .futex_utils
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.reactor
    libc.src.__support.OSUtil.windows.signal_h
    libc.src.__support.OSUtil.windows.slab_pool
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

add_object_library(
  CndVar
  SRCS
    CndVar.cpp
  HDRS
    ../CndVar.h
  DEPENDS
    .futex_utils
    libc.src.__support.threads.cndvar_queue
    libc.src.__support.threads.mutex
    libc.src.__support.threads.raw_mutex
    libc.src.__support.CPP.mutex
)


add_object_library(
  semaphore
  HDRS
    semaphore.h
  SRCS
    semaphore.cpp
  DEPENDS
    .futex_utils
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.optional
    libc.src.__support.time.abs_timeout
)

add_object_library(
  named_semaphore
  HDRS
    named_semaphore.h
  SRCS
    named_semaphore.cpp
  DEPENDS
    .semaphore
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.lazy_init
    libc.src.__support.OSUtil.windows.lazy_init_reset
    libc.src.__support.OSUtil.windows.libc_fini_registry
    libc.src.__support.OSUtil.windows.nt_path
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.security
    libc.src.__support.OSUtil.windows.temp_path
    libc.src.__support.time.abs_timeout
    libc.src.__support.OSUtil.windows.signal_subsystem
)

add_object_library(
  mutex
  HDRS
    mutex.h
  SRCS
    mutex.cpp
  DEPENDS
    .futex_utils
    .futex_word_type
    .thread_lifecycle_h
    .thread_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.libc_fini_registry
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.slab_pool
)

add_object_library(
  cancel_support
  SRCS
    cancel_support.cpp
  DEPENDS
    .thread_lifecycle_h
    .thread_registry
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.threads.cancel_support
)

add_object_library(
  sched_support
  SRCS
    sched_support.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.sched_macros
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.sched_helpers
    libc.src.__support.threads.sched_support
    libc.src.__support.OSUtil.windows.signal_subsystem
)

add_object_library(
  cpuclock_support
  SRCS
    cpuclock_support.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.time_macros
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.threads.cpuclock_support
)

add_object_library(
  thread
  SRCS
    thread.cpp
  DEPENDS
    .futex_utils
    .thread_lifecycle_h
    .thread_lifecycle
    libc.config.app_h
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.stringstream
    libc.src.__support.CPP.string_view
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.OSUtil.windows.app_init
    libc.src.__support.OSUtil.windows.libc_fini_registry
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.__support.OSUtil.windows.process_identity
    libc.src.__support.OSUtil.windows.teb_tls
    libc.src.__support.threads.thread_common
    libc.src.__support.time.abs_timeout
    libc.hdr.time_macros
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.sys.mman.mprotect
    libc.src.__support.OSUtil.windows.signal_subsystem
)
