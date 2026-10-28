# SPDX-License-Identifier: MIT
"""Rules as code: the recorded design limits of the wire, the chain and updates.

Every limit below is a recorded design decision with its reason in docs/timing.md or
docs/update.md. Each lives in ONE named constant whose comment says "recorded design
limit". This guard holds the recorded numbers literally and fails, naming the rule,
when a constant, the code that enforces it or the documents disagree with them. A
failure here is never fixed by editing this file alone: changing a limit needs an
explicit decision first, then the constant, the documents and this file together.

  check_rules.py                  static checks (constants, documents, no shared limit)
  check_rules.py --probe LOG      also the compiled values and refusals (sender/test/rules_probe.c)
  check_rules.py --mutants        prove it: one mutant per rule must fail, naming that rule
  check_rules.py --list           the inventory: rule, constant, value, where enforced
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROTO = "include/ultra_protocol.h"
TRANSPORT = "include/ultra_transport.h"
UPDATE = "include/ultra_update.h"
SUPDATE = "sender/src/ultra_sender_update.h"
TIMING = "docs/timing.md"
UPDMD = "docs/update.md"
COPIES = ("ultra_protocol.h", "ultra_transport.h", "ultra_update.h")   # sender/src/ holds byte copies of include/
DECISION = "recorded design limit: explicit decision needed"

# rule, constant, recorded value, defining file, where enforced, [(document, phrase regex)]
RULES = [
    ("Diff-Power port: at most 8 receivers", "ULTRA_PORT_DIFF_POWER_MAX_BOARDS", 8, PROTO,
     "ultra_port_max_boards: update entry points + job_begin / job_recover, ultra_port_line_profile; ultra_line_path_ns",
     [(TIMING, r"a Diff-Power \(RS-485\) port's: at most eight receivers"),
      (TIMING, r"The GUARANTEE eight-board path uses 588 ns\. A ninth board or any uncertified segment fails"),
      (UPDMD, r"at most eight boards on a Diff-Power port")]),
    ("Dig2Analog-Plus-5ch pin: at most 50 boards", "ULTRA_PORT_D2A_PIN_MAX_BOARDS", 50, PROTO,
     "ultra_port_max_boards: update entry points + job_begin / job_recover, ultra_port_line_profile; ultra_d2a_path_ns",
     [(TIMING, r"single-wire chain of up to 50 boards"),
      (TIMING, r"35\.\.50 200 kHz; more than 50 is refused"),
      (UPDMD, r"at most 50 on a Dig2Analog-Plus-5ch data pin")]),
    ("D2A pin table: 1..20 boards at 800 kHz", "ULTRA_D2A_800_MAX_BOARDS", 20, SUPDATE,
     "ultra_port_line_profile, ultra_port_line_too_fast (every update entry point)",
     [(TIMING, r"1\.\.20 boards 800 kHz"), (TIMING, r"800 stops at 20, one hop below its model ceiling")]),
    ("D2A pin table: 21..34 boards at 600 kHz", "ULTRA_D2A_600_MAX_BOARDS", 34, SUPDATE,
     "ultra_port_line_profile, ultra_port_line_too_fast (every update entry point)",
     [(TIMING, r"21\.\.34 600 kHz")]),
    ("Diff-Power GUARANTEE per-hop bound 20 ns", "ULTRA_CHAIN_GUARANTEE_HOP_NS", 20, PROTO, "ultra_line_path_ns",
     [(TIMING, r"The GUARANTEE per-hop width-error bound is 20 ns")]),
    ("Diff-Power forwarding allowance 4 ns", "ULTRA_CHAIN_FORWARD_NS", 4, PROTO, "ultra_line_path_ns",
     [(TIMING, r"The forwarding allowance is 4 ns")]),
    ("Diff-Power hop + forwarding allowance 24 ns", "ULTRA_CHAIN_HOP_DISTORTION_NS", 24, PROTO,
     "test_vectors.c topology; HOP + FORWARD", [(TIMING, r"GUARANTEE gives `D\(N\)=74\*N-4`")]),
    ("Diff-Power cable / protection allowance 50 ns per 1-100 m segment", "ULTRA_CHAIN_CABLE_DISTORTION_NS", 50, PROTO,
     "host path budgets (cable_sum_ns)",
     [(TIMING, r"Each cable/protection allowance is <=50 ns"), (TIMING, r"Each qualified 1\.\.100 m segment has C_i <=50 ns")]),
    ("Cable delay 538 ns per 100 m", "ULTRA_CHAIN_CABLE_DELAY_NS", 538, PROTO, "return-window budget",
     [(TIMING, r"Cable delay is 538 ns/100 m")]),
    ("Diff-Power eight-board GUARANTEE path 588 ns", "ULTRA_CHAIN_USED_NS", 588, PROTO, "test_vectors.c topology",
     [(TIMING, r"The GUARANTEE eight-board path uses 588 ns")]),
    ("Qualified stage delay 300 ns", "ULTRA_HOP_DELAY_NS", 300, PROTO, "return-window budget",
     [(TIMING, r"qualified 300 ns ULTRA_HOP_DELAY_NS stage bound")]),
    ("Sender residual <= 10 ns", "ULTRA_LINE_TX_TOL_NS", 10, PROTO, "RX validation windows, backend admission",
     [(TIMING, r"Sender residual HIGH and period error is <=10 ns")]),
    ("Port gap >= 500 us", "ULTRA_FRAME_GAP_US", 500, PROTO, "job_begin port_gap_us floor; ultra_rw_next_us",
     [(TIMING, r"Sender reset/processing LOW >=500 us"),
      (TIMING, r"port_gap_us = max\(500, all affected LINK\.min_gap_us\)"),
      (UPDMD, r"port_gap_us = max\(500, all affected LINK\.min_gap_us\)")]),
    ("D2A hop bound +-9 ns", "ULTRA_D2A_HOP_NS", 9, TRANSPORT, "ultra_d2a_path_ns, ultra_d2a_admits",
     [(TIMING, r"one hop is bounded by \+/-9 ns"), (TIMING, r"`D\(N\) = 9\*\(N-1\) \+ E_N` ns")]),
    ("D2A gate reopening <= 100 ns", "ULTRA_D2A_GATE_REOPEN_NS", 100, TRANSPORT, "ultra_d2a_admits",
     [(TIMING, r"reopen its gate \(<=100 ns after its removed slot\)")]),
    ("D2A LOW residual 20 ns", "ULTRA_D2A_LOW_RESIDUAL_NS", 20, TRANSPORT, "ultra_d2a_admits",
     [(TIMING, r"`LOW_p - 20 - D\(N-1\) > 100` ns")]),
    ("Rate lock within +-200 ppm", "ULTRA_RATE_ERROR_PPM", 200, PROTO, "rate-lock checks (receivers, sender)",
     [(TIMING, r"rate within ±200 ppm"), (UPDMD, r"\+/-200 ppm / 1000 ms rules")]),
    ("Rate lock lost after 2 s without timing observations", "ULTRA_RATE_LOCK_LOSS_MS", 2000, PROTO,
     "ultra_sender_update.c NORMAL_TRAIN restart",
     [(TIMING, r"After missing valid timing observations for 2 s")]),
    ("No reply before >= 1000 ms of headers after the preamble", "ULTRA_RW_LOCK_MS", 1000, PROTO,
     "RW scheduling; ultra_sender_update.c NORMAL_TRAIN",
     [(TIMING, r"No receiver transmits before at least 1000 ms of valid headers after preamble")]),
    ("Return pitch (335 << rate) + 165 us: scaled part", "ULTRA_RW_PITCH_SCALED_US", 335, PROTO, "ultra_rw_slot_us",
     [(TIMING, r"\| Answer pitch \| `\(335 << rate\) \+ 165`: 500 / 835 / 1505 \|")]),
    ("Return pitch (335 << rate) + 165 us: fixed part", "ULTRA_RW_PITCH_FIXED_US", 165, PROTO, "ultra_rw_slot_us",
     [(TIMING, r"\| Answer pitch \| `\(335 << rate\) \+ 165`: 500 / 835 / 1505 \|")]),
    ("RW release: LOW >= 60 us", "ULTRA_RW_RELEASE_MIN_US", 60, PROTO, "sender RW release scheduling",
     [(TIMING, r"\| Host holds LOW and releases \| LOW ≥60; stopped driving by 150 \(target 80\) \|")]),
    ("RW release: stopped driving by 150 us", "ULTRA_RW_RELEASE_US", 150, PROTO, "sender RW release scheduling",
     [(TIMING, r"\| Host holds LOW and releases \| LOW ≥60; stopped driving by 150 \(target 80\) \|")]),
    ("Slot plan re-arm: after 200 us of quiet LOW", "ULTRA_RW_REARM_US", 200, PROTO, "every board's gate (apps, bootloaders)",
     [(TIMING, r"\| Slot plan re-arm \(every board\) \| after ≥200 quiet LOW \(`ULTRA_RW_REARM_US`\), never in a released HIGH \|"),
      (TIMING, r"It re-arms that plan only after 200 µs of LOW with no edge after a frame")]),
    ("Marker RW: a release after +250 us is no RW", "ULTRA_DP_RELEASE_MAX_US", 250, PROTO, "digital-port receivers",
     [(TIMING, r"a start after \+250 is rejected as RW"), (TIMING, r"the 250 us late-release limit")]),
    ("Back-off: 3 quick polls", "ULTRA_DP_TRIES", 3, PROTO, "ultra_dport.h back-off",
     [(TIMING, r"\| Retry/backoff \| 3 quick attempts, 3 more after ≥60 s, then user intervention \|")]),
    ("Back-off: 3 more after 60 s", "ULTRA_DP_RETRY_S", 60, PROTO, "ultra_dport.h back-off",
     [(TIMING, r"\| Retry/backoff \| 3 quick attempts, 3 more after ≥60 s, then user intervention \|")]),
    ("Blind update: >= 3 complete DATA passes per family", "ULTRA_UPDATE_BLIND_MIN_PASSES", 3, SUPDATE,
     "job_begin + update entry points (ULTRA_UPDATE_TOO_FEW_PASSES)",
     [(UPDMD, r"Every family requires at least three complete DATA passes")]),
    ("Host quiet >= 2100 ms after the last UPD frame", "ULTRA_UPDATE_QUIET_MS", 2100, SUPDATE,
     "ultra_sender_update.c reserve() after END / ABORT / idle PING / ACK; ABORT hold",
     [(TIMING, r"hold quiet LOW for >=2100 ms \(the 2000 ms idle close plus receiver tick granularity\)"),
      (UPDMD, r"hold quiet LOW for at least 2100 ms \(the 2000 ms transparent idle close plus tick margin\)")]),
    ("At most three acknowledgement copies", "ULTRA_UPDATE_ACK_COPIES", 3, SUPDATE, "ultra_sender_update.c ACK state",
     [(UPDMD, r"send at most three acknowledgement copies")]),
    ("Blind update: END sent three times", "ULTRA_UPDATE_END_COPIES", 3, SUPDATE, "ultra_sender_update.c END state",
     [(UPDMD, r"a blind host sends END three times")]),
    ("Idle confirmation PING sent six times", "ULTRA_UPDATE_CONFIRM_COPIES", 6, SUPDATE,
     "ultra_sender_update.c REBOOT state",
     [(UPDMD, r"idle application confirmation: six copies")]),
    ("Decision frames start >= 10 ms after the frame before", "ULTRA_UPDATE_CONTROL_PACE_US", 10000, SUPDATE,
     "ultra_sender_update.c last training PING, SELECT / RESELECT, ANNOUNCE half 0, END copies",
     [(TIMING, r"starts at least 10 ms plus the port processing gap after the end of the frame before it"),
      (UPDMD, r"starts at least 10 ms plus the port gap after the frame before it")]),
    ("Update training >= 1000 ms of UPD PINGs", "UPD_TRAIN_MIN_MS", 1000, UPDATE,
     "ultra_sender_update.c PREAMBLE / TRAIN",
     [(TIMING, r"Every group transfer starts with >=1000 ms of valid UPD PING headers"),
      (UPDMD, r"UPD PINGs for >=1000 ms before entry ANNOUNCE"),
      (UPDMD, r"PING training for >=1000 ms before resending both boot ANNOUNCE halves")]),
    ("Transparent idle close 2000 ms", "UPD_IDLE_CLOSE_MS", 2000, UPDATE,
     "receivers; ultra_sender_update.c training restart",
     [(TIMING, r"The transparent idle-close time is 2000 ms after the last valid UPD header")]),
    ("Forward gate opens after >= 350 us quiet LOW", "UPD_FORWARD_QUIET_US", 350, UPDATE,
     "receivers (every app and bootloader gate)",
     [(TIMING, r"opens only inside a quiet inter-frame LOW after at least 350 us of LOW with no edge")]),
    ("Transit hold: largest u8 erase bound 255 ms", "UPD_TRANSIT_ERASE_MS", 255, UPDATE,
     "upd_transit_hold_ms (forwarders), lead check in ultra_update_plan_metadata",
     [(TIMING, r"`transit_hold_ms = erase_units\*2\*255 \+ 100`")]),
    ("Erase lead / transit hold margin +100 ms", "UPD_DECLARED_MARGIN_MS", 100, UPDATE,
     "upd_transit_hold_ms, ultra_update_plan_metadata lead",
     [(TIMING, r"`transit_hold_ms = erase_units\*2\*255 \+ 100`"),
      (UPDMD, r"3400 ms erase time plus 100 ms margin")]),
    ("Transit hold maximum 114340 ms", "UPD_TRANSIT_HOLD_MAX_MS", 114340, UPDATE,
     "upd_transit_hold_ms, ABORT hold",
     [(TIMING, r"holds the largest valid value, 114340 ms \(full layout-1 slot, 224 units\)")]),
    ("Flash deadline = 2 x advertised maximum", "UPD_OPERATION_DEADLINE_FACTOR", 2, UPDATE,
     "ultra_update_plan_metadata (lead, pauses, finish), upd_transit_hold_ms",
     [(TIMING, r"operation deadline = twice the advertised maximum")]),
    ("Verification / dispatch allowance 100 ms", "UPD_DISPATCH_MS", 100, UPDATE, "ultra_update_plan_metadata finish",
     [(UPDMD, r"allow `max_i\(100 \+ 2\*program_max_ms_i\)` ms for verification/dispatch"),
      (UPDMD, r"leaves LOW for `100 \+ 2\*program_max_ms` ms")]),
    ("Inactivity timeout 10 s", "UPD_IDLE_TIMEOUT_MS", 10000, UPDATE, "ultra_sender_update.c progress deadline",
     [(UPDMD, r"A ten-second inactivity timeout")]),
    ("Entry bound >= 50 ms", "UPD_ENTRY_MIN_MS", 50, UPDATE, "job_begin, ultra_update_plan_metadata",
     [(TIMING, r"The immutable `entry_10ms` bound is at least 50 ms"),
      (TIMING, r"max\(50, max_i\(10\*entry_10ms_i\)\)")]),
]
# The four exact profiles: period / zero / one samples, V203 / V407 thresholds, certified admission ns
PROFILES = [(12, 3, 9, 22, 31, 220), (16, 4, 12, 30, 42, 300), (24, 6, 18, 45, 62, 450), (48, 12, 36, 90, 125, 750)]
PROFILE_RULE = "Four exact profiles and their path admission 220 / 300 / 450 / 750 ns"
PROFILE_DOC = [r"\| 0 / 800 kHz normal \| 12 / 3 / 9 \| 1250 / 312\.5 / 937\.5 \| 22 / 31 \| 220 \|",
               r"\| 1 / 600 kHz \| 16 / 4 / 12 \| 1666\.6667 / 416\.6667 / 1250 \| 30 / 42 \| 300 \|",
               r"\| 2 / 400 kHz \| 24 / 6 / 18 \| 2500 / 625 / 1875 \| 45 / 62 \| 450 \|",
               r"\| 3 / 200 kHz recovery \| 48 / 12 / 36 \| 5000 / 1250 / 3750 \| 90 / 125 \| 750 \|"]
NO_400 = "D2A pin table never picks 400 kHz"
NO_400_DOC = [(TIMING, r"400 kHz is not picked automatically")]
NO_SHARED = "No single board limit for all ports"
# board-limit names that may exist: the two port types, the pin table edges, storage capacities
BOARD_NAMES = {"ULTRA_PORT_DIFF_POWER_MAX_BOARDS", "ULTRA_PORT_D2A_PIN_MAX_BOARDS", "ULTRA_D2A_800_MAX_BOARDS",
               "ULTRA_D2A_600_MAX_BOARDS", "ULTRA_UPDATE_MAX_BOARDS", "ULTRA_SENDER_MAX_BOARDS"}
SCAN_DIRS = ("include", "sender/src", "sender/test", "sender/examples", "test-vectors", "examples", "docs")
SCAN_FILES = ("README.md", "sender/README.md", "board-ids.md")
TEXT = (".h", ".c", ".cpp", ".hpp", ".inc", ".py", ".md", ".json", ".txt", ".ino", ".yml", ".yaml", ".properties")


def load(root):
    """every scanned text file, repo-relative path -> text (LF)"""
    files = {}
    paths = [p for d in SCAN_DIRS if (root / d).is_dir() for p in sorted((root / d).rglob("*"))]
    paths += [root / f for f in SCAN_FILES if (root / f).is_file()]
    for p in paths:
        rel = p.relative_to(root).as_posix()
        if p.is_file() and p.suffix in TEXT and "__pycache__" not in rel and rel != "test-vectors/check_rules.py":
            files[rel] = p.read_bytes().decode("utf-8", "replace").replace("\r\n", "\n")
    return files


def rule_of(name):
    return next(r[0] for r in RULES if r[1] == name)


def norm(text):
    return re.sub(r"\s+", " ", text.replace("**", ""))


def defines(files, name):
    """(file, line index, value text) of every #define of name outside the sender's byte copies"""
    out = []
    for rel, text in files.items():
        if rel.startswith("sender/src/") and rel.split("/")[-1] in COPIES:
            continue
        for i, line in enumerate(text.split("\n")):
            m = re.match(r"\s*#\s*define\s+" + name + r"\s+(\S+)", line)
            if m:
                out.append((rel, i, m.group(1)))
    return out


