#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent port schedule and byte oracle; no sender implementation imports."""
from pathlib import Path
import importlib.util
import struct
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('update_trace_codec',
    Path(__file__).resolve().parents[2] / 'test-vectors' / 'codec.py')
codec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codec)
TIME_WRAP = 4194304000   # header TIME = host us modulo 2^22 x 1000 (ULTRA_TIME_WRAP)
PACE = 10000             # a decision frame (ANNOUNCE half, END copy, the frame after a train) starts >= 10 ms + the port gap later


def expected(family, profile, marker):
    body = bytes((i * 37) & 255 for i in range(516))
    load = 0x08005000 if family else 0x08002100
    descriptor = codec.encode('image', dict(rev_min=1, rev_max=255, board=3,
        mcu=4 if family else 1, package=5 if family else 1, layout=4 if family else 1,
        abi=3, features=3 if family else 0, variant=201, min_bl=3,
        firmware=0x000700ff, length=len(body), body_crc=codec.crc32(body),
        load=load, entry=load, record_format=2, wire=3, descriptor_format=1,
        build_id='00' * 16))
    period, zero, one = ((12, 3, 9), (16, 4, 12), (24, 6, 18), (48, 12, 36))[profile]
    at = TIME_WRAP - 67296   # the schedule runs across TIME's wrap
    records = bytearray()
    count = 0

    def emit(op=None, index=0, pass_byte=0, data=bytes(32), session=7, dark=False):
        nonlocal at, records, count
        start = at + (530 if marker else 0)
        length = 40 if op is None else 48
        prefix = (length * 8 - 1) * period

        def build(stamp):
            if op is None:
                return (bytes(32) if dark else codec.seal(bytes(30))) + codec.build_header(3, 0, stamp)
            return codec.encode('update', dict(opcode=op, session=session, index=index,
                **{'pass': pass_byte}, flags=0, data=data.hex(), time=stamp))

        # Integer TIME closest to the exact physical edge; stable earliest tie.
        candidates = range((prefix + zero) * 1000000 // 9600000,
                           ((prefix + one) * 1000000 + 9599999) // 9600000 + 1)
        def error(k):
            frame = build((start + k) % TIME_WRAP)
            ticks = prefix + (one if frame[-1] & 1 else zero)
            return abs(ticks * 1000000 - k * 9600000)
        stamp = min(candidates, key=error)
        frame = build((start + stamp) % TIME_WRAP)
        ticks = prefix + (one if frame[-1] & 1 else zero)
        end = start + (ticks * 1000000 + 9599999) // 9600000
        records.extend(struct.pack('>Q I Q Q 6I', at, len(frame), start, end,
            500 if marker else 0, 20 if marker else 0, 10 if marker else 0, 500, 0, 0))
        records.extend(frame)
        count += 1
        at = end + 500
        return end

    def train(op=0x85, pace=PACE):
        nonlocal at
        first = emit(op)
        at_least = first + 1000000
        while True:
            at += 10000
            if emit(op) >= at_least:
                break
        at += pace   # the last PING too: the ANNOUNCE half after it is a decision frame

    def pair():
        nonlocal at
        emit(0x81, data=descriptor[:32])
        at += PACE
        return emit(0x81, index=1, data=descriptor[32:])

    train()
    at = pair() + 100000 + 500  # entry bound + port gap
    train()
    at = pair() + (500000 if family else 260000)
    for pass_number in range(3):
        for index in range((len(body) + 31) // 32):
            end = emit(0x82, index=index, pass_byte=pass_number * 16 + 3,
                       data=body[32 * index:32 * index + 32].ljust(32, b'\0'))
            if index % 8 == 7 or index == 16:
                at = end + (61000 if family else 21000)
    # END has no feedback: three copies, each followed by the port gap (emit's 500 us) and the pace; the quiet LOW counts
    # from the last
    for _ in range(2):
        emit(0x83, data=struct.pack('>II', codec.crc32(body), len(body)) + bytes(24))
        at += PACE
    at = emit(0x83, data=struct.pack('>II', codec.crc32(body), len(body)) + bytes(24)) + 2100000
    for _ in range(6):
        emit(dark=True)
    train(op=None, pace=0)   # ordinary frames: the rate lock train, no decision frame after it
    # the idle confirmation PING: six copies, each with its confirmation-write pause (100 + 2 x program ms, + the port gap)
    for _ in range(6):
        at = emit(0x85, session=0) + (160000 if family else 120000) + 500
    return bytes(records), count


def main(directory):
    frames = 0
    for family in range(2):
        for profile in range(4):
            for marker in range(2):
                want, count = expected(family, profile, marker)
                path = Path(directory) / f'update-{family}-{profile}-{marker}.bin'
                got = path.read_bytes()
                if got != want:
                    first = next((i for i, pair in enumerate(zip(got, want)) if pair[0] != pair[1]), min(len(got), len(want)))
                    raise AssertionError(f'{path.name}: byte {first}, got {got[first:first+16].hex()}, expected {want[first:first+16].hex()}')
                frames += count
    print(f'PASS 16 byte-exact update schedules, {frames} frames and envelopes (V203/V407, 4 profiles, CHAIN/marker, the TIME wrap at 2^22 x 1000 us, partial blocks)')


if __name__ == '__main__':
    main(sys.argv[1])
