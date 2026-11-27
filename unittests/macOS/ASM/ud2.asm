%ifdef CONFIG
{
  "RegData": {
    "RAX": "0"
  }
}
%endif

; SPDX-License-Identifier: MIT
; A real invalid opcode: `ud2` must stop the run as a guest fault (fexmac
; FEXMAC_STOP_GUEST_FAULT, SIGILL, #UD), not as an instruction the translator cannot
; translate. Its correct outcome is that stop, recorded in unittests/ASM/Known_Failures_macOS.
mov rsp, 0xe0001000
mov eax, 0
ud2
hlt
