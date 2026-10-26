/* SPDX-License-Identifier: APSL-2.0
 * Adapted from the UMI port; see uefi/README.md for provenance.
 * Relocate FD and XNUP without destroying uncopied input on overlap.
 * Preserve the incoming x0/x1 handoff values. No stack or boot services.
 */
.text
.global _start
_start:
image_start:
  adr x20, firmware
  b relocate
  .quad FD_BASE
  .quad FD_SIZE
  .quad 0
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0
relocate:
  mov x26, x0
  mov x27, x1
  ldr x23, =FD_SIZE
  add x19, x20, x23
  mov x21, #0
  ldr w3, [x19]
  ldr w4, =0x50554e58
  cmp w3, w4
  b.ne fd_first
  ldr w21, [x19, #4]
  ldr w4, =0x03fff000
  cmp w21, w4
  b.hi reject
  add x21, x21, #23
  and x21, x21, #~15
  /* Payload output must not overwrite the code performing the transfer. */
  ldr x8, =PAYLOAD_BASE
  add x9, x8, x21
  adr x10, image_start
  cmp x8, x20
  b.hs payload_code_safe
  cmp x9, x10
  b.hi reject
payload_code_safe:
  /* If FD output overlaps payload input, save the payload first. */
  ldr x8, =FD_BASE
  add x9, x8, x23
  add x10, x19, x21
  cmp x8, x10
  b.hs fd_first
  cmp x9, x19
  b.ls fd_first
  /* Do not save payload over the still-needed firmware or shim itself. */
  ldr x8, =PAYLOAD_BASE
  add x9, x8, x21
  adr x10, image_start
  cmp x8, x19
  b.hs payload_first
  cmp x9, x10
  b.hi reject
payload_first:
  ldr x0, =PAYLOAD_BASE
  mov x1, x19
  mov x2, x21
  bl copy16
  mov x21, #0
fd_first:
  /* A running shim must never be overwritten by its own FD copy. */
  ldr x8, =FD_BASE
  add x9, x8, x23
  adr x10, image_start
  cmp x8, x20
  b.hs fd_copy
  cmp x9, x10
  b.hi reject
fd_copy:
  ldr x0, =FD_BASE
  mov x1, x20
  mov x2, x23
  bl copy16
  cbz x21, enter_firmware
  ldr x0, =PAYLOAD_BASE
  mov x1, x19
  mov x2, x21
  bl copy16
enter_firmware:
  mov x0, x26
  mov x1, x27
  dsb sy
  ic iallu
  dsb sy
  isb
  ldr x5, =FD_BASE
  br x5
reject:
  /* Reject impossible/self-overlapping layouts rather than execute corruption. */
  wfe
  b reject
/* memmove for aligned, nonzero, multiple-of-16 extents. */
copy16:
  cmp x0, x1
  b.eq copy_done
  b.lo copy_forward
  add x3, x1, x2
  cmp x0, x3
  b.hs copy_forward
  add x0, x0, x2
  add x1, x1, x2
copy_backward:
  ldp x3, x4, [x1, #-16]!
  stp x3, x4, [x0, #-16]!
  subs x2, x2, #16
  b.ne copy_backward
  ret
copy_forward:
  ldp x3, x4, [x1], #16
  stp x3, x4, [x0], #16
  subs x2, x2, #16
  b.ne copy_forward
copy_done:
  ret
.ltorg
.balign 16
firmware:
