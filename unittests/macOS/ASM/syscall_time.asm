%ifdef CONFIG
{
  "RegData": {
    "R8": "1",
    "R10": "1",
    "R12": "1",
    "R14": "1",
    "R15": "1"
  }
}
%endif

; SPDX-License-Identifier: MIT
; A `syscall` whose result can be checked: Linux `time` (201), which returns the seconds
; since the epoch and also stores them through rdi. Added by the macOS port, whose test
; runner answers this one call; upstream's TwoByte/0F_02 makes the same call but only
; checks that rax is not zero, which it also is if the call never happens.
;
; Checks: rax is a plausible current time (after 2023, before 2^32), the value stored
; through rdi equals rax, rcx holds the address of the next instruction and r11 the
; flags, as `syscall` sets them.

mov rsp, 0xe0001000
mov rdi, 0xe0000000
mov qword [rdi], 0

; Known flags before the call: CF set (bit 0) plus the always-set bit 1.
stc
pushfq
pop rbx

mov rax, 201
syscall
.after:

; rcx = address of the instruction after `syscall`
lea rdx, [rel .after]
xor r8, r8
cmp rcx, rdx
sete r8b

; [rdi] = rax
xor r10, r10
cmp [rdi], rax
sete r10b

; rax > 1,700,000,000 (November 2023)
xor r12, r12
mov r13, 1700000000
cmp rax, r13
seta r12b

; rax < 2^32
xor r14, r14
mov r13, 0x100000000
cmp rax, r13
setb r14b

; r11 = rflags as they were before the call
xor r15, r15
cmp r11, rbx
sete r15b

hlt
