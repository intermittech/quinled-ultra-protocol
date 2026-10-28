#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 QuinLED
"""Build and run the allocation-free wire-3 sender reference.

python sender/test/run_tests.py [--firmware DIR] [--only STEPS] [--skip-cross]

The default runs strict host/cross C99 and C++ builds, shared Python/C/C++
fixtures, exact-clock TIME sweeps, scheduler, AUTO, update, PARLIO and mocked
RMT ownership tests, examples, manifests and sanitizer suites. Artifacts go to
../tmp/sender-build or ULTRA_TEST_BUILD; child processes have bounded runtimes.

The receiver simulators (test_sim, test_sim_rx, dline_sim) are part of the
default set: they need the receiver firmware (--firmware / ULTRA_FIRMWARE, else
a quinled-ultra-firmware checkout next to this repository) of the same protocol
commit. A missing or byte-different firmware header fails; another wire version
is never substituted. --sync-header copies all three normative headers into src/ and
updates PROTOCOL-PIN.json; package versions must match the protocol release.
"""

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

HERE = Path(__file__).resolve().parent
SENDER = HERE.parent
REPO = SENDER.parent
PROTO_INC = REPO / "include"
VECTORS = REPO / "test-vectors" / "vectors.json"
BUILD = Path(os.environ.get("ULTRA_TEST_BUILD", str(REPO.parent / "tmp" / "sender-build"))).resolve()
SRC = SENDER / "src"
LIB = sorted(SRC.glob("*.c"))
INC = [SRC]                                      # every header, the protocol header's copy too
FW_FILES = ("frame_rx.c", "proto.c", "ultra.c", "ultra_tables.c", "math32.c", "config.c", "presets.c")
# the receiver build (test_sim_rx) also: its lifetime record and LIFE_SYNC merge (energy.c) and their dependencies
RX_FILES = ("energy.c", "rc.c", "scene.c", "isense.c", "pagepair.c", "store.c")

_subprocess_run = subprocess.run
def bounded_run(*args, **kwargs):
    kwargs.setdefault("timeout", 180)
    return _subprocess_run(*args, **kwargs)
subprocess.run = bounded_run

results = []


def note(ok, what):
    results.append((ok, what))
    print(("PASS " if ok else "FAIL ") + what, flush=True)


# ------------------------------------------------------------------------------------------------ compilers
def find_msvc():
    if sys.platform != "win32":
        return None
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                         capture_output=True, text=True).stdout.strip()
    bat = Path(out) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat" if out else None
    return bat if bat and bat.exists() else None


def run_msvc(vcvars, args, cwd):
    """cl with args (a list) inside the MSVC environment"""
    BUILD.mkdir(parents=True, exist_ok=True)
    script = BUILD / "msvc_cmd.bat"
    script.write_text('@echo off\ncall "%s" >nul 2>nul\n%s\n' % (vcvars, subprocess.list2cmdline(["cl"] + args)))
    return subprocess.run(["cmd", "/c", str(script)], cwd=cwd, capture_output=True, text=True)


def which_any(names, extra_globs=()):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    for g in extra_globs:
        hits = sorted(glob.glob(os.path.expandvars(os.path.expanduser(g))))
        if hits:
            return hits[-1]
    return None


def cross_compilers():
    """(label, path, flags) of the cross compilers found: warnings and size only"""
    local = os.environ.get("LOCALAPPDATA", "")
    found = []
    xt = which_any(["xtensa-esp32-elf-gcc"], [
        "C:/Espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc.exe",
        "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc",
        "~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-gcc*"])
    if xt:
        found.append(("ESP32 (Xtensa) gcc", xt, ["-mlongcalls"]))
    rv = which_any(["riscv-none-elf-gcc", "riscv32-esp-elf-gcc"], [
        local + "/xPacks/*/bin/riscv-none-elf-gcc.exe",
        "~/.local/xPacks/*/bin/riscv-none-elf-gcc",
        "C:/Espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc.exe"])
    if rv:
        found.append(("RV32IMC gcc", rv, ["-march=rv32imc_zicsr", "-mabi=ilp32"]))
    arm = which_any(["arm-none-eabi-gcc"])
    if arm:
        found.append(("Cortex-M4 (Thumb-2) gcc", arm, ["-mcpu=cortex-m4", "-mthumb"]))
    cl = which_any(["clang"], ["C:/Espressif/tools/esp-clang/*/esp-clang/bin/clang.exe",
                               "~/.espressif/tools/esp-clang/*/esp-clang/bin/clang"])
    if cl:
        target = []
        if "esp-clang" in cl.replace("\\", "/"):
            target = ["--target=riscv32-esp-elf", "-march=rv32imc", "-ffreestanding"]
        found.append(("clang", cl, target))
    return found


