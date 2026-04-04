add_custom_target(libc_signal_unittests)

add_libc_unittest(
  raise_test
  SUITE
    libc_signal_unittests
  SRCS
    raise_test.cpp
  DEPENDS
    libc.include.signal
    libc.src.signal.raise
)

if(NOT LIBC_TARGET_OS_IS_WINDOWS)
  add_libc_unittest(
    kill_test
    SUITE
      libc_signal_unittests
    SRCS
      kill_test.cpp
    DEPENDS
      libc.include.signal
      libc.src.signal.kill
      libc.include.sys_syscall
      libc.src.__support.OSUtil.osutil
      libc.test.UnitTest.ErrnoSetterMatcher
  )
endif()

add_libc_unittest(
  sigaction_test
  SUITE
    libc_signal_unittests
  SRCS
    sigaction_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.src.signal.raise
    libc.src.signal.sigaction
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_libc_unittest(
  sigprocmask_test
  SUITE
    libc_signal_unittests
  SRCS
    sigprocmask_test.cpp
  DEPENDS
    libc.src.errno.errno
    libc.src.signal.raise
    libc.src.signal.sigaddset
    libc.src.signal.sigemptyset
    libc.src.signal.sigprocmask
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_libc_unittest(
  sigaddset_test
  SUITE
    libc_signal_unittests
  SRCS
    sigaddset_test.cpp
  DEPENDS
    libc.include.signal
    libc.src.errno.errno
    libc.src.signal.sigaddset
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_libc_unittest(
  signal_test
  SUITE
    libc_signal_unittests
  SRCS
    signal_test.cpp
  DEPENDS
    libc.src.errno.errno
    libc.src.signal.raise
    libc.src.signal.signal
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_libc_unittest(
  psignal_test
  SUITE
    libc_signal_unittests
  SRCS
    psignal_test.cpp
  DEPENDS
    libc.include.signal
    libc.src.signal.psignal
)

add_libc_unittest(
  psiginfo_test
  SUITE
    libc_signal_unittests
  SRCS
    psiginfo_test.cpp
  DEPENDS
    libc.hdr.stdint_proxy
    libc.include.signal
    libc.src.signal.psiginfo
)

add_libc_unittest(
  sigfillset_test
  SUITE
    libc_signal_unittests
  SRCS
    sigfillset_test.cpp
  DEPENDS
    libc.include.signal
    libc.src.errno.errno
    libc.src.signal.raise
    libc.src.signal.sigfillset
    libc.src.signal.sigprocmask
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_libc_unittest(
  sigdelset_test
  SUITE
    libc_signal_unittests
  SRCS
    sigdelset_test.cpp
  DEPENDS
    libc.include.signal
    libc.src.errno.errno
    libc.src.signal.raise
    libc.src.signal.sigdelset
    libc.src.signal.sigfillset
    libc.src.signal.sigprocmask
    libc.test.UnitTest.ErrnoSetterMatcher
)

set(_sigaltstack_test_deps
    libc.hdr.signal_macros
    libc.hdr.stdint_proxy
    libc.src.errno.errno
    libc.src.signal.raise
    libc.src.signal.sigaltstack
    libc.src.signal.sigaction
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)
if(NOT LIBC_TARGET_OS_IS_WINDOWS)
  list(APPEND _sigaltstack_test_deps
    libc.src.__support.OSUtil.osutil
  )
endif()

add_libc_unittest(
  sigaltstack_test
  SUITE
    libc_signal_unittests
  SRCS
    sigaltstack_test.cpp
  DEPENDS
    ${_sigaltstack_test_deps}
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${LIBC_TARGET_OS})
endif()
