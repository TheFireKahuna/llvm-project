if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  openpty
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.openpty
)

add_entrypoint_object(
  login_tty
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.login_tty
)