def host_cc():
    cc = os.environ.get("CC")
    if cc:
        return ("cc", cc)
    vc = find_msvc()
    if vc:
        return ("msvc", vc)
    for n in ("cc", "gcc", "clang"):
        p = shutil.which(n)
        if p:
            return ("cc", p)
    return (None, None)


# ------------------------------------------------------------------------------------------------ steps
def warnings_check(kind, cc):
    BUILD.mkdir(parents=True, exist_ok=True)
    incs = [f"-I{p}" for p in INC]
    if kind == "msvc":
        for src in LIB:
            r = run_msvc(cc, ["/nologo", "/c", "/W4", "/WX", "/O2", "/I", str(SRC), "/Fo" + str(BUILD) + "\\",
                              str(src)], BUILD)
            note(r.returncode == 0, f"MSVC /W4 /WX: {src.name}" + ("" if r.returncode == 0 else "\n" + r.stdout))
        # the C++ wrapper (its run: the cpp step)
        r = run_msvc(cc, ["/nologo", "/c", "/W4", "/WX", "/EHsc", "/std:c++17", "/DULTRA_CPP_COMPILE_ONLY", "/I",
                          str(SRC), "/Fo" + str(BUILD) + "\\", str(HERE / "test_cpp.cpp")], BUILD)
        note(r.returncode == 0, "MSVC /W4 /WX: ultra_sender.hpp (C++17)" + ("" if r.returncode == 0 else "\n" + r.stdout))
        return
    for src in LIB:
        r = subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2", *incs, "-c", str(src),
                            "-o", str(BUILD / (src.stem + ".host.o"))], capture_output=True, text=True)
        note(r.returncode == 0, f"{Path(cc).name} -Wall -Wextra -Werror: {src.name}" + ("" if r.returncode == 0 else "\n" + r.stderr))