def number(token):
    m = re.fullmatch(r"\(?(-?(?:0x[0-9A-Fa-f]+|\d+))[uUlL]*\)?", token)
    return int(m.group(1), 0) if m else None


def tagged(text, index):
    """the define line, or the run of comment / #define lines directly above it, says 'recorded design limit'"""
    lines = text.split("\n")
    i = index
    while i >= 0:
        s = lines[i].strip()
        if "ecorded design limit" in lines[i]:
            return True
        if i != index and not (s.startswith(("/*", "*", "#define", "#  define")) or s.endswith("*/")):
            return False
        i -= 1
    return False


def check_static(files, fail):
    for rule, name, value, where, _, docs in RULES:
        found = defines(files, name)
        if len(found) != 1:
            fail(rule, f"{name} must be defined exactly once (include/ or sender/src/), found "
                       f"{[(f, i + 1) for f, i, _ in found]}")
        for rel, i, token in found:
            got = number(token)
            if rel != where:
                fail(rule, f"{name} is defined in {rel}, recorded in {where}")
            if got != value:
                fail(rule, f"{name} is {token}, recorded {value}")
            if not tagged(files[rel], i):
                fail(rule, f"{name} ({rel}:{i + 1}): its comment must say 'recorded design limit'")
        for doc, phrase in docs:
            if not re.search(phrase, norm(files.get(doc, ""))):
                fail(rule, f"{doc} no longer states it (expected /{phrase}/)")
    # the sender's copies of the normative headers carry the same values
    for c in COPIES:
        if files.get("sender/src/" + c) != files.get("include/" + c):
            fail("Normative headers", f"sender/src/{c} differs from include/{c} (run_tests.py --sync-header)")
    # the profile table
    m = re.search(r"profiles\[4\]=\{(.*?)\};", files.get(TRANSPORT, ""), re.S)
    got = [tuple(int(x) for x in row.split(",")) for row in re.findall(r"\{([\d,\s]+)\}", m.group(1))] if m else None
    if got != PROFILES:
        fail(PROFILE_RULE, f"ultra_line_profile() table is {got}, recorded {PROFILES}")
    if m and not tagged(files[TRANSPORT], files[TRANSPORT][:m.start()].count("\n") - 1):
        fail(PROFILE_RULE, "ultra_line_profile(): its comment must say 'recorded design limit'")
    for phrase in PROFILE_DOC:
        if not re.search(phrase, norm(files.get(TIMING, ""))):
            fail(PROFILE_RULE, f"{TIMING} no longer states it (expected /{phrase}/)")
    for doc, phrase in NO_400_DOC:
        if not re.search(phrase, norm(files.get(doc, ""))):
            fail(NO_400, f"{doc} no longer states it (expected /{phrase}/)")
    code = files.get("sender/src/ultra_sender_update.c", "")
    body = re.search(r"uint8_t ultra_port_line_profile\(.*?\n\}", code, re.S)
    if not body or "ULTRA_PROFILE_400" in body.group(0) or re.search(r"\b2u\b", body.group(0)):
        fail(NO_400, "ultra_port_line_profile() must never return profile 2 (400 kHz)")
    # no single shared board limit
    for rel, text in files.items():
        if "ULTRA_CHAIN_MAX_BOARDS" in text:
            fail(NO_SHARED, f"{rel} uses ULTRA_CHAIN_MAX_BOARDS: one limit for all ports is gone for good")
        if rel.endswith((".h", ".c", ".hpp", ".cpp", ".inc")):
            for m in re.finditer(r"#\s*define\s+(\w*MAX_BOARDS\w*)", text):
                if m.group(1) not in BOARD_NAMES:
                    fail(NO_SHARED, f"{rel}: {m.group(1)} is a board limit outside the per-port-type names")
        if rel.startswith(("include/", "sender/src/")) and rel.endswith((".h", ".c", ".hpp")):
            for m in re.finditer(r"\bboards?\s*(?:>=?|<=?)\s*(?:8|9|50|51)u?\b", text):
                fail(NO_SHARED, f"{rel}: bare board-limit literal '{m.group(0)}', use the port type's name")
    u = defines(files, "ULTRA_UPDATE_MAX_BOARDS")
    if [t for _, _, t in u] != ["ULTRA_PORT_D2A_PIN_MAX_BOARDS"]:
        fail(NO_SHARED, "ULTRA_UPDATE_MAX_BOARDS (storage) must be ULTRA_PORT_D2A_PIN_MAX_BOARDS, the largest port type")
    q = re.search(r"uint16_t ultra_port_max_boards\(uint8_t port_type\)\s*\{(.*?)\}", code, re.S)
    if not q or not re.search(r"ULTRA_PORT_DIFF_POWER\s*\?\s*ULTRA_PORT_DIFF_POWER_MAX_BOARDS", q.group(1)) or \
            not re.search(r"ULTRA_PORT_D2A_PIN\s*\?\s*ULTRA_PORT_D2A_PIN_MAX_BOARDS", q.group(1)):
        fail(NO_SHARED, "ultra_port_max_boards() must map each port type to its own limit")


