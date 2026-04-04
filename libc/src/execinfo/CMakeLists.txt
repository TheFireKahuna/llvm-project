if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})

  add_entrypoint_object(
    backtrace
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.backtrace
  )

  add_entrypoint_object(
    backtrace_symbols
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.backtrace_symbols
  )

  add_entrypoint_object(
    backtrace_symbols_fd
    ALIAS
    DEPENDS
      .${LIBC_TARGET_OS}.backtrace_symbols_fd
  )
else()
  # Stub implementations for platforms without native backtrace support.
  add_entrypoint_object(
    backtrace
    SRCS
      backtrace.cpp
    HDRS
      backtrace.h
  )

  add_entrypoint_object(
    backtrace_symbols
    SRCS
      backtrace_symbols.cpp
    HDRS
      backtrace_symbols.h
  )

  add_entrypoint_object(
    backtrace_symbols_fd
    SRCS
      backtrace_symbols_fd.cpp
    HDRS
      backtrace_symbols_fd.h
  )
endif()
