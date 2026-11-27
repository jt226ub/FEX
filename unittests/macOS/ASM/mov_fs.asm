%ifdef CONFIG
{
  "RegData": {
    "RAX": "0"
  }
}
%endif

; SPDX-License-Identifier: MIT
; A valid x86-64 instruction FEX does not implement: `mov fs, ax` in 64-bit mode. The run
; must stop with FEXMAC_STOP_UNSUPPORTED_INSTRUCTION at the instruction, telling the
; embedder that the translator cannot run it, which differs from ud2.asm's guest #UD.
; Its correct outcome is that stop, recorded in unittests/ASM/Known_Failures_macOS.
mov rsp, 0xe0001000
mov eax, 0
mov fs, ax
hlt