def parse_probe(text):
    p = {"const": {}, "max": {}, "profile": {}, "job": {}, "job_line": {}, "job_nodes": {}, "recover": {}, "path": {}, "admits": {},
         "line_profile": {}, "pitch": {}, "rc": {}, "why": {}, "type": {}}
    for line in text.split("\n"):
        f = line.split()
        if not f or f[0] not in p:
            continue
        k = f[0]
        if k == "const":
            p[k][f[1]] = int(f[2])
        elif k in ("rc", "why", "type"):
            p[k][f[1]] = int(f[2])
        elif k == "max":
            p[k][int(f[1])] = int(f[2])
        elif k == "profile":
            p[k][(int(f[1]), int(f[2]))] = (int(f[3]), int(f[4]))
        elif k in ("job", "job_line", "job_nodes", "recover"):
            p[k][(int(f[1]), int(f[2]))] = int(f[3])
        elif k == "path":
            p[k][(f[1], int(f[2]))] = int(f[3])
        elif k == "admits":
            p[k][(int(f[1]), int(f[2]))] = int(f[3])
        elif k == "line_profile":
            p[k][int(f[1])] = tuple(int(x) for x in f[2:])
        elif k == "pitch":
            p[k][int(f[1])] = int(f[2])
    p["complete"] = "\nend" in "\n" + text
    return p