def cross_check(label, cc, flags):
    """-Wall -Wextra -Werror and the .text size at -Os"""
    BUILD.mkdir(parents=True, exist_ok=True)
    incs = [f"-I{p}" for p in INC]
    size = {}
    ok = True
    for src in LIB:
        obj = BUILD / (src.stem + "." + "".join(c for c in label if c.isalnum()) + ".o")
        r = subprocess.run([cc, *flags, "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Os",
                            "-ffunction-sections", *incs, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        if r.returncode != 0:
            ok = False
            print(r.stderr)
            continue
        size[src.stem] = text_size(cc, obj)
    note(ok, f"{label} -Wall -Wextra -Wpedantic -Werror: the library")
    name = Path(cc).name
    cxx = str(Path(cc).with_name(name.replace("gcc", "g++") if "gcc" in name else name.replace("clang", "clang++")))
    if Path(cxx).exists() or Path(cxx + ".exe").exists() or shutil.which(cxx):
        r = subprocess.run([cxx, *flags, "-std=c++11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Os",
                            "-DULTRA_CPP_COMPILE_ONLY", *incs, "-c", str(HERE / "test_cpp.cpp"), "-o",
                            str(BUILD / "test_cpp.cross.o")], capture_output=True, text=True)
        note(r.returncode == 0, f"{label} (C++11): ultra_sender.hpp" + ("" if r.returncode == 0 else "\n" + r.stderr))
    if ok and all(v is not None for v in size.values()):
        core = size["ultra_sender"] + size["ultra_sender_bits"]
        print(f"     size -Os (.text + .rodata): core {core} B (scheduler {size['ultra_sender']}, encoders "
              f"{size['ultra_sender_bits']}), return channel +{size['ultra_sender_rc']} B")
    if ok and all(v is not None for v in size.values()):
        print("     all modules -Os (.text + .rodata): " + str(sum(size.values())) + " B; " + ", ".join(k + "=" + str(v) for k,v in size.items()))
    return size


def text_size(cc, obj):
    """.text* + .rodata* of an object, via the toolchain's size / objdump"""
    d = Path(cc).parent
    base = Path(cc).name
    for tool in (base.replace("gcc", "size").replace("clang", "llvm-size"), "llvm-size", "size"):
        cand = d / tool
        exe = str(cand) if cand.exists() or (cand.with_suffix(".exe")).exists() else shutil.which(tool)
        if not exe:
            continue
        r = subprocess.run([exe, "-A", str(obj)], capture_output=True, text=True)
        if r.returncode != 0:
            continue
        total = 0
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and (parts[0].startswith(".text") or parts[0].startswith(".rodata") or
                                    parts[0].startswith(".literal") or parts[0].startswith(".srodata")):
                try:
                    total += int(parts[1])
                except ValueError:
                    pass
        return total
    return None


def build_run(kind, cc, name, sources, incs, args=(), defines=(), fw_objs=(), cpp=False, asan=False, loose=False):
    """build sources (C, or C++ with cpp: C++17; the C ones of a C++ build as C) into one executable and run it
    (loose: the firmware's own test file is among them, built with its warning level)"""
    BUILD.mkdir(parents=True, exist_ok=True)
    tag = name + ("_asan" if asan else "")
    exe = BUILD / (tag + (".exe" if sys.platform == "win32" else ""))
    objdir = BUILD / ("obj_" + tag)
    objdir.mkdir(exist_ok=True)
    if kind == "msvc":
        a = ["/nologo", "/O2", "/W3", "/wd4996"] if loose else ["/nologo", "/O2", "/W4", "/WX", "/wd4996"]
        if cpp:
            a += ["/EHsc", "/std:c++17"]
        if asan:
            a += ["/fsanitize=address", "/Zi"]
        for d in defines:
            a.append("/D" + d)
        for i in incs:
            a += ["/I", str(i)]
        a += ["/Fe:" + str(exe), "/Fo" + str(objdir) + "\\", "/Fd" + str(objdir) + "\\"]
        a += [str(s) for s in sources] + [str(o) for o in fw_objs]
        r = run_msvc(cc, a, BUILD)
        out = r.stdout + r.stderr
    else:
        san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"] if asan else []
        objs, out, r = [], "", None
        for src in sources:
            src = Path(src)
            is_cpp = src.suffix == ".cpp"
            comp = os.environ.get("CXX", cc.replace("gcc", "g++").replace("clang", "clang++")) if is_cpp else cc
            obj = objdir / (src.stem + ".o")
            r = subprocess.run([comp, "-std=c++17" if is_cpp else "-std=c99", "-O2", *(["-w"] if loose else ["-Wall", "-Wextra", "-Werror"]),
                                *san, *["-D" + d for d in defines], *[f"-I{i}" for i in incs], "-c", str(src), "-o",
                                str(obj)], capture_output=True, text=True)
            out += r.stdout + r.stderr
            if r.returncode != 0:
                break
            objs.append(str(obj))
        if r is not None and r.returncode == 0:
            link = os.environ.get("CXX", cc.replace("gcc", "g++").replace("clang", "clang++")) if cpp else cc
            r = subprocess.run([link, *san, *objs, *[str(o) for o in fw_objs], "-o", str(exe), "-lm"],
                               capture_output=True, text=True)
            out += r.stdout + r.stderr
    if r.returncode != 0:
        note(False, f"{tag}: build\n{out}")
        return False
    if asan and kind == "msvc":
        # MSVC's ASan runtime is a DLL of the compiler: run inside its environment
        script = BUILD / ("run_" + tag + ".bat")
        script.write_text('@echo off\ncall "%s" >nul 2>nul\n%s\n' % (cc, subprocess.list2cmdline([str(exe), *args])))
        r = subprocess.run(["cmd", "/c", str(script)], capture_output=True, text=True, cwd=str(BUILD))
    else:
        r = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=str(BUILD))
    (BUILD / (tag + ".log")).write_text(r.stdout + r.stderr)
    tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-40:])
    print(tail)
    note(r.returncode == 0, f"{tag}" + ("" if r.returncode == 0 else f" (exit {r.returncode})"))
    return r.returncode == 0


