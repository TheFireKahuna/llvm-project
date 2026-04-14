if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  getgrgid
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getgrgid
)

add_entrypoint_object(
  getgrnam
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getgrnam
)

add_entrypoint_object(
  getgrgid_r
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getgrgid_r
)

add_entrypoint_object(
  getgrnam_r
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getgrnam_r
)