def check_probe(text, fail):
    p = parse_probe(text)
    if not p["complete"]:
        fail("Probe", "rules_probe output incomplete")
        return
    for rule, name, value, *_ in RULES:
        if p["const"].get(name) != value:
            fail(rule, f"{name} compiles to {p['const'].get(name)}, recorded {value}")
    if p["const"].get("ULTRA_SENDER_MAX_BOARDS", 0) < 52:
        fail("Probe", "rules_probe needs ULTRA_SENDER_MAX_BOARDS >= 52")
    dp, d2a = p["type"].get("diff_power"), p["type"].get("d2a_pin")
    many, why = p["rc"].get("too_many"), p["why"]
    dp_rule, d2a_rule = rule_of("ULTRA_PORT_DIFF_POWER_MAX_BOARDS"), rule_of("ULTRA_PORT_D2A_PIN_MAX_BOARDS")
    for t, limit, rule, path_kind, path_at_limit in ((dp, 8, dp_rule, "diff_power", 8 * 20 + 7 * 4),
                                                     (d2a, 50, d2a_rule, "d2a_pin", 49 * 9)):
        if p["max"].get(t) != limit:
            fail(rule, f"ultra_port_max_boards({t}) is {p['max'].get(t)}, recorded {limit}")
        if p["profile"].get((t, limit + 1)) != (why["too_many"], 255):
            fail(rule, f"ultra_port_line_profile() must refuse {limit + 1} boards, gives {p['profile'].get((t, limit + 1))}")
        if p["profile"].get((t, limit), (None, 255))[1] == 255:
            fail(rule, f"ultra_port_line_profile() must admit {limit} boards")
        for k in ("job", "job_line", "job_nodes", "recover"):
            fn = "ultra_update_job_recover()" if k == "recover" else "ultra_update_job_begin()"
            if p[k].get((t, limit + 1)) != many:
                fail(rule, f"{fn} ({k}) must refuse {limit + 1} boards with TOO_MANY_BOARDS, "
                           f"gives {p[k].get((t, limit + 1))}")
            if p[k].get((t, limit)) == many:
                fail(rule, f"{fn} ({k}) refuses {limit} boards as too many")
        if p["path"].get((path_kind, limit)) != path_at_limit or p["path"].get((path_kind, limit + 1)) != 65535:
            fail(rule, f"{path_kind} path budget: {limit} boards {p['path'].get((path_kind, limit))} "
                       f"(recorded {path_at_limit}), {limit + 1} must fail closed")
    for unknown in (0, 3):
        if p["max"].get(unknown) != 0 or p["profile"].get((unknown, 1)) != (why["unknown_type"], 255):
            fail(NO_SHARED, f"port type {unknown} is unknown: no board limit, profile refused")
    for n in range(1, 51):
        want = (why["default"], 0) if n <= 20 else (why["board_count"], 1 if n <= 34 else 3)
        if p["profile"].get((d2a, n)) != want:
            rule = rule_of("ULTRA_D2A_800_MAX_BOARDS") if n <= 21 else rule_of("ULTRA_D2A_600_MAX_BOARDS") if n <= 35 else d2a_rule
            fail(rule, f"D2A pin with {n} boards runs {p['profile'].get((d2a, n))}, recorded {want} "
                       f"(1..20 800, 21..34 600, 35..50 200 kHz)")
    for n in range(1, 9):
        if p["profile"].get((dp, n)) != (why["default"], 0):
            fail(dp_rule, f"a Diff-Power port with {n} boards must start at 800 kHz, gives {p['profile'].get((dp, n))}")
    if any(v[1] == 2 for v in p["profile"].values()):
        fail(NO_400, "ultra_port_line_profile() picked 400 kHz")
    for prof, ceiling in ((0, 21), (1, 34), (2, 50), (3, 50)):
        if p["admits"].get((prof, ceiling)) != 1 or p["admits"].get((prof, ceiling + 1)) != 0:
            fail(rule_of("ULTRA_D2A_HOP_NS"), f"D2A link model at ideal wiring: profile {prof} must admit {ceiling} boards, not "
                               f"{ceiling + 1} (timing.md model ceilings 21 / 34 / 50 / 50)")
    if [p["line_profile"].get(i) for i in range(4)] != PROFILES:
        fail(PROFILE_RULE, f"compiled profile table {[p['line_profile'].get(i) for i in range(4)]}")
    if [p["pitch"].get(i) for i in range(3)] != [500, 835, 1505]:
        fail(rule_of("ULTRA_RW_PITCH_SCALED_US"), f"return pitch {[p['pitch'].get(i) for i in range(3)]}, recorded 500 / 835 / 1505 us")


