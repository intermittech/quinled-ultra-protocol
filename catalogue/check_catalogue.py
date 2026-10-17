#!/usr/bin/env python3
"""Check a QuinLED Ultra pixel effect catalogue (pixel_fx_catalogue.json).

    python check_catalogue.py pixel_fx_catalogue.json [--against previous.json]

Rules: ids are 1-127 (the official catalogue) or 128-255 (custom), unique, never reused: with --against, every id of
the previous catalogue is still there with the same name (an effect may be marked "retired": true, never removed or
renamed). Names are unique (the Home Assistant effect names). Palette ids are 0-127; 0xFE (c1 -> c2 -> c3) and 0xFF
(the effect's own) are implicit and may not be listed. Exit status 0: fine, 1: a problem (each printed).
"""
import json
import sys

EFFECT_KEYS = {"id", "name", "colors", "speed", "intensity", "palette", "custom1", "custom2", "defaults", "retired"}
DEFAULT_KEYS = {"speed", "intensity", "palette"}


def check(cat, previous=None):
    errors = []
    if not isinstance(cat.get("catalogue_version"), int) or cat["catalogue_version"] < 0 or cat["catalogue_version"] > 65535:
        errors.append("catalogue_version must be an integer 0-65535 (the u16 of page 0x81 part 0)")
    for kind, lo, hi in (("effects", 1, 255), ("palettes", 0, 127)):
        if not isinstance(cat.get(kind), list) or not cat[kind]:
            errors.append(f"{kind}: a non-empty list is required")
            continue
        ids, names = set(), set()
        for i, e in enumerate(cat[kind]):
            where = f"{kind}[{i}]"
            eid = e.get("id")
            if not isinstance(eid, int) or not lo <= eid <= hi:
                errors.append(f"{where}: id must be an integer {lo}-{hi}")
                continue
            if eid in ids:
                errors.append(f"{where}: id {eid} is used twice")
            ids.add(eid)
            name = e.get("name")
            if not isinstance(name, str) or not name.strip():
                errors.append(f"{where}: a name is required")
            elif name.lower() in names:
                errors.append(f"{where}: name '{name}' is used twice")
            else:
                names.add(name.lower())
            if kind == "effects":
                extra = set(e) - EFFECT_KEYS
                if extra:
                    errors.append(f"{where}: unknown keys {sorted(extra)}")
                if not isinstance(e.get("colors"), int) or not 0 <= e["colors"] <= 3:
                    errors.append(f"{where}: colors must be 0-3")
                for k in ("speed", "intensity", "palette"):
                    if not isinstance(e.get(k), bool):
                        errors.append(f"{where}: {k} must be true or false")
                for k in ("custom1", "custom2"):
                    if e.get(k) is not None and not isinstance(e.get(k), str):
                        errors.append(f"{where}: {k} must be null or a label")
                d = e.get("defaults", {})
                if set(d) - DEFAULT_KEYS or any(not isinstance(v, int) or not 0 <= v <= 255 for v in d.values()):
                    errors.append(f"{where}: defaults hold speed, intensity, palette as 0-255")
                if "palette" in d and d["palette"] not in (0xFE, 0xFF) and d["palette"] > 127:
                    errors.append(f"{where}: default palette {d['palette']} is not a palette id, 0xFE or 0xFF")
    if previous is not None:
        for kind in ("effects", "palettes"):
            now = {e["id"]: e for e in cat.get(kind, []) if isinstance(e.get("id"), int)}
            for old in previous.get(kind, []):
                new = now.get(old["id"])
                if new is None:
                    errors.append(f"{kind} id {old['id']} ('{old['name']}') was removed: mark it \"retired\": true instead")
                elif new["name"] != old["name"]:
                    errors.append(f"{kind} id {old['id']} was renamed '{old['name']}' -> '{new['name']}': ids are never reused")
        if cat.get("catalogue_version", 0) <= previous.get("catalogue_version", 0) and cat != previous:
            errors.append("the catalogue changed: raise catalogue_version")
    return errors


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        cat = json.load(f)
    previous = None
    if "--against" in argv:
        with open(argv[argv.index("--against") + 1], encoding="utf-8") as f:
            previous = json.load(f)
    errors = check(cat, previous)
    for e in errors:
        print("FAIL:", e)
    if not errors:
        print(f"catalogue ok: version {cat['catalogue_version']}, {len(cat['effects'])} effects, {len(cat['palettes'])} palettes")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
