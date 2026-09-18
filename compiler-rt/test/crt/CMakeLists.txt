set(CRT_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# The early wincrt ExternalProject is a dependency of the parent runtimes
# build. In standalone compiler-rt builds, use the local wincrt target.
set(CRT_TEST_DEPS ${SANITIZER_COMMON_LIT_TEST_DEPS})
if(COMPILER_RT_TEST_EXTERNAL_WINCRT)
  if(NOT WIN32_ITANIUM)
    message(FATAL_ERROR "External wincrt tests require Windows Itanium")
  endif()
elseif(TARGET wincrt)
  list(APPEND CRT_TEST_DEPS wincrt)
else()
  message(FATAL_ERROR "CRT tests requested without a wincrt library")
endif()

set(CRT_TEST_TARGET_ARCH ${COMPILER_RT_DEFAULT_TARGET_ARCH})
get_compiler_rt_output_dir(${CRT_TEST_TARGET_ARCH} CRT_TEST_LIBDIR)

# Prefer the C++ runtimes from this build over an installed toolchain. These
# targets are configured before compiler-rt by runtimes/CMakeLists.txt.
set(CRT_TEST_INCLUDE_DIRS)
set(CRT_TEST_LINK_DIRS)
set(CRT_TEST_DLL_DIRS)
if(TARGET cxx-headers)
  get_target_property(CRT_TEST_INCLUDE_DIRS cxx-headers INTERFACE_INCLUDE_DIRECTORIES)
  list(APPEND CRT_TEST_DEPS cxx-headers)
endif()
foreach(runtime cxx unwind)
  if(TARGET ${runtime})
    list(APPEND CRT_TEST_DEPS ${runtime})
    if(TARGET ${runtime}_shared)
      set(library ${runtime}_shared)
      get_target_property(dll_dir ${library} RUNTIME_OUTPUT_DIRECTORY)
      list(APPEND CRT_TEST_DLL_DIRS "${dll_dir}")
    else()
      set(library ${runtime}_static)
    endif()
    get_target_property(lib_dir ${library} ARCHIVE_OUTPUT_DIRECTORY)
    list(APPEND CRT_TEST_LINK_DIRS "${lib_dir}")
  endif()
endforeach()
list(REMOVE_DUPLICATES CRT_TEST_LINK_DIRS)
list(REMOVE_DUPLICATES CRT_TEST_DLL_DIRS)

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
)

add_lit_testsuite(check-crt "Running the Windows Itanium CRT tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CRT_TEST_DEPS})
set_target_properties(check-crt PROPERTIES FOLDER "Compiler-RT Tests")
