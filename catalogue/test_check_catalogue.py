#!/usr/bin/env python3
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
expect("palette must be a bool", c.check(broken(lambda x: x["effects"][0].update(palette="yes"))), "palette must be")
expect("an origin for the licence notices", c.check(broken(lambda x: x["effects"][0].pop("origin"))), "origin")
expect("a default speed over 255", c.check(broken(lambda x: x["effects"][0]["defaults"].update(speed=300))), "defaults.speed")
expect("a default palette not in the catalogue", c.check(broken(lambda x: x["effects"][0]["defaults"].update(palette=99))),
       "not in the catalogue")
expect("the special palettes are fine as defaults", c.check(broken(lambda x: x["effects"][0]["defaults"].update(palette=254))))
expect("at most 3 colours", c.check(broken(lambda x: x["effects"][0].update(colors=["a", "b", "c", "d"]))), "at most 3")
expect("a version over 65535", c.check(broken(lambda x: x.update(catalogue_version=70000))), "catalogue_version")
expect("an unknown key", c.check(broken(lambda x: x["effects"][0].update(colour=1))), "unknown keys")
expect("an empty label", c.check(broken(lambda x: x["effects"][2].update(speed=""))), "speed must be")
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
