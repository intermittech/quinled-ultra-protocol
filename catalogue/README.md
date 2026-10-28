# Pixel effect catalogue

`pixel_fx_catalogue.json` lists the effects and palettes the [Diff-Power Ultra Receiver Digital-Medium](https://github.com/intermittech/quinled-ultra-firmware/wiki/Board-Diff-Power-UR-Digital-M) receiver runs itself
(the PIXEL_FX command, wire 3, descriptor `LOCAL_FX` capability). Analog receivers use host-driven SET transitions; this catalogue applies only to pixels.
A host builds its pixel effect list and its palette choices
from it; the receiver's page 0x11 (parts 1 and 2) says which of them its firmware has.

- **Ids are never reused.** 1-127 are the official effects, 128-255 custom ones; palettes 0-127, `0xFE` is
  colour 1 → 2 → 3 and `0xFF` Effect default (both implicit). An effect or palette that goes away is marked
  `"retired": true`, never removed or renamed.
- **`catalogue_version`** (a u16, also in page 0x11 part 0) goes up whenever the content changes.
- **Per effect:** its name (the name Home Assistant shows), a summary, the labels of the Speed and Intensity sliders it
  uses (`speed` / `intensity` true or false, with `speed_label` / `intensity_label`), its Effect default
  (`default_palette_id`, or `"default_palette": "own"`: its own colours; every effect takes a palette), the colour slots it
  still uses while a palette is chosen (`palette_slots`) and what the palette colours (`palette_note`), how
  many colours it uses (`colors`) with their labels by position (`color_slots`), its two extra sliders (`null` or a
  label), the defaults, the state bytes per pixel it needs and where the algorithm is from (for the licence notices).
- The file is written by the receiver firmware's effect engine (`tools/gen_fx_tables.py` in
  [quinled-ultra-firmware](https://github.com/intermittech/quinled-ultra-firmware)); this repository holds the copy
  hosts take, and the ESPHome component keeps a byte-identical copy of it.

```
python catalogue/check_catalogue.py catalogue/pixel_fx_catalogue.json [--against previous.json]
python catalogue/test_check_catalogue.py
```

`--against` is the release gate: no id removed or renamed, a changed catalogue has a higher version.
