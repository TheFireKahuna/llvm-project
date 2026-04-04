add_custom_target(libc-support-threads-windows-tests)

add_libc_unittest(
  mutex_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    mutex_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.raw_mutex
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.mutex
    libc.src.__support.threads.windows.thread
    libc.src.__support.time.clock_gettime
    libc.test.UnitTest.Test
)

add_libc_unittest(
  thread_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    thread_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.mutex
    libc.src.__support.threads.windows.thread
    libc.test.UnitTest.Test
)

# Crystalline-W thread registry: basic-correctness coverage. Each test
# exercises one invariant of the new design (task_id monotonicity, handle
# resolve, iteration, foreign-thread invariance, etc.) using a small
# thread count so a regression in basic correctness is unambiguous.
add_libc_unittest(
  thread_registry_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    thread_registry_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.thread
    libc.src.__support.threads.windows.thread_registry
    libc.test.UnitTest.Test
)

# TID-recycling regression: structural-claim test. Captures a handle
# for an exited thread, drives churn that may recycle the NT TID, and
# asserts the captured handle never cross-resolves. Most informative
# single test — passing it justifies the rewrite.
add_libc_unittest(
  thread_registry_recycling_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    thread_registry_recycling_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.thread
    libc.src.__support.threads.windows.thread_registry
    libc.test.UnitTest.Test
)

# Concurrency / scale stress: hash split under contention, lookup vs.
# concurrent splits, iterate-all under churn, alert_all torrent.
# Higher thread counts than the basic test — exercises concurrency
# corners the unit tests can't reach.
add_libc_unittest(
  thread_registry_stress_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    thread_registry_stress_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.thread
    libc.src.__support.threads.windows.thread_registry
    libc.test.UnitTest.Test
)

# Long-running soak: thread create/destroy with PagefileUsage sampling
# (NtQueryInformationProcess(ProcessVmCounters)). Verifies process
# commit returns to within a bounded delta of baseline after a long
# run — catches BucketEntry / Lifecycle slab leaks invisible to
# live_count. Override LIBC_REGISTRY_SOAK_ITERS at compile time for
# overnight runs.
add_libc_unittest(
  thread_registry_soak_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    thread_registry_soak_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.thread
    libc.src.__support.threads.windows.thread_registry
    libc.test.UnitTest.Test
)

add_libc_unittest(
  cndvar_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    cndvar_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.windows.CndVar
    libc.src.__support.threads.windows.mutex
    libc.src.__support.threads.windows.thread
    libc.src.__support.time.clock_gettime
    libc.test.UnitTest.Test
)

add_libc_unittest(
  barrier_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    barrier_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.pthread_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.thread_common
    libc.src.__support.threads.barrier
    libc.src.__support.threads.windows.thread
    libc.test.UnitTest.Test
)

add_libc_unittest(
  futex_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    futex_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.sleep
    libc.src.__support.threads.windows.futex_utils
    libc.test.UnitTest.Test
)

add_libc_unittest(
  futex_stress_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    futex_stress_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.futex_utils
    libc.test.UnitTest.Test
)

# Deep state-machine coverage for Futex: 64-bit CAS-64 ABA, three-phase
# escalation with forced deep kernel park, unlock_notify / handoff reuse,
# notify_one fairness, StoreLoad-reorder regression, cross-futex
# independence. Complements futex_stress_test — different failure modes.
add_libc_unittest(
  futex_advanced_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    futex_advanced_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.futex_utils
    libc.src.__support.time.abs_timeout
    libc.test.UnitTest.Test
)

# Robust-mutex EOWNERDEAD death-detection tests. The recovery path is
# isolated from the general mutex_test to keep the failure mode clear
# (a regression here corrupts synchronization state across threads).
add_libc_unittest(
  raw_mutex_robust_test
  SUITE
    libc-support-threads-windows-tests
  SRCS
    raw_mutex_robust_test.cpp
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.mutex
    libc.src.__support.threads.mutex_common
    libc.src.__support.threads.raw_mutex
    libc.src.__support.threads.windows.mutex
    libc.test.UnitTest.Test
)

# Internal-primitive bench: exercises Futex directly via its LIBC_NAMESPACE
# API. Hermetic link — no c.dll or c.lib involvement. Gets its own private
# copies of Futex / PCB / slab-pool / registry state through the per-test
# `.__libc__` archive assembled from OBJECT_FILE_RAW of every DEPENDS entry.
add_libc_hermetic(
  futex_bench
  SUITE
    libc-support-threads-windows-tests
  NO_RUN_POSTBUILD
  SRCS
    futex_bench.cpp
  COMPILE_OPTIONS
    -fno-stack-protector
    -DLIBC_FUTEX_INSTRUMENT=1
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.threads.raw_mutex
    libc.src.__support.threads.windows.futex_instrument
    libc.src.__support.threads.windows.futex_utils
    libc.src.__support.threads.windows.thread
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)
if(TARGET futex_bench AND TARGET libc-unit-tests-build)
  add_dependencies(libc-unit-tests-build futex_bench)
endif()

# Sibling bench: futex_addr parking-lot primitive. Same link rules as
# futex_bench — hermetic exe linking a per-test .__libc__ archive so
# each run exercises its own futex_addr / wait_slot / bucket state.
# Shares the timing/WorkerPool/report harness via futex_bench_common.h.
add_libc_hermetic(
  futex_addr_bench
  SUITE
    libc-support-threads-windows-tests
  NO_RUN_POSTBUILD
  SRCS
    futex_addr_bench.cpp
  COMPILE_OPTIONS
    -fno-stack-protector
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.threads.raw_mutex
    libc.src.__support.threads.windows.futex_utils
    libc.src.__support.threads.windows.thread
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)
if(TARGET futex_addr_bench AND TARGET libc-unit-tests-build)
  add_dependencies(libc-unit-tests-build futex_addr_bench)
endif()
