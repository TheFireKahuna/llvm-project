// DEFINE: %{pp} = %clang_cc1 -E %s -o -
// RUN: %{pp} -triple x86_64-unknown-windows-itanium | FileCheck %s --check-prefix=PREFIX
// RUN: %{pp} -triple aarch64-pc-windows-ntposix | FileCheck %s --check-prefix=PREFIX
// RUN: %{pp} -triple x86_64-unknown-linux-gnu -ffunction-type-prefix | FileCheck %s --check-prefix=PREFIX
// RUN: %{pp} -triple x86_64-unknown-windows-itanium -fno-function-type-prefix | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %{pp} -triple x86_64-pc-windows-msvc | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %{pp} -triple i686-unknown-windows-itanium | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %{pp} -triple x86_64-unknown-linux-gnu -fsanitize=kcfi | FileCheck %s --check-prefix=NO-PREFIX
// RUN: not %clang --target=x86_64-unknown-linux-gnu -ffunction-type-prefix -E %s 2>&1 | FileCheck %s --check-prefix=DRIVER

/// Every function carries a type prefix on the 64-bit forms of Windows
/// Itanium and NT-POSIX, whether or not KCFI checks calls, and elsewhere only
/// where -cc1 is told so. The option is not a driver option.

// PREFIX: int FunctionTypePrefix();
// NO-PREFIX: int NoFunctionTypePrefix();
// DRIVER: error: unknown argument '-ffunction-type-prefix'

#if __has_feature(function_type_prefix)
int FunctionTypePrefix();
#else
int NoFunctionTypePrefix();
#endif
