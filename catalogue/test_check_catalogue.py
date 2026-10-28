#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 QuinLED
"""Tests of the catalogue and its checker: the real file passes, every rule has a negative control.

    python catalogue/test_check_catalogue.py
"""
import copy
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_catalogue as c   # noqa: E402

CAT = json.loads((HERE / "pixel_fx_catalogue.json").read_text(encoding="utf-8"))
failures = 0


def expect(name, errors, needle=None):
    global failures
    good = (not errors) if needle is None else any(needle in e for e in errors)
    print(f"{'ok  ' if good else 'FAIL'} {name}" + ("" if good else f": {errors[:2]}"))
    failures += not good


def broken(mut):
    x = copy.deepcopy(CAT)
    mut(x)
    return x


expect("the catalogue passes", c.check(CAT))
expect("a catalogue against itself", c.check(CAT, copy.deepcopy(CAT)))
expect("an id twice", c.check(broken(lambda x: x["effects"][1].update(id=1))), "used twice")
expect("a name twice (any case)", c.check(broken(lambda x: x["effects"][1].update(name="solid"))), "used twice")
expect("id 0 is the stream, not an effect", c.check(broken(lambda x: x["effects"][0].update(id=0))), "id must be")
expect("palette ids are 0-127", c.check(broken(lambda x: x["palettes"][0].update(id=200))), "id must be")
expect("no per-effect palette flag", c.check(broken(lambda x: x["effects"][0].update(palette=True))), "unknown keys")
own = next(i for i, e in enumerate(CAT["effects"]) if e.get("default_palette") == "own")
pal = next(i for i, e in enumerate(CAT["effects"]) if "default_palette_id" in e)
expect("own colours and a default palette", c.check(broken(lambda x: x["effects"][own].update(default_palette_id=0))),
       "Effect default is either")
expect("neither own colours nor a default palette", c.check(broken(lambda x: x["effects"][pal].pop("default_palette_id"))),
       "Effect default is either")
expect("defaults.palette follows the default palette", c.check(broken(lambda x: x["effects"][pal]["defaults"].update(palette=255))),
       "defaults.palette must be default")
expect("defaults.palette 255 for own colours", c.check(broken(lambda x: x["effects"][own]["defaults"].update(palette=0))),
       "defaults.palette must be default")
expect("palette_slots are colour slot labels", c.check(broken(lambda x: x["effects"][own].update(palette_slots=["Nope"]))),
       "palette_slots")
expect("a palette note", c.check(broken(lambda x: x["effects"][own].update(palette_note="the palette"))), "palette_note")
expect("an origin for the licence notices", c.check(broken(lambda x: x["effects"][0].pop("origin"))), "origin")
expect("a default speed over 255", c.check(broken(lambda x: x["effects"][0]["defaults"].update(speed=300))), "defaults.speed")
expect("a default palette not in the catalogue", c.check(broken(lambda x: (x["effects"][pal].update(default_palette_id=99),
       x["effects"][pal]["defaults"].update(palette=99)))), "not in the catalogue")
expect("colors 0-3", c.check(broken(lambda x: x["effects"][0].update(colors=4))), "colors must be 0-3")
expect("colors = the slots", c.check(broken(lambda x: x["effects"][0].update(colors=2))), "not the number of color_slots")
expect("a label exactly when the slider is used", c.check(broken(lambda x: x["effects"][2].update(speed_label=None))), "disagree")
expect("a version over 65535", c.check(broken(lambda x: x.update(catalogue_version=70000))), "catalogue_version")
expect("an unknown key", c.check(broken(lambda x: x["effects"][0].update(colour=1))), "unknown keys")
expect("an empty label", c.check(broken(lambda x: x["effects"][2].update(speed_label=""))), "speed_label must be")
expect("no summary", c.check(broken(lambda x: x["effects"][2].update(summary=""))), "summary")
expect("a wrong palette_special", c.check(broken(lambda x: x.update(palette_special={"colors": 1, "default": 2}))), "palette_special")
prev = copy.deepcopy(CAT)
expect("an effect removed", c.check(broken(lambda x: x.update(effects=x["effects"][1:])), prev), "removed")
expect("a palette removed", c.check(broken(lambda x: x.update(palettes=x["palettes"][1:])), prev), "removed")
expect("an effect renamed", c.check(broken(lambda x: x["effects"][0].update(name="Plain")), prev), "renamed")
expect("a change without a new version", c.check(broken(lambda x: x["effects"][0].update(summary="changed")), prev),
       "raise catalogue_version")


def retire_and_bump(x):
    x["effects"][0]["retired"] = True
    x["catalogue_version"] += 1


expect("retired + a new version is fine", c.check(broken(retire_and_bump), prev))
print(f"{'FAILED' if failures else 'ALL CATALOGUE TESTS PASSED'} ({failures} failures)")
sys.exit(1 if failures else 0)
