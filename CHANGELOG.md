# Changelog

Protocol versions (`ULTRA_VERSION` in the header byte 2) and releases of this repository.

## v2.0.0 (protocol version 2), 2026

The first public version.

- **Frame:** N slots (32 bytes each, board 0 first), then the 8-byte header as a **trailer**.
- **TIME** is the host clock at the end of the frame (the falling edge of the header's last bit).
- **Start-up preamble:** 4 N + 2 frames of DARK slots (zero bits).
- **Slot types:**
  - 0 NOP, 1 LIGHT_DEF, 2 SET, 3 EFFECT, 4 STEP, 5 CONFIG, 6 IDENTIFY, 8 SCENE, 9 LIGHT_OPT;
  - 7 POLL is reserved.
- **Reserved:**
  - header FLAGS RW (return channel), HINT, EXT, UPD (firmware update);
  - ranges for slot types, lights, roles, effects, CONFIG ids, return-channel pages and feature bits;
  - broadcast / extension slots after the last board's slot.

## Protocol version 1 (withdrawn, never shipped)

The header came first.

- A board blocks its own slot while it forwards the rest, so the next board saw a ~320 µs LOW gap after the header.
- That gap ended the frame there, so boards after the first never got their slot.
- Version 2 moved the header to the end. Boards and controllers reject each other's versions.
