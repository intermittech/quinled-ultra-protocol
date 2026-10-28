# Command contracts

The host and receiver/bootloader share the training, reboot and descriptor
contracts below. Physical qualification remains a separate release gate.

| Item | Scope | Contract |
|---|---|---|
| Training | Labelled alternating/zero/one traffic | NOP bytes 3..29 are ignored training payload, including repeated 55/AA/00/FF. TARGET/SEQ/flags are zero. Complete Ultra inventory, DARK reacquisition and the control gate are required. |
| Status reboot | COMMITTED acknowledgement | Hold COMMITTED and answer status until the selected session's PING acknowledges it, then reboot within the declared entry bound. The host sends at most three ACK copies, each followed by the full entry bound. Applications ignore nonzero-session PINGs; only idle session zero can confirm. |
| SENSE fixture | Synthetic descriptor | ID 254, root DOMAIN and NAME are mandatory in the fixture. Both independent and host semantic descriptor validators accept it. |
| PIXEL_FX ACKS | Segment acknowledgement | A column selecting PIXEL_FX reports SEQ zero and NEVER in every ACKS row. Segment acknowledgement uses PIXEL_FX part 0, arg3=output, arg1=segment/4 and SEQ[segment%4]. |
| L1 phase | Local packet scheduling | Kind-1 START/phase offsets use captured completion of the final CRC byte, before dispatch. No trailer TIME is needed. LINK local latency/jitter bounds qualify synchronization; the codec resolves signed millisecond offsets from acceptance. |

Marker polling follows timing.md's measured scheduling lock before POLL; the
marker envelope does not waive that requirement. Physical HIGH/LOW and residual
clock error still need real source/divider, PVT, cable, DMA and ISR qualification.

## Stored output levels

Each analog receiver stores two output levels: power-on and signal loss. SCENE
(`0x08`) targets a light and supplies these values through its definition:

| Payload byte | Meaning |
|---|---|
| 0 | Selector: 0 power-on, 1 signal loss; 2..255 reserved and ignored |
| 1 | Fade into the selected level, units of 100 ms |
| 2..3 | M, u16 |
| 4..13 | A[5], u16 each |
| 14..23 | B[5], u16 each |
| 24..26 | Reserved zero |

The receiver calculates the output levels using the light definition and power
limits. All-zero levels mean dark; an unset level defaults to dark. Power-on
fades up from dark, and signal loss fades from the current outputs. CONFIG 8
selects hold (0), off (1), fade off (2), or fade to the stored signal-loss level
(3). The controller's first SET supplies the transition out of the power-on
level. Both selectors share duplicate/SEQ tracking per (TARGET, type); each stored selector must keep a distinct SEQ, including in retries and refresh.

Stored levels contain no effects. Flash persistence occurs only when the
resulting levels or fade change, while outputs are dark; it must not blink the
outputs. Do not use SCENE for animation targets.
