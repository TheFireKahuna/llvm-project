add_custom_target(libc_assert_unittests)

add_libc_unittest(
  assert_test
  SUITE
    libc_assert_unittests
  SRCS
    assert_test.cpp
  DEPENDS
    libc.src.assert.__assert_fail
    # These are necessary for now because dependencies are not properly added.
    libc.src.signal.raise
    libc.src.stdlib._Exit
    libc.src.stdlib.abort
    # EXPECT_DEATH body calls extern-C close(); under freestanding-unit
    # there is no c.lib to resolve it, so pull the entrypoint directly.
    libc.src.unistd.close
)