def run(files, probe=None):
    problems = []

    def fail(rule, detail):
        problems.append(f"RULE '{rule}': {detail} - {DECISION}")
    check_static(files, fail)
    if probe is not None:
        check_probe(probe, fail)
    return problems


def mutants(files, probe):
    """one mutant per rule; each must fail naming its rule"""
    survived, count = [], 0

    def expect(label, rule, fs, pr=probe):
        nonlocal count
        count += 1
        if not any(f"RULE '{rule}'" in x for x in run(fs, pr)):
            survived.append(label)

    for rule, name, value, where, _, docs in RULES:
        fs = dict(files)
        fs[where] = re.sub(r"(#\s*define\s+" + name + r"\s+)\S+", lambda m: m.group(1) + f"{value + 1}u", fs[where], count=1)
        expect(f"{name} {value} -> {value + 1}", rule, fs)
        for doc, phrase in docs:
            fs = dict(files)
            text = norm(fs[doc])
            m = re.search(phrase, text)
            if m:
                # the document's own figure for this rule (digits or its word), else the first figure
                said = [str(value)] + {8: ["eight"], 3: ["three"], 6: ["six"], 10000: ["ten"], 2: ["twice"], 2000: ["2"],
                                       24: ["74"]}.get(value, [])
                hit = next((t for t in said if re.search(rf"(?<![\w.]){t}(?![\w])", m.group(0))), r"\d+")
                bad = re.sub(rf"(?<![\w.]){hit}(?![\w])" if hit != r"\d+" else hit, "X", m.group(0), count=1)
                fs[doc] = text[:m.start()] + bad + text[m.end():]
            expect(f"{doc}: {phrase[:40]}", rule, fs)
        if probe is not None:
            expect(f"compiled {name}", rule, files, re.sub(r"(?m)^const " + name + r" \d+$",
                                                           f"const {name} {value + 1}", probe))
    fs = dict(files)
    fs[TRANSPORT] = fs[TRANSPORT].replace("{12,3,9,22,31,220}", "{12,3,9,22,31,221}", 1)
    expect("profile table admission 220 -> 221", PROFILE_RULE, fs)
    fs = dict(files)
    fs[PROTO] = fs[PROTO].replace("#define ULTRA_PORT_D2A_PIN_MAX_BOARDS 50u\n",
                                  "#define ULTRA_PORT_D2A_PIN_MAX_BOARDS 50u\n#define ULTRA_CHAIN_MAX_BOARDS 8u\n", 1)
    expect("ULTRA_CHAIN_MAX_BOARDS back", NO_SHARED, fs)
    fs = dict(files)
    fs[PROTO] = fs[PROTO].replace("#define ULTRA_PORT_D2A_PIN_MAX_BOARDS 50u\n",
                                  "#define ULTRA_PORT_D2A_PIN_MAX_BOARDS 50u\n#define ULTRA_MAX_BOARDS 50u\n", 1)
    expect("a new shared ULTRA_MAX_BOARDS", NO_SHARED, fs)
    fs = dict(files)
    fs[TRANSPORT] = fs[TRANSPORT].replace("boards>ULTRA_PORT_DIFF_POWER_MAX_BOARDS", "boards>8u", 1)
    expect("bare 'boards>8u' in a path helper", NO_SHARED, fs)
    fs = dict(files)
    c = "sender/src/ultra_sender_update.c"
    fs[c] = fs[c].replace("port_type==ULTRA_PORT_D2A_PIN?ULTRA_PORT_D2A_PIN_MAX_BOARDS",
                          "port_type==ULTRA_PORT_D2A_PIN?ULTRA_PORT_DIFF_POWER_MAX_BOARDS", 1)
    expect("one limit for both port types in ultra_port_max_boards", NO_SHARED, fs)
    fs = dict(files)
    fs[c] = fs[c].replace("?ULTRA_PROFILE_600:ULTRA_PROFILE_200", "?ULTRA_PROFILE_600:ULTRA_PROFILE_400", 1)
    expect("pin table picks 400", NO_400, fs)
    if probe is not None:
        dp, d2a = re.search(r"(?m)^type diff_power (\d+)$", probe).group(1), re.search(r"(?m)^type d2a_pin (\d+)$", probe).group(1)
        for t, n, rule in ((dp, 9, rule_of("ULTRA_PORT_DIFF_POWER_MAX_BOARDS")), (d2a, 51, rule_of("ULTRA_PORT_D2A_PIN_MAX_BOARDS"))):
            for k in ("job", "job_line", "job_nodes", "recover"):
                expect(f"{k} admits {n} boards", rule, files,
                       re.sub(rf"(?m)^{k} {t} {n} -?\d+$", f"{k} {t} {n} -1", probe))
            expect(f"line profile admits {n} boards", rule, files,
                   re.sub(rf"(?m)^profile {t} {n} \d+ \d+$", f"profile {t} {n} 0 0", probe))
        expect("probe: pin table 400 kHz at 40 boards", NO_400, files,
               re.sub(rf"(?m)^profile {d2a} 40 (\d+) \d+$", rf"profile {d2a} 40 \1 2", probe))
        expect("probe: 21 boards at 800", rule_of("ULTRA_D2A_800_MAX_BOARDS"), files,
               re.sub(rf"(?m)^profile {d2a} 21 \d+ \d+$", f"profile {d2a} 21 0 0", probe))
        expect("probe: 35 boards at 600", rule_of("ULTRA_D2A_600_MAX_BOARDS"), files,
               re.sub(rf"(?m)^profile {d2a} 35 (\d+) \d+$", rf"profile {d2a} 35 \1 1", probe))
    return count, survived


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--probe", help="output of sender/test/rules_probe.c")
    ap.add_argument("--mutants", action="store_true")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    files = load(Path(a.root))
    probe = Path(a.probe).read_text(encoding="utf-8") if a.probe else None
    if a.list:
        for rule, name, value, where, enforced, docs in RULES:
            print(f"| {rule} | `{name}` | {value} | {where}; {enforced} | {', '.join(sorted({d for d, _ in docs}))} |")
        return 0
    problems = run(files, probe)
    for p in problems:
        print("FAIL " + p)
    if problems:
        return 1
    if a.mutants:
        count, survived = mutants(files, probe)
        for s in survived:
            print("FAIL mutant survived: " + s)
        if survived:
            return 1
        print(f"PASS: {count} mutants, each fails naming its rule")
    print(f"PASS: {len(RULES) + 3} recorded rules hold (constants, documents"
          + (", compiled values and refusals" if probe is not None else "") + "); no shared board limit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
