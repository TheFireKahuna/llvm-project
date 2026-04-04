if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  creat
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.creat
)

add_entrypoint_object(
  fcntl
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.fcntl
)

add_entrypoint_object(
  open
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.open
)

add_entrypoint_object(
  openat
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.openat
)

add_entrypoint_object(
  posix_fallocate
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.posix_fallocate
)

add_entrypoint_object(
  posix_openpt
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.posix_openpt
)