def dline_sim(kind, cc, core):
    """a digital port (0.2.8) against the Diff-Power-UR-Digital-M receiver's own delay line: the firmware's test/test_dline.c (a
    sample-exact model of the receiver's hardware, with core/dline.c on it and its checks on the strip) with its model
    hub replaced by the reference hub of test/dport_driver.h (the sender library, ultra_dport.h's planner, the RMT words
    as the ESP32 would send them). The firmware's file is patched as text (its hub function, its main) and not modified."""
    src = core.parent.parent / "test" / "test_dline.c"
    if not (src.exists() and (core / "dline.c").exists() and (core / "ws_decode.c").exists()):
        note(False, f"dline sim: the firmware has no test/test_dline.c + core/dline.c ({core})")
        return
    text = src.read_text(encoding="utf-8")
    NL = chr(10)
    edits = (('#include "../src/core/', '#include "'),
             ('#include "ws_decode.h"' + NL, '#include "ws_decode.h"' + NL + '#include "dport_driver.h"' + NL),
             ("int main(int argc, char **argv)", "static int fw_main(int argc, char **argv)"),
             ("static void run(const scen_t *sc)" + NL,
              '#include "sim_dline_hook.inc"' + NL + NL + "static void run(const scen_t *sc)" + NL))
    for old, new in edits:
        if old not in text:
            note(False, f"dline sim: the firmware's test_dline.c has changed ({old!r} not found): update run_tests.py")
            return
        text = text.replace(old, new)
    text += NL + '#include "sim_dline_main.inc"' + NL
    BUILD.mkdir(parents=True, exist_ok=True)
    out = BUILD / "dline_sim.c"
    out.write_text(text, encoding="utf-8", newline=NL)
    build_run(kind, cc, "dline_sim", [out, core / "dline.c", core / "ws_decode.c", *LIB], [HERE, core] + INC, loose=True)


def firmware_dir(arg):
    """an absolute path (the compilers run in the build folder). --firmware / ULTRA_FIRMWARE as given; otherwise a
    quinled-ultra-firmware checkout next to this repository (its header must be the package's, or the header check
    fails)"""
    def core_of(c):
        core = c / "src" / "core" if (c / "src" / "core").exists() else c
        return core.resolve() if all((core / f).exists() for f in FW_FILES) else None
    for given in (arg, os.environ.get("ULTRA_FIRMWARE")):
        if given:
            return core_of(Path(given).expanduser().resolve())
    root = REPO.parent
    found = [c for c in map(core_of, [root / "quinled-ultra-firmware"]) if c is not None]
    same = [c for c in found if (c / "ultra_protocol.h").read_bytes() == (SRC / "ultra_protocol.h").read_bytes()]
    return (same or found or [None])[0]


