#!/usr/bin/env python3
"""Check the QuinLED Ultra pixel effect catalogue (pixel_fx_catalogue.json).

    python check_catalogue.py pixel_fx_catalogue.json [--against previous.json]

The catalogue lists the effects and palettes the Ultra-Digital receiver's firmware runs itself (PIXEL_FX, protocol
0.2.8); the ESPHome component generates its effect list from it.

Rules:
  - catalogue_version is an integer 0-65535 (the u16 of page 0x81 part 0); raise it whenever the content changes.
  - effects: id 1-255 (1-127 official, 128-255 custom), unique and never reused; a unique name (the Home Assistant
    effect name, case-insensitive); summary (text); speed / intensity / palette: true / false (the slider or the Palette
    select is used) with speed_label / intensity_label (text, null when unused); colors: how many of c1..c3 it uses (0-3)
    and color_slots: three labels by position (null: that colour is not used); custom1 / custom2: null or a label; defaults: speed, intensity, palette (0-127, 0xFE c1 -> c2 -> c3, 0xFF the effect's own),
    c1 / c2 / c3 ([r, g, b]), custom1, custom2; state_bytes_per_pixel (int); origin (text: where the algorithm is from,
    for the licence notices).
  - palettes: id 0-127, unique and never reused; unique name; origin (text); cyclic (true / false). 0xFE and 0xFF are
    implicit (palette_special) and are not listed.
  - With --against: every id of the previous catalogue is still there with the same name (an effect or palette may be
    marked "retired": true, never removed or renamed), and a changed catalogue has a higher catalogue_version.
  - "retired": true effects and palettes stay in the file; a host does not offer them.
Exit status 0: fine, 1: a problem (each printed).
"""
import json
import sys

EFFECT_KEYS = {"id", "name", "summary", "colors", "color_slots", "speed", "speed_label", "intensity", "intensity_label",
               "palette", "custom1", "custom2", "defaults", "state_bytes_per_pixel", "origin", "retired"}
DEFAULT_KEYS = {"speed", "intensity", "palette", "c1", "c2", "c3", "custom1", "custom2"}
PALETTE_KEYS = {"id", "name", "origin", "cyclic", "retired"}


def _label(v):
    return v is None or (isinstance(v, str) and v.strip() != "")


def _byte(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 255


def check(cat, previous=None):
    errors = []
    version = cat.get("catalogue_version")
    if not isinstance(version, int) or isinstance(version, bool) or not 0 <= version <= 65535:
        errors.append("catalogue_version must be an integer 0-65535 (the u16 of page 0x81 part 0)")
    special = cat.get("palette_special")
    if special is not None and special != {"colors": 254, "default": 255}:
        errors.append("palette_special must be {colors: 254, default: 255}")
    for kind, lo, hi in (("effects", 1, 255), ("palettes", 0, 127)):
        if not isinstance(cat.get(kind), list) or not cat[kind]:
            errors.append(f"{kind}: a non-empty list is required")
            continue
        ids, names = set(), set()
        for i, e in enumerate(cat[kind]):
            where = f"{kind}[{i}]"
            eid = e.get("id")
            if not isinstance(eid, int) or isinstance(eid, bool) or not lo <= eid <= hi:
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
            if not isinstance(e.get("origin"), str) or not e["origin"].strip():
                errors.append(f"{where}: origin (where it is from, for the licence notices) is required")
            if kind == "palettes":
                extra = set(e) - PALETTE_KEYS
                if extra:
                    errors.append(f"{where}: unknown keys {sorted(extra)}")
                if not isinstance(e.get("cyclic"), bool):
                    errors.append(f"{where}: cyclic must be true or false")
                continue
            extra = set(e) - EFFECT_KEYS
            if extra:
                errors.append(f"{where}: unknown keys {sorted(extra)}")
            if not isinstance(e.get("summary"), str) or not e["summary"].strip():
                errors.append(f"{where}: a summary is required")
            for k in ("custom1", "custom2", "speed_label", "intensity_label"):
                if k not in e or not _label(e[k]):
                    errors.append(f"{where}: {k} must be null or a label")
            for k in ("speed", "intensity", "palette"):
                if not isinstance(e.get(k), bool):
                    errors.append(f"{where}: {k} must be true or false")
                elif k != "palette" and e[k] != (e.get(k + "_label") is not None):
                    errors.append(f"{where}: {k} and {k}_label disagree (a label exactly when it is used)")
            cols, slots = e.get("colors"), e.get("color_slots")
            if not isinstance(cols, int) or isinstance(cols, bool) or not 0 <= cols <= 3:
                errors.append(f"{where}: colors must be 0-3")
            if not isinstance(slots, list) or len(slots) != 3 or any(not _label(c) for c in slots):
                errors.append(f"{where}: color_slots is three labels or nulls")
            elif isinstance(cols, int) and sum(c is not None for c in slots) != cols:
                errors.append(f"{where}: colors ({cols}) is not the number of color_slots labels")
            d = e.get("defaults")
            if not isinstance(d, dict) or set(d) != DEFAULT_KEYS:
                errors.append(f"{where}: defaults hold exactly {sorted(DEFAULT_KEYS)}")
            else:
                for k in ("speed", "intensity", "custom1", "custom2"):
                    if not _byte(d[k]):
                        errors.append(f"{where}: defaults.{k} must be 0-255")
                if not (_byte(d["palette"]) and (d["palette"] < 128 or d["palette"] in (0xFE, 0xFF))):
                    errors.append(f"{where}: defaults.palette must be a palette id, 254 or 255")
                for k in ("c1", "c2", "c3"):
                    if not (isinstance(d[k], list) and len(d[k]) == 3 and all(_byte(v) for v in d[k])):
                        errors.append(f"{where}: defaults.{k} must be [r, g, b]")
            if not isinstance(e.get("state_bytes_per_pixel"), int) or e["state_bytes_per_pixel"] < 0:
                errors.append(f"{where}: state_bytes_per_pixel must be an integer >= 0")
    if isinstance(cat.get("effects"), list) and isinstance(cat.get("palettes"), list):
        palette_ids = {p.get("id") for p in cat["palettes"]}
        for e in cat["effects"]:
            pal = (e.get("defaults") or {}).get("palette")
            if isinstance(pal, int) and pal < 128 and pal not in palette_ids:
                errors.append(f"effect '{e.get('name')}': default palette {pal} is not in the catalogue")
    if previous is not None:
        for kind in ("effects", "palettes"):
            now = {e["id"]: e for e in cat.get(kind, []) if isinstance(e.get("id"), int)}
            for old in previous.get(kind, []):
                new = now.get(old["id"])
                if new is None:
                    errors.append(f"{kind} id {old['id']} ('{old['name']}') was removed: mark it \"retired\": true")
                elif new["name"] != old["name"]:
                    errors.append(f"{kind} id {old['id']} was renamed '{old['name']}' -> '{new['name']}': ids are never reused")
        if isinstance(version, int) and version <= previous.get("catalogue_version", 0) and cat != previous:
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
