# DetectWindowsItanium.cmake - Detect Windows Itanium target
#
# Provides:
#   WIN32_ITANIUM - TRUE if targeting Windows Itanium ABI
#   WIN32_NTPOSIX - TRUE if targeting Windows NTPOSIX (POSIX-on-NT)

include_guard(GLOBAL)

if(CMAKE_C_COMPILER_TARGET MATCHES "windows-itanium" OR
   CMAKE_CXX_COMPILER_TARGET MATCHES "windows-itanium" OR
   LLVM_RUNTIMES_TARGET MATCHES "windows-itanium" OR
   LLVM_HOST_TRIPLE MATCHES "windows-itanium")
  set(WIN32_ITANIUM TRUE)
else()
  set(WIN32_ITANIUM FALSE)
endif()

if(CMAKE_C_COMPILER_TARGET MATCHES "windows-ntposix" OR
   CMAKE_CXX_COMPILER_TARGET MATCHES "windows-ntposix" OR
   LLVM_RUNTIMES_TARGET MATCHES "windows-ntposix" OR
   LLVM_HOST_TRIPLE MATCHES "windows-ntposix")
  set(WIN32_NTPOSIX TRUE)
else()
  set(WIN32_NTPOSIX FALSE)
endif()