def build_firmware_objs(kind, cc, core, defines=(), sub="fw", files=FW_FILES):
    """the board firmware's receive path and engine, compiled as the firmware's host tests do (defines: e.g. BOARD=2,
    the Diff-Power ultra receiver's build)"""
    objs = []
    fwb = BUILD / sub
    fwb.mkdir(parents=True, exist_ok=True)
    for f in files:
        if kind == "msvc":
            obj = fwb / (Path(f).stem + ".obj")
            r = run_msvc(cc, ["/nologo", "/c", "/O2", "/W3", "/wd4996", *["/D" + d for d in defines], "/I", str(core),
                              "/Fo" + str(obj), str(core / f)], BUILD)
        else:
            obj = fwb / (Path(f).stem + ".o")
            r = subprocess.run([cc, "-std=c99", "-O2", "-w", *["-D" + d for d in defines], f"-I{core}", "-c",
                                str(core / f), "-o", str(obj)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, getattr(r, "stderr", ""))
            return None
        objs.append(obj)
    return objs


# ------------------------------------------------------------------------------------------------ the package
def header_release(path):
    m = re.search(r"this file: release (\d+\.\d+\.\d+(?:-draft)?)", Path(path).read_text(encoding="utf-8"))
    return m.group(1) if m else None


def check_package():
    """the manifests against src/: CI-friendly (no compiler needed)"""
    problems, notes = [], []
    lj = json.loads((SENDER / "library.json").read_text(encoding="utf-8"))
    props = dict(line.split("=", 1) for line in (SENDER / "library.properties").read_text(encoding="utf-8").splitlines()
                 if "=" in line and not line.startswith("#"))
    yml = (SENDER / "idf_component.yml").read_text(encoding="utf-8")
    cmake = (SENDER / "CMakeLists.txt").read_text(encoding="utf-8")
    sources = sorted(p.name for p in SRC.glob("*.c"))
    headers = sorted(p.name for p in list(SRC.glob("*.h")) + list(SRC.glob("*.hpp")))
    public = [h for h in headers if not h.endswith("_internal.h")]

    # versions: the protocol release of the package's header
    release = header_release(SRC / "ultra_protocol.h")
    m = re.search(r'#define ULTRA_SENDER_VERSION_STR "([^"]+)"', (SRC / "ultra_sender.h").read_text(encoding="utf-8"))
    ym = re.search(r'^version:\s*"?([^"\s]+)"?', yml, re.M)
    versions = {"src/ultra_protocol.h (release)": release, "library.json": lj.get("version"),
                "library.properties": props.get("version"), "idf_component.yml": ym.group(1) if ym else None,
                "ULTRA_SENDER_VERSION_STR": m.group(1) if m else None}
    if len(set(versions.values())) != 1 or release is None:
        problems.append("versions differ: " + ", ".join(f"{k} {v}" for k, v in versions.items()))

    # sources and headers
    cm = re.search(r"set\(ULTRA_SENDER_SRCS([^)]*)\)", cmake)
    listed = sorted(Path(x).name for x in re.findall(r'"([^"]+\.c)"', cm.group(1))) if cm else []
    if listed != sources:
        problems.append(f"CMakeLists.txt sources {listed} != src/*.c {sources}")
    if 'INCLUDE_DIRS "src"' not in cmake:
        problems.append('CMakeLists.txt: INCLUDE_DIRS "src" missing')
    if sorted(lj.get("headers", [])) != public:
        problems.append(f"library.json headers {sorted(lj.get('headers', []))} != src/ public headers {public}")
    b = lj.get("build", {})
    if b.get("srcDir") != "src" or b.get("includeDir") != "src":
        problems.append("library.json build.srcDir / includeDir must be src")
    for e in lj.get("export", {}).get("include", []):
        if not (SENDER / e).exists():
            problems.append(f"library.json export.include: {e} missing")
    for k in ("name", "version", "author", "maintainer", "sentence", "paragraph", "category", "url", "architectures"):
        if not props.get(k):
            problems.append(f"library.properties: {k} missing")
    for h in props.get("includes", "").split(","):
        if h.strip() and h.strip() not in public:
            problems.append(f"library.properties includes {h.strip()}: not a header in src/")
    for ex in re.findall(r"^\s*-\s*path:\s*(\S+)", yml, re.M):
        if not (SENDER / ex).exists():
            problems.append(f"idf_component.yml example {ex} missing")

    # self-contained: every quoted include of src/ is in src/ (the RMT helper's ESP-IDF headers aside)
    for f in sorted(SRC.iterdir()):
        for inc in re.findall(r'#include\s+"([^"]+)"', f.read_text(encoding="utf-8")):
            idf = f.name == "ultra_esp32_rmt.h" and inc.startswith(("driver/", "soc/", "esp_"))
            if not (SRC / inc).exists() and not idf:
                problems.append(f"src/{f.name} includes {inc}: not in src/")

    # The implementation and normative headers must describe the same wire-3 contract.
    copy = (SRC / "ultra_protocol.h").read_bytes().replace(b"\r\n", b"\n")
    pin = json.loads((SENDER / "PROTOCOL-PIN.json").read_text(encoding="utf-8"))
    if pin["protocol"] != release or hashlib.sha256(copy).hexdigest() != pin["sha256_lf"]:
        problems.append("sender protocol header differs from PROTOCOL-PIN.json")
    for name in ("ultra_protocol.h", "ultra_transport.h", "ultra_update.h"):
        if (SRC/name).read_bytes().replace(b"\r\n",b"\n") != (PROTO_INC/name).read_bytes().replace(b"\r\n",b"\n"):
            problems.append(name + " differs from normative include/")
    if pin.get("wire") != 3: problems.append("wire-3 pin required")
    if (SENDER / "LICENSE").read_bytes() != (REPO / "LICENSE").read_bytes():
        problems.append("LICENSE differs from the repository's")

    # The installed-version gate: applications and bootloaders refuse an equal or older firmware unless the ANNOUNCE
    # carries FORCE (which the sender never sets). No text may say a bootloader doesn't check versions, and the
    # texts that give the recovery planning rule name FORCE as the reason.
    stale = re.compile(r"bootloaders?\s+(?:doesn'?t|don'?t|does\s+not|do\s+not|never)\s+check\s+versions?", re.I)
    for f in [*sorted(SRC.iterdir()), SENDER / "README.md", *sorted((REPO / "docs").glob("*.md"))]:
        if stale.search(re.sub(r"\s*\n\s*(?:\*\s*)?", " ", f.read_text(encoding="utf-8"))):
            problems.append(f"{f.relative_to(REPO)}: says a bootloader doesn't check versions")
    for f in (REPO / "docs" / "update.md", SENDER / "README.md", SRC / "ultra_sender_job.h", SRC / "ultra_sender_job.c"):
        if "FORCE" not in f.read_text(encoding="utf-8"):
            problems.append(f"{f.relative_to(REPO)}: the equal / newer rule must name FORCE as its reason")
    # "Update all" refuses while a known board is silent; recover is the separate action for one board. The port
    # job plans no recovery target of its own, in code or in text.
    for f, need in ((REPO / "docs" / "update.md", ("### Recover one silent board", "(#recover-one-silent-board)",
                                                    '"not answering', '"recovery sent"')),
                    (SENDER / "README.md", ("ULTRA_UPDATE_SILENT_BOARDS", "ultra_update_job_recover(",
                                            "ULTRA_UPDATE_NOT_RECOVERABLE")),
                    (SRC / "ultra_sender_job.h", ("ULTRA_UPDATE_SILENT_BOARDS", "ultra_update_job_recover(",
                                                  "ULTRA_UPDATE_NOT_RECOVERABLE"))):
        text = f.read_text(encoding="utf-8")
        problems += [f"{f.relative_to(REPO)}: must describe {n}" for n in need if n not in text]
    for f in [*sorted(SRC.iterdir()), SENDER / "README.md", *sorted((REPO / "docs").glob("*.md"))]:
        if re.search(r"recovery\s+target", f.read_text(encoding="utf-8"), re.I):
            problems.append(f"{f.relative_to(REPO)}: names a port-job recovery target")

    for n in notes:
        print("NOTE " + n)
    note(not problems, f"package: manifests match src/ ({len(sources)} sources, {len(public)} public headers), "
                       f"version {release}" + "".join("\n     " + p for p in problems))


def release_vectors():
    """Only the checked-in wire-3 contract is supported."""
    return VECTORS


def sync_header():
    for name in ("ultra_protocol.h", "ultra_transport.h", "ultra_update.h"):
        (SRC/name).write_bytes((PROTO_INC/name).read_bytes())
    pin=json.loads((SENDER/"PROTOCOL-PIN.json").read_text())
    pin.update(protocol=header_release(SRC/"ultra_protocol.h"),wire=3,sha256_lf=hashlib.sha256((SRC/"ultra_protocol.h").read_bytes().replace(b"\r\n",b"\n")).hexdigest())
    (SENDER/"PROTOCOL-PIN.json").write_text(json.dumps(pin,indent=2)+"\n")



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", help="the board firmware (its src/core): frame_rx.c, proto.c, ultra.c")
    ap.add_argument("--skip-cross", action="store_true", help="no cross-compiler warnings / size check")
    ap.add_argument("--only", help="comma list of steps: manifests, warnings, cross, vectors, unit, review, pixel, dport, chain, "
                                   "cpp, rmt, equiv, rotation, rules, minimal, wire3, wire3cpp, example, sim, dline, asan")
    ap.add_argument("--sync-header", action="store_true", help="copy include/ultra_protocol.h into src/ and stop")
    a = ap.parse_args()
    if a.sync_header:
        sync_header()
        return 0
    only = set(a.only.split(",")) if a.only else None

    def want(step):
        return only is None or step in only

    if want("manifests"):
        check_package()
        if only == {"manifests"}:
            return 0 if all(ok for ok, _ in results) else 1
    kind, cc = host_cc()
    if not cc:
        print("no host C compiler found (set CC)")
        return 2
    print(f"host compiler: {'MSVC ' + str(cc) if kind == 'msvc' else cc}")
    if want("warnings"):
        warnings_check(kind, cc)
    if want("cross") and not a.skip_cross:
        for label, xcc, flags in cross_compilers():
            cross_check(label, xcc, flags)

    lib_incs = INC                               # (the package: src/ only, its own protocol header)
    host = {
        "example": ("host_frame_example", [REPO/"examples"/"host_frame_example.c"], [PROTO_INC], [], (), False),
        "wire3": ("test_wire3", [HERE / "test_vectors.c", *LIB], lib_incs, [], (), False),
        "wire3cpp": ("test_wire3_cpp", [HERE / "test_wire3.cpp", *LIB], lib_incs, [], (), True),
        "vectors": ("test_spec_vectors", [REPO / "test-vectors" / "test_vectors.c"], [PROTO_INC], [str(VECTORS)], (), False),
        "unit": ("test_unit", [HERE / "test_unit.c", *LIB], lib_incs, [], (), False),
        "review": ("test_review", [HERE / "test_review.c", *LIB], lib_incs, [], (), False),
        "pixel": ("test_pixel", [HERE / "test_pixel.c", *LIB], lib_incs, [], (), False),
        "dport": ("test_dport", [HERE / "test_dport.c", *LIB], [HERE, *lib_incs], [], (), False),
        "chain": ("test_chain", [HERE / "test_chain.c", *LIB], lib_incs,
                  [str(BUILD / "update-traces")], ("ULTRA_SENDER_MAX_BOARDS=1024",), False),
        "cpp": ("test_cpp", [HERE / "test_cpp.cpp", *LIB], lib_incs, [], (), True),
        "rmt": ("test_rmt", [HERE / "test_rmt.c", *LIB], [HERE / "mock_idf", *lib_incs], [], ("ESP_PLATFORM",),
                False),
        # round 9: the changed arithmetic against its old code, verbatim (the library's sources compiled in)
        "equiv": ("test_equiv", [HERE / "test_equiv.c", SRC / "ultra_sender_descriptor.c"], lib_incs, [], (), False),
        # round 11: the refresh rotation, round by round, against the golden trace ESPHome's host test shares
        "rotation": ("test_rotation", [HERE / "test_rotation.c", SRC / "ultra_sender_descriptor.c"], lib_incs, [str(HERE / "rotation_trace.txt")], (),
                     False),
        # the recorded design limits as compiled, for test-vectors/check_rules.py (one line wider than every port type)
        "rules": ("rules_probe", [HERE / "rules_probe.c", *LIB], lib_incs, [str(BUILD / "rules_probe.txt")],
                  ("ULTRA_SENDER_MAX_BOARDS=64",), False),
    }
    for step, (name, srcs, incs, args, defines, cpp) in host.items():
        if want(step):
            if step == "chain":
                (BUILD / "update-traces").mkdir(parents=True, exist_ok=True)
            if step == "vectors":
                for script, extra in ((REPO / "test-vectors" / "check_vectors.py", []),):
                    result = subprocess.run([sys.executable, str(script), *extra], capture_output=True, text=True)
                    print(result.stdout + result.stderr)
                    note(result.returncode == 0, script.name)
            if step == "rules":
                (BUILD / "rules_probe.txt").unlink(missing_ok=True)
            passed = build_run(kind, cc, name, srcs, incs, args, defines, cpp=cpp)
            if step == "rules":
                # every recorded limit: its constant, documents and refusals; a mutant per rule must fail
                probe = ["--probe", str(BUILD / "rules_probe.txt")] if passed else []
                result = subprocess.run([sys.executable, str(REPO / "test-vectors" / "check_rules.py"), *probe,
                                         "--mutants"], capture_output=True, text=True)
                print(result.stdout + result.stderr)
                note(result.returncode == 0 and passed, "check_rules.py: recorded design limits")
            if step == "chain" and passed:
                result = subprocess.run([sys.executable, str(HERE / "check_update_traces.py"),
                                         str(BUILD / "update-traces")], capture_output=True, text=True)
                print(result.stdout + result.stderr)
                note(result.returncode == 0, "independent update schedules")
            if step == "vectors":
                build_run(kind, cc, "test_spec_cpp", [REPO / "test-vectors" / "test_vectors.cpp"],
                          [PROTO_INC], [str(VECTORS)], cpp=True)
    if want("minimal"):
        build_run(kind, cc, "host_minimal", [SENDER / "examples" / "host_minimal.c", *LIB], lib_incs, ["--quiet"])

    core = firmware_dir(a.firmware) if want("sim") else None
    if not want("sim"):
        pass
    elif core is None:
        note(False, "board firmware not found (--firmware / ULTRA_FIRMWARE): test_sim can't run")
    else:
        print(f"board firmware: {core}")
        fw_h, pkg_h = (core / "ultra_protocol.h").read_bytes(), (SRC / "ultra_protocol.h").read_bytes()
        if fw_h != pkg_h:
            # one translation unit holds both sides: the first header's include guard would hide the other, and the
            # sender's structs (CONFIG storage) would differ between test_sim.c and the library
            eol = (" Only the line endings differ: a CRLF checkout of the firmware (its .gitattributes: LF for "
                   "src/core/ultra_protocol.h)." if fw_h.replace(b"\r\n", b"\n") == pkg_h.replace(b"\r\n", b"\n")
                   else "")
            note(False, f"test_sim: the firmware's ultra_protocol.h (md5 {hashlib.md5(fw_h).hexdigest()}, release "
                        f"{header_release(core / 'ultra_protocol.h')}) isn't the package's (md5 "
                        f"{hashlib.md5(pkg_h).hexdigest()}, {header_release(SRC / 'ultra_protocol.h')}): a firmware "
                        f"of the same protocol release (--firmware).{eol}")
        else:
            objs = build_firmware_objs(kind, cc, core)
            if objs is None:
                note(False, "test_sim: building the firmware's receive path")
            else:
                print(f"one ultra_protocol.h for both sides: md5 {hashlib.md5(pkg_h).hexdigest()}")
                build_run(kind, cc, "test_sim", [HERE / "test_sim.c", *LIB], [core] + INC, fw_objs=objs)
                # one shared start against the boards' boot epochs: every chain length, rate, slot and launch phase
                build_run(kind, cc, "test_epoch", [HERE / "test_epoch.c", *LIB], [core] + INC,
                          defines=("ULTRA_SENDER_MAX_BOARDS=50",), fw_objs=objs)   # (a Dig2Analog-Plus-5ch pin: 50)
                # the app's one-frame capture driven by the sender's frame times, into the app's update decisions
                # (core/upd_app.c): the paced library takes the image; the unpaced control must not (test_mailbox.c)
                upd = build_firmware_objs(kind, cc, core, (), "fw_upd", ("upd_app.c",))
                if upd is None:
                    note(False, "test_mailbox: building the firmware's core/upd_app.c")
                else:
                    others = [s for s in LIB if s.name != "ultra_sender_update.c"]   # (test_mailbox.c includes it)
                    build_run(kind, cc, "test_mailbox", [HERE / "test_mailbox.c", *others], [core] + INC, fw_objs=upd)
                    # the control: the same source, each pace step adding 0u (the schedule before the pace)
                    text = (SRC / "ultra_sender_update.c").read_text(encoding="utf-8")
                    steps = text.count("+=ULTRA_UPDATE_CONTROL_PACE_US")
                    if steps != 4:
                        note(False, f"test_mailbox_unpaced: {steps} pace steps in ultra_sender_update.c, the control "
                                    "knows 4 (the last training PING, SELECT, ANNOUNCE half 0, END copies)")
                    else:
                        copy = BUILD / "unpaced_src"
                        copy.mkdir(parents=True, exist_ok=True)
                        (copy / "ultra_sender_update.c").write_text(
                            text.replace("+=ULTRA_UPDATE_CONTROL_PACE_US", "+=0u"), encoding="utf-8")
                        build_run(kind, cc, "test_mailbox_unpaced", [HERE / "test_mailbox.c", *others],
                                  [copy, core] + INC, defines=("MAILBOX_UNPACED=1",), fw_objs=upd)
                # the same receive path built as the Diff-Power ultra receiver (BOARD=2: it takes IDENTIFY 7)
                rx = build_firmware_objs(kind, cc, core, ("BOARD=2",), "fw_rx", FW_FILES + RX_FILES)
                if rx is None:
                    note(False, "test_sim_rx: building the receiver's receive path (BOARD=2)")
                else:
                    build_run(kind, cc, "test_sim_rx", [HERE / "test_sim.c", *LIB], [core] + INC,
                              defines=("SIM_RX=1",), fw_objs=rx)

    if want("dline"):
        core = core if core is not None else firmware_dir(a.firmware)
        if core is None:
            note(False, "dline sim: board firmware not found (--firmware / ULTRA_FIRMWARE)")
        elif (core / "ultra_protocol.h").read_bytes() != (SRC / "ultra_protocol.h").read_bytes():
            note(False, "dline sim: the firmware's ultra_protocol.h isn't the package's (see test_sim)")
        else:
            dline_sim(kind, cc, core)

    if want("asan"):
        # the same tests under AddressSanitizer (the library's memory use; UBSan too with gcc / clang)
        for step in ("wire3", "wire3cpp", "unit", "vectors", "review", "pixel", "dport", "chain", "cpp", "rmt", "equiv", "rotation"):
            name, srcs, incs, args, defines, cpp = host[step]
            build_run(kind, cc, name, srcs, incs, args, defines, cpp=cpp, asan=True)

    failed = [w for ok, w in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed" + (": FAILED" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
