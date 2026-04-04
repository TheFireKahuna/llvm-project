if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  writev
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.writev
)

add_entrypoint_object(
  readv
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.readv
)

add_entrypoint_object(
  preadv
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.preadv
)

add_entrypoint_object(
  pwritev
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.pwritev
)
