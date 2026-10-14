#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 QuinLED
"""Build and run the QuinLED Ultra sender library's tests.

  python sender/test/run_tests.py [--firmware DIR] [--release] [--only STEPS] [--skip-cross]

1. Warnings: the library compiles with every compiler found at -std=c99 -Wall -Wextra -Wpedantic -Werror (MSVC: /W4
   /WX): the host compiler, and cross compilers if present (xtensa-esp32-elf-gcc, riscv*-elf-gcc, clang / esp-clang),
   the C++ wrapper too.
2. Host tests (host compiler: CC (+ CXX), else MSVC on Windows, else cc / gcc / clang):
   - test_vectors: every protocol test vector, produced through the library's API, byte for byte;
   - test_unit: the scheduler's rules (copies, order, refresh, preamble, TIME, start offsets, encoders, ...);
   - test_review: the regressions of the independent review (round 7);
   - test_cpp: the C++ wrapper at run time;
   - test_rmt: the ESP32 RMT helper (ultra_esp32_rmt.h) against a mocked ESP-IDF RMT driver;
   - test_sim: the library's frames through the board firmware's own receive path and light engine (frame_rx.c,
     proto.c, ultra.c), 1-64 boards, frame loss, power cycles, group starts. Needs the firmware sources: --firmware,
     ULTRA_FIRMWARE, or a checkout next to this repository; skipped (with a note) when there is none, a failure with
     --release. Both sides share one translation unit, so the firmware's ultra_protocol.h must be byte-identical to
     src/ultra_protocol.h (a firmware of the same protocol release): a different one is a failure, never mixed;
   - examples/host_minimal.c builds and runs;
   - asan: test_unit, test_vectors, test_review, test_cpp and test_rmt again under AddressSanitizer (MSVC
     /fsanitize=address; gcc / clang: -fsanitize=address,undefined).
3. Code size (cross compilers found): the library's .text at -Os for ESP32 (Xtensa) and RV32IMC.
4. The package (`--only manifests`, CI-friendly): library.json, library.properties, idf_component.yml and CMakeLists.txt
   list exactly the sources and headers in src/, every version is the protocol release of src/ultra_protocol.h, that
   header is a byte-identical copy of include/ultra_protocol.h, src/ includes nothing from outside src/, LICENSE is the
   repository's.
Exit status 0 when everything that ran passed.

  python sender/test/run_tests.py --sync-header     copy include/ultra_protocol.h into src/ (a new protocol release);
                                                    then set the four versions the check names
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

HERE = Path(__file__).resolve().parent
SENDER = HERE.parent
REPO = SENDER.parent
PROTO_INC = REPO / "include"
VECTORS = REPO / "test-vectors" / "vectors.json"
BUILD = SENDER / "build"
SRC = SENDER / "src"
LIB = [SRC / f for f in ("ultra_sender.c", "ultra_sender_bits.c", "ultra_sender_rc.c")]
INC = [SRC]                                      # every header, the protocol header's copy too
FW_FILES = ("frame_rx.c", "proto.c", "ultra.c", "ultra_tables.c")

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
    BUILD.mkdir(exist_ok=True)
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
    BUILD.mkdir(exist_ok=True)
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
    BUILD.mkdir(exist_ok=True)
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


def build_run(kind, cc, name, sources, incs, args=(), defines=(), fw_objs=(), cpp=False, asan=False):
    """build sources (C, or C++ with cpp: C++17; the C ones of a C++ build as C) into one executable and run it"""
    BUILD.mkdir(exist_ok=True)
    tag = name + ("_asan" if asan else "")
    exe = BUILD / (tag + (".exe" if sys.platform == "win32" else ""))
    objdir = BUILD / ("obj_" + tag)
    objdir.mkdir(exist_ok=True)
    if kind == "msvc":
        a = ["/nologo", "/O2", "/W4", "/WX", "/wd4996"]
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
            r = subprocess.run([comp, "-std=c++17" if is_cpp else "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
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
    tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-40:])
    print(tail)
    note(r.returncode == 0, f"{tag}" + ("" if r.returncode == 0 else f" (exit {r.returncode})"))
    return r.returncode == 0


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


def build_firmware_objs(kind, cc, core):
    """the board firmware's receive path and engine, compiled as the firmware's host tests do"""
    objs = []
    fwb = BUILD / "fw"
    fwb.mkdir(parents=True, exist_ok=True)
    for f in FW_FILES:
        if kind == "msvc":
            obj = fwb / (Path(f).stem + ".obj")
            r = run_msvc(cc, ["/nologo", "/c", "/O2", "/W3", "/wd4996", "/I", str(core), "/Fo" + str(obj),
                              str(core / f)], BUILD)
        else:
            obj = fwb / (Path(f).stem + ".o")
            r = subprocess.run([cc, "-std=c99", "-O2", "-w", f"-I{core}", "-c", str(core / f), "-o", str(obj)],
                               capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, getattr(r, "stderr", ""))
            return None
        objs.append(obj)
    return objs


# ------------------------------------------------------------------------------------------------ the package
def header_release(path):
    m = re.search(r"this file: release (\d+\.\d+\.\d+)", Path(path).read_text(encoding="utf-8"))
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

    # the protocol header: a byte-identical copy of the repository's
    copy, master = (SRC / "ultra_protocol.h").read_bytes(), PROTO_INC.joinpath("ultra_protocol.h").read_bytes()
    if copy != master:
        head = subprocess.run(["git", "show", "HEAD:include/ultra_protocol.h"], cwd=REPO, capture_output=True)
        if head.returncode == 0 and head.stdout.replace(b"\r\n", b"\n") == copy:
            notes.append(f"include/ultra_protocol.h has uncommitted changes; the package's copy is the committed release "
                         f"{release}: --sync-header when the next release lands")
        else:
            problems.append("src/ultra_protocol.h differs from include/ultra_protocol.h: --sync-header")
    if (SENDER / "LICENSE").read_bytes() != (REPO / "LICENSE").read_bytes():
        problems.append("LICENSE differs from the repository's")

    for n in notes:
        print("NOTE " + n)
    note(not problems, f"package: manifests match src/ ({len(sources)} sources, {len(public)} public headers), "
                       f"version {release}" + "".join("\n     " + p for p in problems))


def release_vectors():
    """the vectors of the release the package's header is: the working tree's, or (a newer header in progress, the
    package still on the committed release) the committed ones"""
    if (SRC / "ultra_protocol.h").read_bytes() == PROTO_INC.joinpath("ultra_protocol.h").read_bytes():
        return VECTORS
    head = subprocess.run(["git", "show", "HEAD:test-vectors/vectors.json"], cwd=REPO, capture_output=True)
    hh = subprocess.run(["git", "show", "HEAD:include/ultra_protocol.h"], cwd=REPO, capture_output=True)
    if head.returncode == 0 and hh.returncode == 0 and \
            hh.stdout.replace(b"\r\n", b"\n") == (SRC / "ultra_protocol.h").read_bytes():
        BUILD.mkdir(exist_ok=True)
        out = BUILD / "vectors_release.json"
        out.write_bytes(head.stdout)
        print(f"NOTE the vectors of the committed release {header_release(SRC / 'ultra_protocol.h')} (the package's header)")
        return out
    return VECTORS


def sync_header():
    shutil.copyfile(PROTO_INC / "ultra_protocol.h", SRC / "ultra_protocol.h")
    print(f"src/ultra_protocol.h = include/ultra_protocol.h (release {header_release(SRC / 'ultra_protocol.h')}); "
          f"now set that version in library.json, library.properties, idf_component.yml and ULTRA_SENDER_VERSION_STR")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", help="the board firmware (its src/core): frame_rx.c, proto.c, ultra.c")
    ap.add_argument("--skip-cross", action="store_true", help="no cross-compiler warnings / size check")
    ap.add_argument("--only", help="comma list of steps: manifests, warnings, cross, vectors, unit, review, cpp, rmt, "
                                   "minimal, sim, asan")
    ap.add_argument("--release", action="store_true",
                    help="a release check: a step that can't run (no firmware for sim) is a failure, not a note")
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
        "vectors": ("test_vectors", [HERE / "test_vectors.c", *LIB], lib_incs, [str(release_vectors())], (), False),
        "unit": ("test_unit", [HERE / "test_unit.c", *LIB], lib_incs, [], (), False),
        "review": ("test_review", [HERE / "test_review.c", *LIB], lib_incs, [], (), False),
        "cpp": ("test_cpp", [HERE / "test_cpp.cpp", *LIB], lib_incs, [], (), True),
        "rmt": ("test_rmt", [HERE / "test_rmt.c", *LIB[:2]], [HERE / "mock_idf", *lib_incs], [], ("ESP_PLATFORM",),
                False),
    }
    for step, (name, srcs, incs, args, defines, cpp) in host.items():
        if want(step):
            build_run(kind, cc, name, srcs, incs, args, defines, cpp=cpp)
    if want("minimal"):
        build_run(kind, cc, "host_minimal", [SENDER / "examples" / "host_minimal.c", *LIB[:2]], lib_incs, ["--quiet"])

    core = firmware_dir(a.firmware) if want("sim") else None
    if not want("sim"):
        pass
    elif core is None:
        msg = "board firmware not found (--firmware / ULTRA_FIRMWARE): test_sim"
        if a.release:
            note(False, msg + " can't run (--release)")
        else:
            print("NOTE " + msg + " skipped")
    else:
        print(f"board firmware: {core}")
        fw_h, pkg_h = (core / "ultra_protocol.h").read_bytes(), (SRC / "ultra_protocol.h").read_bytes()
        if fw_h != pkg_h:
            # one translation unit holds both sides: the first header's include guard would hide the other, and the
            # sender's structs (CONFIG storage) would differ between test_sim.c and the library
            note(False, f"test_sim: the firmware's ultra_protocol.h (md5 {hashlib.md5(fw_h).hexdigest()}, release "
                        f"{header_release(core / 'ultra_protocol.h')}) isn't the package's (md5 "
                        f"{hashlib.md5(pkg_h).hexdigest()}, {header_release(SRC / 'ultra_protocol.h')}): a firmware "
                        f"of the same protocol release (--firmware)")
        else:
            objs = build_firmware_objs(kind, cc, core)
            if objs is None:
                note(False, "test_sim: building the firmware's receive path")
            else:
                print(f"one ultra_protocol.h for both sides: md5 {hashlib.md5(pkg_h).hexdigest()}")
                build_run(kind, cc, "test_sim", [HERE / "test_sim.c", *LIB[:2]], [core] + INC, fw_objs=objs)

    if want("asan"):
        # the same tests under AddressSanitizer (the library's memory use; UBSan too with gcc / clang)
        for step in ("unit", "vectors", "review", "cpp", "rmt"):
            name, srcs, incs, args, defines, cpp = host[step]
            build_run(kind, cc, name, srcs, incs, args, defines, cpp=cpp, asan=True)

    failed = [w for ok, w in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed" + (": FAILED" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
