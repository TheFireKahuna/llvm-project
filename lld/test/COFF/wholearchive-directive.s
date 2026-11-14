# REQUIRES: x86

## /wholearchive:<lib> in a directive loads every member of the library,
## appending .lib to a name without an extension, wherever the command line
## names it, when a /defaultlib directive read it, and under /nodefaultlib, as
## link.exe does. The library is the first one of its file name read or queued.
## A bare /wholearchive also loads every member of a library the command line
## named that was read before it, except those already loaded, and not a
## default library.

# RUN: rm -rf %t && mkdir -p %t/sub && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=x86_64-windows-msvc
# RUN: echo '.globl main; main: call a; ret' | %{mc} -o main.obj
# RUN: echo '.section .drectve,"yni"; .ascii " /defaultlib:lib.lib"' | %{mc} -o dflt.obj
# RUN: echo '.section .drectve,"yni"; .ascii " /wholearchive:lib"' | %{mc} -o whole.obj
# RUN: echo '.section .drectve,"yni"; .ascii " /wholearchive"' | %{mc} -o late.obj
# RUN: echo '.globl a; a: ret' | %{mc} -o a.obj
# RUN: echo '.globl b; b: ret' | %{mc} -o b.obj
# RUN: echo '.globl c; c: ret' | %{mc} -o c.obj
# RUN: echo '.globl d; d: ret' | %{mc} -o d.obj
# RUN: llvm-lib -out:lib.lib a.obj b.obj
# RUN: llvm-lib -out:sub/lib.lib c.obj
# RUN: llvm-lib -out:d.lib d.obj
# DEFINE: %{link} = lld-link -entry:main -out:out.exe -verbose

# RUN: %{link} lib.lib whole.obj main.obj 2>&1 | FileCheck %s
# RUN: %{link} whole.obj main.obj lib.lib 2>&1 | FileCheck %s
# RUN: %{link} dflt.obj whole.obj main.obj 2>&1 | FileCheck %s
# RUN: %{link} whole.obj main.obj -nodefaultlib 2>&1 | FileCheck %s
# RUN: %{link} whole.obj main.obj -nodefaultlib:lib.lib 2>&1 | FileCheck %s
# RUN: %{link} main.obj lib.lib late.obj -defaultlib:d.lib 2>&1 | \
# RUN:   FileCheck %s --check-prefix=BARE --implicit-check-not=d.obj
# CHECK: Loaded lib.lib(b.obj) for <whole-archive>
# BARE:  Loaded lib.lib(a.obj) for a
# BARE:  Loaded lib.lib(b.obj) for <whole-archive>

# RUN: %{link} sub/lib.lib whole.obj main.obj lib.lib 2>&1 | \
# RUN:   FileCheck %s --check-prefix=SUB --implicit-check-not=b.obj
# RUN: %{link} whole.obj main.obj sub/lib.lib lib.lib 2>&1 | \
# RUN:   FileCheck %s --check-prefix=SUB --implicit-check-not=b.obj
# SUB: Loaded lib.lib(c.obj) for <whole-archive>
