add_custom_target(libc_stdlib_windows_unittests)

add_libc_unittest(
  arc4random_test
  SUITE
    libc_stdlib_windows_unittests
  SRCS
    arc4random_test.cpp
  DEPENDS
    libc.src.stdlib.arc4random
    libc.src.stdlib.arc4random_buf
    libc.src.stdlib.arc4random_uniform
    libc.test.UnitTest.Test
)

add_libc_unittest(
  posix_alloc_test
  SUITE
    libc_stdlib_windows_unittests
  SRCS
    posix_alloc_test.cpp
  DEPENDS
    libc.src.stdlib.malloc
    libc.src.stdlib.free
    libc.test.UnitTest.Test
)

add_libc_unittest(
  posix_alloc_bench_test
  SUITE
    libc_stdlib_windows_unittests
  SRCS
    posix_alloc_bench_test.cpp
  DEPENDS
    libc.src.stdlib.malloc
    libc.src.stdlib.free
    libc.test.UnitTest.Test
)
