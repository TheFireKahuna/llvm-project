# REQUIRES: x86
# RUN: split-file %s %t.dir && cd %t.dir
# DEFINE: %{ro} = llvm-readobj --sections --coff-imports --coff-load-config o.exe
# DEFINE: %{mc} = llvm-mc -filetype=obj -triple=x86_64-windows
# RUN: echo '.globl main; main: call *__imp_f(%%rip); ret' | %{mc} -o main.obj
# RUN: echo '.globl __delayLoadHelper2; __delayLoadHelper2: ret' | %{mc} -o helper.obj
# RUN: echo '.data; .globl __MSSafeDelayLoadIAT; __MSSafeDelayLoadIAT: .long 0' | %{mc} -o symbol.obj
# RUN: echo '.globl __delayLoadHelper2; __delayLoadHelper2: ret; .data; .globl __MSSafeDelayLoadIAT; __MSSafeDelayLoadIAT: .long 0' | \
# RUN:   %{mc} -o safe.obj
# RUN: echo '.section .didat$b,"dr"; .quad 2; .section .didat,"dw"; .globl didat; didat: .quad 1' | %{mc} -o didat.obj
# RUN: echo '.section .drectve,"yn"; .ascii " /didatownsection"' | %{mc} -o own.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows %S/Inputs/loadconfig-cfg-x64.s -o loadconfig.obj
# RUN: llvm-lib -machine:x64 -def:foo.def -out:foo.lib

## The delay-load IAT's placement follows link.exe: unprotected in .data, unless
## the helper's own object defines __MSSafeDelayLoadIAT and the subsystem is
## above 6.3, when it is protected beside the IAT; /guard:cf protects it in its
## own .didat otherwise. /didatownsection, on the command line or in a
## directive, moves any protected table to .didat.

# RUN: lld-link @common.rsp helper.obj -subsystem:console,10.0 -didatownsection && %{ro} | FileCheck --check-prefix=DATA %s
# RUN: lld-link @common.rsp safe.obj -subsystem:console,6.3 && %{ro} | FileCheck --check-prefix=DATA %s
# RUN: lld-link @common.rsp helper.obj symbol.obj -subsystem:console,10.0 && %{ro} | FileCheck --check-prefix=DATA %s

## link.exe protects no table for a subsystem other than GUI or console, or
## under /nooptdidat.
# RUN: lld-link @common.rsp helper.obj -subsystem:native -guard:cf && %{ro} | FileCheck --check-prefix=DATA %s
# RUN: lld-link @common.rsp helper.obj -guard:cf -nooptdidat && %{ro} | FileCheck --check-prefix=DATA %s

# DATA:      ImportAddressTable: 0x30{{[0-9A-F]{2}$}}
# DATA:      GuardFlags [
# DATA-NOT:  DELAYLOAD_IAT

# RUN: lld-link @common.rsp safe.obj -subsystem:console,10.0 -lldmingw && %{ro} | FileCheck --check-prefix=RDATA %s
# RUN: lld-link @common.rsp safe.obj -subsystem:console,10.0 -guard:cf && %{ro} | FileCheck --check-prefix=RDATA %s

# RDATA:      Name: .rdata
# RDATA-NEXT: VirtualSize:
# RDATA-NEXT: VirtualAddress: 0x2000
# RDATA:      ImportAddressTable: 0x2{{[0-9A-F]{3}$}}
# RDATA:      GuardFlags [
# RDATA-NOT:  DELAYLOAD_IAT_IN_ITS_OWN_SECTION
# RDATA:      PROTECT_DELAYLOAD_IAT (0x1000)

# RUN: lld-link @common.rsp helper.obj didat.obj -guard:cf -opt:noref && \
# RUN:   %{ro} --hex-dump=.didat | FileCheck --check-prefixes=DIDAT,USER %s
# RUN: lld-link @common.rsp safe.obj -subsystem:console,6.3 -guard:cf && %{ro} | FileCheck --check-prefix=DIDAT %s
# RUN: lld-link @common.rsp safe.obj -subsystem:console,10.0 -didatownsection && %{ro} | FileCheck --check-prefix=DIDAT %s
# RUN: lld-link @common.rsp helper.obj own.obj -guard:cf && %{ro} | FileCheck --check-prefix=DIDAT %s
# RUN: lld-link @common.rsp safe.obj own.obj -subsystem:console,10.0 && %{ro} | FileCheck --check-prefix=DIDAT %s
# RUN: lld-link @common.rsp safe.obj -subsystem:console,6.3 -guard:cf -didatownsection && %{ro} | FileCheck --check-prefix=DIDAT %s
# RUN: lld-link @common.rsp safe.obj -subsystem:console,10.0 -guard:cf -didatownsection && %{ro} | FileCheck --check-prefix=DIDAT %s

## The IAT is alone at the front of a writable .didat; input .didat pieces
## follow in name order, starting on the next page.
# DIDAT:      Name: .didat
# USER-NEXT:  VirtualSize: 0x1010
# DIDAT:      VirtualAddress: [[DIDAT:0x[0-9A-F]+]]
# DIDAT:      Characteristics [ (0xC0000040)
# USER:       0x140006000 01000000 00000000 02000000 00000000
# DIDAT:      ImportAddressTable: [[DIDAT]]
# DIDAT:      GuardFlags [
# DIDAT:        DELAYLOAD_IAT_IN_ITS_OWN_SECTION (0x2000)
# DIDAT:        PROTECT_DELAYLOAD_IAT (0x1000)

#--- common.rsp
-entry:main -nodefaultlib -out:o.exe main.obj loadconfig.obj foo.lib -delayload:foo.dll

#--- foo.def
LIBRARY foo.dll
EXPORTS
  f
