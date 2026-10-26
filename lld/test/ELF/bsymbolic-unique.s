# REQUIRES: x86
## STB_GNU_UNIQUE symbols have one definition in the process, so they stay
## preemptible under -Bsymbolic, -Bsymbolic-non-weak and --dynamic-list, as in
## GNU ld. --no-gnu-unique makes them STB_GLOBAL, which -Bsymbolic binds.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64 a.s -o a.o

# RUN: ld.lld -shared a.o -o def.so
# RUN: llvm-readobj -r def.so | FileCheck %s --check-prefix=UNIQUE
# RUN: ld.lld -shared -Bsymbolic a.o -o all.so
# RUN: llvm-readobj -r all.so | FileCheck %s --check-prefixes=UNIQUE,LOCAL-G
# RUN: ld.lld -shared -Bsymbolic-non-weak a.o -o nonweak.so
# RUN: llvm-readobj -r nonweak.so | FileCheck %s --check-prefixes=UNIQUE,LOCAL-G
# RUN: ld.lld -shared --dynamic-list=dl a.o -o list.so
# RUN: llvm-readobj -r list.so | FileCheck %s --check-prefix=UNIQUE

# UNIQUE:      R_X86_64_GLOB_DAT u 0x0
# LOCAL-G-NOT: R_X86_64_GLOB_DAT g
# UNIQUE:      R_X86_64_DTPMOD64 tu 0x0
# UNIQUE-NEXT: R_X86_64_DTPOFF64 tu 0x0

# RUN: ld.lld -shared --no-gnu-unique -Bsymbolic a.o -o global.so
# RUN: llvm-readobj -r global.so | FileCheck %s --check-prefix=GLOBAL

# GLOBAL-NOT: R_X86_64_DTPOFF64 tu
# GLOBAL-NOT: R_X86_64_GLOB_DAT

#--- dl
{ g; };

#--- a.s
.data
.globl u, g
.type u, @gnu_unique_object
u: .long 1
.type g, @object
g: .long 2

.section .tbss,"awT",@nobits
.globl tu
.type tu, @gnu_unique_object
tu: .zero 4

.text
.globl f
f:
  movq u@GOTPCREL(%rip), %rax
  movq g@GOTPCREL(%rip), %rax
  data16 leaq tu@tlsgd(%rip), %rdi
  data16 data16 rex64 call __tls_get_addr@PLT
  ret
