set(CRT_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT WIN32)
  message(STATUS "CRT tests: Skipping (not Windows target)")
  return()
endif()

if(NOT TARGET crt)
  message(STATUS "CRT tests: Skipping (crt target not built)")
  return()
endif()

set(CRT_TEST_DEPS ${SANITIZER_COMMON_LIT_TEST_DEPS} crt)

set(CRT_TESTSUITES ${CMAKE_CURRENT_BINARY_DIR}/TestCases)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X64")
    set(CRT_TEST_TARGET_ARCH "x86_64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(CRT_TEST_TARGET_ARCH "aarch64")
  endif()
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i[3-6]86|X86")
    set(CRT_TEST_TARGET_ARCH "i386")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM")
    set(CRT_TEST_TARGET_ARCH "arm")
  endif()
endif()

get_compiler_rt_output_dir(${CRT_TEST_TARGET_ARCH} CRT_TEST_LIBDIR)

pythonize_bool(MSVC)

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
)

add_lit_testsuite(check-crt "Running the CRT tests"
  ${CRT_TESTSUITES}
  DEPENDS ${CRT_TEST_DEPS})

set_target_properties(check-crt PROPERTIES FOLDER "Compiler-RT Tests")
