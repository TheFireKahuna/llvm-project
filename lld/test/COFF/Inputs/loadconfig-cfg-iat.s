## _load_config_used through the guard IAT fields, for /guard:cf images of
## either machine.
  .section .rdata,"dr"
  .p2align 3
  .globl _load_config_used
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .fill 100, 1, 0
