# Detect whether the code being configured targets one of the Windows
# environments that use the Itanium C++ ABI.
#
# Sets:
#   WIN32_ITANIUM         - TRUE when targeting *-windows-itanium.
#   WIN32_NTPOSIX         - TRUE when targeting *-windows-ntposix.
#   WIN32_ITANIUM_TRIPLE  - The triple that set either of them, else empty.
#
# CMake reports both as plain Windows, and neither compiler defines _MSC_VER,
# so the environment is read from the triple of the code being built. The
# module is included by projects that do not have LLVM's CMake modules on
# their module path, so the helper is included by its path.

include(
  ${CMAKE_CURRENT_LIST_DIR}/../../llvm/cmake/modules/LLVMTargetTriple.cmake)

llvm_get_effective_target_triple(_effective_triple)
set(WIN32_ITANIUM FALSE)
set(WIN32_NTPOSIX FALSE)
set(WIN32_ITANIUM_TRIPLE "")
if(_effective_triple MATCHES "-windows-itanium")
  set(WIN32_ITANIUM TRUE)
  set(WIN32_ITANIUM_TRIPLE "${_effective_triple}")
elseif(_effective_triple MATCHES "-windows-ntposix")
  set(WIN32_NTPOSIX TRUE)
  set(WIN32_ITANIUM_TRIPLE "${_effective_triple}")
endif()
unset(_effective_triple)
