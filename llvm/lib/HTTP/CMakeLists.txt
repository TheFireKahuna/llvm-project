# Link LibCURL if the user wants it
if (LLVM_ENABLE_CURL)
  set(imported_libs CURL::libcurl)
endif()

# Link cpp-httplib if the user wants it
if (LLVM_ENABLE_HTTPLIB)
  set(imported_libs ${imported_libs} httplib::httplib)
endif()

# Use WinHTTP on Windows
if (WIN32)
  set(imported_libs ${imported_libs} winhttp.lib crypt32.lib)
endif()

# This is no component library so the LLVM dylib does not gain a dependency on curl
add_llvm_library(LLVMHTTP
  HTTPClient.cpp
  HTTPServer.cpp
  StreamedHTTPResponseHandler.cpp

  LINK_LIBS
  ${imported_libs}

  LINK_COMPONENTS
  Support
)

# Only an HTTP request calls into WinHTTP and CryptoAPI, so delay load them to
# keep them from loading in every process of a tool that links this library.
# LLVMSupport provides the delay load helper.
if (LLVM_RUNTIME_WIN32 AND MSVC)
  target_link_options(LLVMHTTP INTERFACE
    "$<$<NOT:$<LINK_LANGUAGE:Swift>>:LINKER:/DELAYLOAD:winhttp.dll>"
    "$<$<NOT:$<LINK_LANGUAGE:Swift>>:LINKER:/DELAYLOAD:crypt32.dll>")
endif()
