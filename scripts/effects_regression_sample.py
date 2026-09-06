#!/usr/bin/env python3
"""effects_regression_sample.py -- the pixel side of scripts/effects-regression.sh.

Knows the layout tests/effects_scene_client.c paints (change one, change
both) and samples a `gamescopectl screenshot "<path> 4"` capture by REGION:
the mean grey of the interior of each flat band and rectangle. Every check
below is a statement about those region values, printed as one line
    STATUS<TAB>name<TAB>detail
exactly like pixel_regression_sample.py, so the shell script's results table
treats both the same way.

Subcommands
    regions <image> <scene>              print every region's value (informational)
    check   <image> <scene> <what>       assert; `what` is off | dynamic
    temporal <settled> <t1> <t2> <t3> <band>
                                         the middle-band values across a scene switch
                                         must approach the settled value monotonically
"""
import sys

try:
    from PIL import Image
except ImportError:
    print("FAIL\tsetup\tpython3 PIL (Pillow) not importable", file=sys.stderr)
    sys.exit(2)

W, H = 1280, 720

SCENES = {
    # bands top->bottom, rectangle value, rect size, black corner?
    "dark":   dict(bands=[5, 8, 12, 16, 20], rect=240, rw=40, rh=40, black=True),
    "bright": dict(bands=[200, 215, 230, 245, 255], rect=30, rw=120, rh=50, black=False),
    "mid":    dict(bands=[26, 77, 128, 179, 230], rect=None, rw=0, rh=0, black=False),
}


def emit(ok, name, detail):
    print(f"{'PASS' if ok else 'FAIL'}\t{name}\t{detail}")
    return ok


def load(path):
    try:
        return Image.open(path).convert("RGB")
    except Exception as e:  # noqa: BLE001
        print(f"FAIL\tload\t{path}: {e}", file=sys.stderr)
        sys.exit(2)


def region_mean(img, box, inset=4):
    x0, y0, x1, y1 = box
    x0, y0, x1, y1 = x0 + inset, y0 + inset, x1 - inset, y1 - inset
    px = list(img.crop((x0, y0, x1, y1)).getdata())
    n = len(px)
    return tuple(sum(p[c] for p in px) / n for c in range(3))


def grey(rgb):
    return sum(rgb) / 3.0


def regions(img, scene):
    """Every region of `scene` as name -> mean grey, sampled on the right
    edge of each band (x 1150..1270, clear of the rectangles, which span
    x <= 1110) and the interior of the rectangles / black corner."""
    sc = SCENES[scene]
    sx, sy = img.width / W, img.height / H
    out = {}
    for i in range(5):
        y0, y1 = int(i * img.height / 5), int((i + 1) * img.height / 5)
        out[f"band{i}"] = grey(region_mean(img, (int(1150 * sx), y0, int(1270 * sx), y1)))
    if sc["rect"] is not None:
        rw, rh = int(sc["rw"] * sx), int(sc["rh"] * sy)
        cy = img.height // 2
        vals = []
        for k in range(6):
            cx = int((100 + k * 190) * sx)
            vals.append(grey(region_mean(img, (cx - rw // 2, cy - rh // 2, cx + rw // 2, cy + rh // 2), inset=3)))
        out["rect"] = sum(vals) / len(vals)
    if sc["black"]:
        out["black"] = grey(region_mean(img, (int(40 * sx), int(40 * sy), int(200 * sx), int(120 * sy))))
    return out


def fmt(vals):
    return " ".join(f"{k}={v:.1f}" for k, v in vals.items())


def cmd_regions(args):
    image, scene = args
    print(f"INFO\t{scene}\t{fmt(regions(load(image), scene))}")


def cmd_check(args):
    image, scene, what = args
    sc = SCENES[scene]
    v = regions(load(image), scene)
    name = f"{scene}-{what}"
    ok = True

    if what == "off":
        # The effect off must be the identity to within capture rounding.
        worst = max(abs(v[f"band{i}"] - sc["bands"][i]) for i in range(5))
        if sc["rect"] is not None:
            worst = max(worst, abs(v["rect"] - sc["rect"]))
        ok = worst <= 2.0
        return sys.exit(0 if emit(ok, name, f"identity, worst deviation {worst:.1f} counts; {fmt(v)}") else 1)

    if what != "dynamic":
        print(f"FAIL\t{name}\tunknown check '{what}'", file=sys.stderr)
        sys.exit(2)

    if scene == "dark":
        # p2 (the darkest band, 5) lifted to something readable; the 240
        # highlights not blown out and still above every band; black at 0.
        checks = [
            ("band0 (5) lifted >= 30", v["band0"] >= 30.0),
            ("bands keep their order", v["band0"] < v["band1"] < v["band2"] < v["band3"] < v["band4"]),
            ("240 highlights < 255", v["rect"] < 254.5),
            ("240 highlights above band4", v["rect"] > v["band4"]),
            ("black stays <= 2", v["black"] <= 2.0),
        ]
    elif scene == "bright":
        # p98 (band 3, 245, is the 98th percentile band; band 4 is white)
        # brought down; the 30 shadows dimmed but not crushed; order kept.
        checks = [
            ("band3 (245) brought below 250 - actually below 235", v["band3"] < 235.0),
            ("band4 (255) not above 255", v["band4"] <= 255.0),
            ("30 shadows not crushed: >= 8", v["rect"] >= 8.0),
            ("30 shadows dimmed no further than 30 * min_gain 0.5", v["rect"] >= 14.0),
            ("bands keep their order", v["band0"] < v["band1"] < v["band2"] < v["band3"] <= v["band4"]),
        ]
    elif scene == "mid":
        worst = max(abs(v[f"band{i}"] - sc["bands"][i]) for i in range(5))
        checks = [("near-identity, worst deviation <= 6 counts", worst <= 6.0)]
        v["worst"] = worst
    else:
        sys.exit(2)

    failed = [c for c, ok_ in checks if not ok_]
    ok = not failed
    sys.exit(0 if emit(ok, name, ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(v)) else 1)


def cmd_temporal(args):
    settled, t1, t2, t3, band = args
    settled, t1, t2, t3 = (float(x) for x in (settled, t1, t2, t3))
    # Values must move toward the settled one without overshooting past it
    # (no oscillation), and the 3 s capture must be nearly there.
    d = [abs(t - settled) for t in (t1, t2, t3)]
    mono = d[0] >= d[1] - 0.5 >= d[2] - 1.0
    # sign never flips: all on the same side of settled (or at it)
    sides = [(t - settled) for t in (t1, t2, t3)]
    same_side = all(s >= -1.0 for s in sides) or all(s <= 1.0 for s in sides)
    close = d[2] <= 12.0
    ok = mono and same_side and close
    sys.exit(0 if emit(ok, f"temporal-{band}",
                       f"{band}: 0.2s={t1:.1f} 1s={t2:.1f} 3s={t3:.1f} settled={settled:.1f}; "
                       f"monotone={mono} no-overshoot={same_side} within-12-at-3s={close}") else 1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd, args = sys.argv[1], sys.argv[2:]
    {"regions": cmd_regions, "check": cmd_check, "temporal": cmd_temporal}[cmd](args)


if __name__ == "__main__":
    main()
