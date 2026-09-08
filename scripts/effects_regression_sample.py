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
    split   <image> <what>               halfsplit: the two halves' bands; `what` is
                                         off (identity) or info (print only)
    splitcmp <image-off> <image-on>      halfsplit with Local adaptation 0 % vs 100 %:
                                         the dark half must lift and the bright half
                                         must not blow out -- the headline check
    halo    <image> <scene> <what>       halobox/haloinv: the line profile out from the
                                         box's edge; `what` is off (flat) or on (a
                                         monotone ramp, bounded amplitude -- no ring)
    temporal <settled> <t1> <t2> <t3> <band>
                                         the middle-band values across a scene switch
                                         must approach the settled value monotonically
    ablog   <file> <what>                per-frame `effects_ab_log` lines (gamescope's
                                         console log); `what` is static | pan | transition
                                         | panlocal | agstatic | agpan | bind (the last
                                         three are Adaptive Gamma's, 2026-09-08: the same
                                         stability bars applied to its ONE exponent, plus
                                         an INFO line naming the binding limit)
    noclip  <image> <scene> <label>      Adaptive Gamma's no-clipping property: bands stay
                                         ordered and apart, near-white highlights stay
                                         below white, black stays black
    means   <label> <img...>             INFO: each capture's frame mean, in order
    colorcheck <image> <effect> <strength>
                                         the "colors" scene (2026-09-08): pins one
                                         effect's per-band output against the closed-
                                         form formula; `effect` is off | saturation |
                                         vibrancy
    colorshape <image-saturation> <image-vibrancy>
                                         the same "colors" capture under Saturation and
                                         under Vibrancy at the same nominal strength:
                                         Saturation's per-band boost RATIO must be flat
                                         across bands, Vibrancy's must strictly increase
                                         -- the headline shape difference between the two
"""
import re
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


# ---- The Local-adaptation scenes (2026-09-07) -----------------------------
#
# halfsplit: the left half is the dark scene's five bands, the right half the
# bright scene's. Sampled well inside each half (8..30 % and 70..92 % of the
# width) so the operator's deliberately wide transition across the seam is
# never what a "half" number reports.
SPLIT_LEFT = [5, 8, 12, 16, 20]
SPLIT_RIGHT = [200, 215, 230, 245, 255]

# halobox / haloinv: a 320x320 box centred in the 1280x720 reference frame,
# so its right edge is at x = 800. The profile walks out from that edge along
# the box's own centre line.
HALO_BOX_W, HALO_BOX_H = 320, 320
HALO_EDGE_X = 1280 // 2 + HALO_BOX_W // 2
HALO_DISTANCES = [12, 24, 48, 96, 160, 240, 360, 460]

# ---- The "colors" scene (2026-09-08, Saturation/Vibrancy split) -----------
#
# Five horizontal RGB bands -- mirrors tests/effects_scene_client.c's
# kColorBands exactly; change one, change both. Band 0 is pure grey
# (saturation 0); 1-4 walk a warm hue from near-neutral to fully saturated
# (max(c)-min(c) = 0, 28, 86, 160, 255).
COLOR_BANDS = [
    (128, 128, 128),
    (148, 134, 120),
    (178, 140,  92),
    (214, 118,  54),
    (255,  60,   0),
]
HALO_FIELDS = {"halobox": 200, "haloinv": 15}


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
            # min_gain 0.3 (widened from 0.5, 2026-09-07 request): the floor
            # is 30 * 0.3 = 9.0, down from 30 * 0.5 = 15.0 -- effects_curve.h
            # math gives 9.05, so 8.0 leaves ~1 count of capture-rounding
            # room the same way the old 14.0 did for a 15.0 target.
            ("30 shadows dimmed no further than 30 * min_gain 0.3", v["rect"] >= 8.0),
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


def split_regions(img):
    """halfsplit: band i of each half, as name -> mean grey."""
    sx = img.width / W
    out = {}
    for i in range(5):
        y0, y1 = int(i * img.height / 5), int((i + 1) * img.height / 5)
        out[f"L{i}"] = grey(region_mean(img, (int(0.08 * W * sx), y0, int(0.30 * W * sx), y1), inset=6))
        out[f"R{i}"] = grey(region_mean(img, (int(0.70 * W * sx), y0, int(0.92 * W * sx), y1), inset=6))
    return out


def cmd_split(args):
    image, what = args
    v = split_regions(load(image))
    if what == "off":
        worst = max(max(abs(v[f"L{i}"] - SPLIT_LEFT[i]), abs(v[f"R{i}"] - SPLIT_RIGHT[i])) for i in range(5))
        sys.exit(0 if emit(worst <= 2.0, "halfsplit-off",
                           f"identity, worst deviation {worst:.1f} counts; {fmt(v)}") else 1)
    print(f"INFO\thalfsplit-{what}\t{fmt(v)}")


def cmd_splitcmp(args):
    """The headline case, as a comparison rather than an absolute threshold:
    the SAME frame, Dynamic, with Local adaptation at 0 % and at 100 %.

    A global curve has one gain for a frame that is half 5..20 and half
    200..255; it can serve one half or the other. The local operator must
    make the dark half readable WITHOUT pushing the bright half further up
    (it should in fact bring it down a little, since that half's own
    neighbourhood is brighter than the frame mean)."""
    off_img, on_img = load(args[0]), load(args[1])
    off, on = split_regions(off_img), split_regions(on_img)
    d_lift = [on[f"L{i}"] - off[f"L{i}"] for i in range(5)]
    d_bright = [on[f"R{i}"] - off[f"R{i}"] for i in range(5)]
    checks = [
        ("the dark half is lifted at every band", all(d > 0.5 for d in d_lift)),
        # Relative, not absolute: the point is "further than the global curve
        # managed", and how far depends on the strength the caller chose.
        # Measured 12 -> 18 at the 50 % default and 12 -> 28 at 100 %.
        ("the dark half's darkest band lifts by >= 25 %", on["L0"] >= off["L0"] * 1.25),
        ("the dark half keeps its order", on["L0"] < on["L1"] < on["L2"] < on["L3"] < on["L4"]),
        ("the bright half is not pushed up", all(d <= 1.0 for d in d_bright)),
        ("the bright half's 245 band stays off the ceiling", on["R3"] < 250.0),
        ("the bright half keeps its order", on["R0"] < on["R1"] < on["R2"] < on["R3"] <= on["R4"]),
        # The OTHER failure mode of a local operator, and the reason 100 % is
        # not the default: pushing every neighbourhood at its own target
        # flattens the contrast inside each one. Measured across the bright
        # half's five bands: 40 counts under the global curve, 22 at the 50 %
        # default, 8 at 100 %. Below ~6 the half has stopped being a picture.
        ("the bright half keeps >= 6 counts of internal contrast", on["R4"] - on["R0"] >= 6.0),
    ]
    failed = [c for c, ok_ in checks if not ok_]
    detail = ("FAILED: " + "; ".join(failed) + "; " if failed else "") + \
        "off " + " ".join(f"L{i}={off[f'L{i}']:.0f}/R{i}={off[f'R{i}']:.0f}" for i in range(5)) + \
        " | on " + " ".join(f"L{i}={on[f'L{i}']:.0f}/R{i}={on[f'R{i}']:.0f}" for i in range(5)) + \
        " | lift " + " ".join(f"{d:+.0f}" for d in d_lift) + \
        " bright " + " ".join(f"{d:+.0f}" for d in d_bright)
    sys.exit(0 if emit(not failed, "split-local", detail) else 1)


def halo_profile(img):
    """The field's value at each HALO_DISTANCES step out from the box's right
    edge, along the box's centre line."""
    sx, sy = img.width / W, img.height / H
    cy = img.height // 2
    out = []
    for d in HALO_DISTANCES:
        x = int((HALO_EDGE_X + d) * sx)
        out.append(grey(region_mean(img, (x - 5, cy - 9, x + 5, cy + 9), inset=1)))
    return out


def cmd_halo(args):
    image, scene, what = args[0], args[1], args[2]
    max_amp = float(args[3]) if len(args) > 3 else 12.0
    img = load(image)
    prof = halo_profile(img)
    sx, sy = img.width / W, img.height / H
    box = grey(region_mean(img, (img.width // 2 - int(40 * sx), img.height // 2 - int(40 * sy),
                                 img.width // 2 + int(40 * sx), img.height // 2 + int(40 * sy))))
    far = prof[-1]
    amp = prof[0] - far
    # A local tone operator's artefact is a RING: the field brightening (or
    # darkening) as it approaches the object, then coming back. A ring needs
    # a turning point; a plain ramp has none. The map is a non-negative blur
    # of a step, so a ramp is what the maths predicts and a turning point
    # would mean something is wrong -- this is the check that says so.
    diffs = [b - a for a, b in zip(prof, prof[1:])]
    signs = [1 if d > 0.75 else (-1 if d < -0.75 else 0) for d in diffs]
    nz = [x for x in signs if x != 0]
    monotone = all(x == nz[0] for x in nz) if nz else True
    body = (f"box={box:.1f} far={far:.1f} amp={amp:+.1f} counts; "
            + " ".join(f"d{d}={v:.1f}" for d, v in zip(HALO_DISTANCES, prof)))
    if what == "off":
        # Local adaptation at 0 %: a flat field must come out flat. This is
        # the control -- it proves the profile machinery, the capture and the
        # global curve introduce no gradient of their own.
        flat = max(prof) - min(prof)
        sys.exit(0 if emit(flat <= 1.5, f"halo-{scene}-off",
                           f"flat field stays flat, spread {flat:.1f} counts; {body}") else 1)
    # The two things that matter, and they are different things.
    #
    # NO RING is the hard property, checked at every strength: the map is a
    # non-negative blur of the image, so a step in the image can only become
    # a monotone ramp in the map -- an overshoot here would mean the operator
    # is doing something it must never do. This is the check that would catch
    # a sharpening or edge-aware "improvement" reintroducing a rim.
    #
    # AMPLITUDE is the soft one, and its budget is per capture (the caller
    # passes it): measured on this worst-case flat-field hard edge, +8 counts
    # at the 50 % default and +15 at 100 %, against +57 before the blur was
    # widened to sigma ~4.7 cells. See shader-effects.md's halo table.
    checks = [("no ring (the profile is monotone out from the edge)", monotone),
              (f"halo amplitude within {max_amp:.0f} counts", abs(amp) <= max_amp)]
    failed = [c for c, ok_ in checks if not ok_]
    sys.exit(0 if emit(not failed, f"halo-{scene}-on",
                       ("FAILED: " + "; ".join(failed) + "; " if failed else "")
                       + f"monotone={monotone} " + body) else 1)


def cmd_noclip(args):
    """noclip <image> <scene> <label> -- ADAPTIVE GAMMA's headline property,
    measured rather than argued: an exponent on 0..1 has 0 and 1 as exact
    fixed points and is strictly increasing, so it cannot clip and cannot
    flatten the highlights, at ANY setting. Stated as three things a capture
    can show:

      * every band strictly ordered and at least 2 counts apart -- nothing
        has been compressed into its neighbour;
      * the highlight region (the `dark` scene's 240 squares) stays BELOW
        white, i.e. a value that was distinguishable from white still is
        (Adaptive Brightness's Whole image mode drives exactly this region
        to a clipped 255 on this scene -- see shader-effects.md's table);
      * on the `bright` scene, whose top band IS 255, the 245 band stays
        strictly below it, which is the same statement where the input
        already touches the ceiling.
    """
    image, scene, label = args
    vals = regions(load(image), scene)
    bands = [vals[f"band{i}"] for i in range(5)]
    checks = []
    for i in range(4):
        checks.append((f"band{i} < band{i + 1} by >= 2 counts "
                       f"({bands[i]:.1f} vs {bands[i + 1]:.1f})",
                       bands[i + 1] - bands[i] >= 2.0))
    if "rect" in vals:
        # The rectangles mean different things per scene, so the assertion
        # has to follow the INPUT: `dark`'s are 240 (highlights, brighter
        # than every band), `bright`'s are 30 (shadows, darker than every
        # band). Only the highlight case is a no-clip statement.
        sc = SCENES[scene]
        if sc["rect"] > max(sc["bands"]):
            checks.append((f"the {sc['rect']} highlights stay below white ({vals['rect']:.1f} < 254)",
                           vals["rect"] < 254.0))
            checks.append((f"the {sc['rect']} highlights stay above every band "
                           f"({vals['rect']:.1f} > {bands[4]:.1f})", vals["rect"] > bands[4]))
        else:
            # A darkening exponent has no shadow cap here (that is Adaptive
            # Brightness's min_gain, which this effect does not have), so a
            # deep shadow CAN be pushed to black at a high Max darken -- that
            # is a documented trade-off, not a clip. What must still hold is
            # the ordering: the shadows stay below the darkest band.
            checks.append((f"the {sc['rect']} shadows stay below every band "
                           f"({vals['rect']:.1f} < {bands[0]:.1f})", vals["rect"] < bands[0]))
    if scene == "bright":
        checks.append((f"the 245 band stays below the 255 band "
                       f"({bands[3]:.1f} < {bands[4]:.1f})", bands[3] < bands[4] - 1.0))
    if "black" in vals:
        checks.append((f"pure black stays black ({vals['black']:.1f} <= 2)", vals["black"] <= 2.0))

    failed = [c for c, ok in checks if not ok]
    detail = ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(vals)
    sys.exit(0 if emit(not failed, f"noclip-{scene}-{label}", detail) else 1)


def frame_mean(path):
    """The whole frame's mean grey, at a 160x90 downsample. The cheapest
    honest "did the picture move" measure there is: it is what a person sees
    change when a brightness control works, it does not depend on any scene's
    band geometry, and it is monotone in every knob these checks sweep."""
    img = load(path).resize((160, 90), Image.BILINEAR)
    px = list(img.getdata())
    return sum(grey(p) for p in px) / len(px)


def cmd_means(args):
    """means <label> <img...> -- INFO: each capture's frame mean, in order.
    For showing what a state change did to the picture where the statement
    worth making is "these are different pictures" rather than a threshold."""
    label = args[0]
    vals = [frame_mean(p) for p in args[1:]]
    print(f"INFO\t{label}\tframe mean " + " | ".join(f"{v:.1f}" for v in vals))
    sys.exit(0)


def cmd_slider(args):
    """slider <label> <min-step> <img...> -- the check that pins the
    2026-09-08 report: *"anything above target brightness 0.5 and max gain
    2.0 does [not do] anything at all"*. The images are one knob swept in
    increasing order; every adjacent step must brighten the frame by at
    least <min-step> counts. Before the fix, the pairs this is run on were
    bit-identical (a step of 0.00), which is exactly what an inert slider
    looks like from the outside.

    A NEGATIVE <min-step> means the knob is expected to DARKEN the picture
    (Adaptive Gamma's Max darken, 2026-09-08): every step must then be at
    most that, i.e. at least |min-step| counts downwards. Same statement,
    the other way up -- an inert slider is still a step of 0.00."""
    label, step = args[0], float(args[1])
    vals = [frame_mean(p) for p in args[2:]]
    steps = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    ok = all(d <= step for d in steps) if step < 0 else all(d >= step for d in steps)
    emit(ok, label,
         "frame mean " + " -> ".join(f"{v:.1f}" for v in vals)
         + "; steps " + " ".join(f"{d:+.1f}" for d in steps)
         + f" (each must be {'<=' if step < 0 else '>='} {step:.1f})")


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


# ---- effects_ab_log traces (2026-09-07, "the whole image pulsates") ----
#
# One line per composite, printed by gamescope's `effects_ab_log` command
# (src/rendervulkan.cpp): the raw and smoothed statistics, the gain and
# gamma the pixel pass used, and one probe pixel of the graded output.

AB_LOG_RE = re.compile(
    r"ab_log n=(\d+) t=([\d.]+) dt=([\d.]+) (\w+) "
    r"raw mean=([\d.]+) p2=([\d.]+) p50=([\d.]+) p98=([\d.]+) "
    r"smooth mean=([\d.]+) p2=([\d.]+) p50=([\d.]+) p98=([\d.]+) "
    r"gain=([\d.]+) gamma=([\d.]+) px\((\d+),(\d+)\)=(\d+),(\d+),(\d+)"
    r"(?: local=([\d.]+) lmin=([\d.]+) lmax=([\d.]+) lprobe=([\d.]+) gainlo=([\d.]+) gainhi=([\d.]+))?"
    r"(?: bind=(\d+) \(([^)]*)\))?")


def parse_ablog(path):
    rows = []
    for line in open(path):
        m = AB_LOG_RE.search(line)
        if m:
            g = m.groups()
            row = dict(n=int(g[0]), t=float(g[1]), mode=g[3], rp98=float(g[7]), p50=float(g[10]),
                       p98=float(g[11]), gain=float(g[12]), gamma=float(g[13]),
                       px=(int(g[16]) + int(g[17]) + int(g[18])) / 3.0)
            # The binding readout (2026-09-08). Optional for the same reason
            # the local fields are: an older binary's line does not carry it.
            row.update(bind=int(g[25]) if g[25] else -1, bindtext=g[26] or "")
            # The Local-adaptation fields (2026-09-07). Absent on a line from
            # a binary predating them, so every consumer must tolerate that.
            row.update(local=float(g[19]) if g[19] else 0.0,
                       lmin=float(g[20]) if g[20] else 0.0,
                       lmax=float(g[21]) if g[21] else 0.0,
                       gainlo=float(g[23]) if g[23] else row["gain"],
                       gainhi=float(g[24]) if g[24] else row["gain"])
            rows.append(row)
    return rows


def p2p(rows, key):
    v = [r[key] for r in rows]
    return max(v) - min(v)


def cmd_ablog(args):
    path, what = args
    rows = parse_ablog(path)
    name = f"stability-{what}" if what != "transition" else "transition-gain"
    if what == "static" and rows and rows[-1]["local"] > 0.0:
        name = f"stability-static-local{rows[-1]['local']:.2f}"
    # `bind` is a one-line INFO readout of the frame's own classifier, so it
    # is armed for a handful of composites, not three hundred.
    if len(rows) < (100 if what not in ("panlocal", "bind") else (50 if what == "panlocal" else 5)):
        sys.exit(0 if emit(False, name, f"only {len(rows)} ab_log lines in {path}") else 1)

    if what == "static":
        # A perfectly still frame: the measurement is deterministic, so the
        # RAW statistics must not move at all -- zero, not "a little" (a
        # feedback loop or a non-deterministic estimator would show here
        # first). The smoothed values are still finishing the EMA's last
        # fraction of a percent after the pan stopped 5 s earlier, so they
        # get a tolerance far below one code, and the output pixel must
        # not change.
        d = dict(rp98=p2p(rows, "rp98"), gain=p2p(rows, "gain"), p98=p2p(rows, "p98") * 255.0, px=p2p(rows, "px"))
        ok = d["rp98"] <= 0.0 and d["gain"] <= 0.002 and d["p98"] <= 0.1 and d["px"] <= 0.0
        last = rows[-1]
        detail = (f"{len(rows)} frames: raw p98 p2p={d['rp98']:.6f} (must be 0), gain p2p={d['gain']:.6f} (<= 0.002), "
                  f"smoothed p98 p2p={d['p98']:.4f} codes (<= 0.1), px p2p={d['px']:.1f} (must be 0); "
                  f"gain={last['gain']:.4f} px={last['px']:.0f}; local={last['local']:.2f} "
                  f"map {last['lmin'] * 255:.1f}..{last['lmax'] * 255:.1f} codes "
                  f"gain {last['gainlo']:.3f}..{last['gainhi']:.3f}")
    elif what == "pan":
        # The same texture panning under the tap grid with its true
        # statistics fixed (--periodic): everything that moves is sampling
        # noise, and after the EMA it must stay far below what the eye
        # picks up. The raw 98th-percentile spread is reported so a
        # regression of the estimator itself (the rank-cut cliff: 100+
        # codes raw) is visible even before the EMA hides it.
        # Thresholds: measured 0.013..0.033 / 0.6..1.5 codes / 11 codes over
        # several 300-frame runs after the fix (part of it the EMA's last
        # percent of convergence from the previous scene); the rank-cut
        # estimator read 0.81 / 16.4 codes / 107 codes on this scene.
        d = dict(gain=p2p(rows, "gain"), p98=p2p(rows, "p98") * 255.0, rp98=p2p(rows, "rp98") * 255.0)
        ok = d["gain"] <= 0.06 and d["p98"] <= 3.0 and d["rp98"] <= 40.0
        detail = (f"{len(rows)} frames: gain p2p={d['gain']:.4f} (<= 0.06), "
                  f"smoothed p98 p2p={d['p98']:.2f} codes (<= 3), raw p98 p2p={d['rp98']:.1f} codes (<= 40); "
                  f"gain={rows[-1]['gain']:.4f}")
    elif what == "panlocal":
        # Local adaptation under a pan, reported rather than asserted. A
        # panning scene genuinely CHANGES each cell's own content even when
        # the frame's statistics do not (--periodic fixes the histogram, not
        # the layout), so the probe cell's gain is SUPPOSED to move here --
        # that is the operator working, not a pulse. What must not move is a
        # still frame, which is what `static` asserts at every local
        # strength. So this prints the numbers and judges nothing.
        d = dict(gain=p2p(rows, "gain"), lmin=p2p(rows, "lmin") * 255.0, lmax=p2p(rows, "lmax") * 255.0)
        last = rows[-1]
        print(f"INFO\tlocal-pan-{last['local']:.2f}\t{len(rows)} frames: gain p2p={d['gain']:.4f}, "
              f"map lmin p2p={d['lmin']:.2f} codes lmax p2p={d['lmax']:.2f} codes; "
              f"map {last['lmin'] * 255:.1f}..{last['lmax'] * 255:.1f} codes, "
              f"gain {last['gainlo']:.3f}..{last['gainhi']:.3f} (probe {last['gain']:.3f})")
        sys.exit(0)
    elif what in ("agstatic", "agpan"):
        # ADAPTIVE GAMMA's stability, held to the standard the 2026-09-07
        # pulse fix set. The effect has no gain -- its whole state is the one
        # exponent -- so `gamma` is what must not move here, where the
        # Adaptive Brightness checks above watch `gain`. Everything else is
        # identical, deliberately: the same still frame, the same panning
        # --periodic frame, the same estimator underneath, so a regression
        # in the shared measure pass fails both effects' checks at once.
        d = dict(rp98=p2p(rows, "rp98"), gamma=p2p(rows, "gamma"),
                 p98=p2p(rows, "p98") * 255.0, px=p2p(rows, "px"))
        last = rows[-1]
        if what == "agstatic":
            name = f"ag-stability-static-local{last['local']:.2f}"
            ok = d["rp98"] <= 0.0 and d["gamma"] <= 0.002 and d["p98"] <= 0.1 and d["px"] <= 0.0
            bounds = "(raw must be 0, gamma <= 0.002, smoothed p98 <= 0.1 codes, px must be 0)"
        else:
            # A pan with --periodic: the frame's true statistics never
            # change, so everything that moves is sampling noise. Same
            # thresholds the Adaptive Brightness `pan` check uses, with the
            # exponent standing in for the gain (the exponent's own scale is
            # smaller, so this is if anything the tighter bar).
            name = f"ag-stability-pan-local{last['local']:.2f}"
            ok = d["gamma"] <= 0.06 and d["p98"] <= 3.0 and d["rp98"] * 255.0 <= 40.0
            bounds = "(gamma <= 0.06, smoothed p98 <= 3 codes, raw p98 <= 40 codes)"
            d["rp98"] *= 255.0
        detail = (f"{len(rows)} frames, mode={last['mode']}: raw p98 p2p={d['rp98']:.6f}, "
                  f"gamma p2p={d['gamma']:.6f}, smoothed p98 p2p={d['p98']:.4f} codes, "
                  f"px p2p={d['px']:.1f} {bounds}; gamma={last['gamma']:.4f} px={last['px']:.0f}; "
                  f"local={last['local']:.2f} map {last['lmin'] * 255:.1f}..{last['lmax'] * 255:.1f} codes "
                  f"exponent {last['gainlo']:.3f}..{last['gainhi']:.3f}; bind={last['bindtext']}")
    elif what == "bind":
        # INFO only: WHICH LIMIT the frame's own classifier says is binding,
        # in the panel's exact words (effects_curve.h names them once and
        # both surfaces print that one string). This is how a sweep's "where
        # does it stop, and does the UI say so" question becomes a line in
        # results.txt rather than an opinion.
        last = rows[-1]
        print(f"INFO\tbind-{last['mode']}\t{len(rows)} frames, last: gain={last['gain']:.4f} "
              f"gamma={last['gamma']:.4f} px={last['px']:.0f} "
              f"bind={last['bind']} ({last['bindtext']})")
        sys.exit(0)
    elif what == "transition":
        # Armed just before the dark -> bright switch, so the trace starts
        # with a few dark frames: the switch is the first frame whose raw
        # p98 has jumped, and the probe pixel (the 230 band) first leaps to
        # white under the still-dark gain. From there the gain and the
        # pixel must come down without ever going below where they settle
        # (no overshoot); settling times are measured from the switch.
        i0 = next((i for i, r in enumerate(rows) if abs(r["rp98"] - rows[0]["rp98"]) > 0.1), 0)
        t0 = rows[i0]["t"]
        rows = rows[i0:]
        gains = [r["gain"] for r in rows]
        pxs = [r["px"] for r in rows]
        settled_g, settled_px = gains[-1], pxs[-1]
        mono_g = all(b <= a + 1e-4 for a, b in zip(gains, gains[1:]))
        mono_px = all(b <= a + 1.0 for a, b in zip(pxs, pxs[1:]))
        no_over_g = min(gains) >= settled_g - 1e-3
        no_over_px = min(pxs) >= settled_px - 1.0
        span = abs(gains[0] - settled_g)
        t_settle = next((r["t"] - t0 for r in rows if abs(r["gain"] - settled_g) <= 0.05 * span), None)
        px_span = abs(pxs[0] - settled_px)
        t_settle_px = next((r["t"] - t0 for r in rows if abs(r["px"] - settled_px) <= max(0.05 * px_span, 1.0)), None)
        ok = mono_g and mono_px and no_over_g and no_over_px and t_settle is not None and t_settle <= 4000.0
        detail = (f"{len(rows)} frames: gain {gains[0]:.3f} -> {settled_g:.3f}, monotone={mono_g} "
                  f"no-overshoot={no_over_g}, within 5% at {t_settle if t_settle is None else round(t_settle)} ms (<= 4000); "
                  f"px {pxs[0]:.0f} -> {settled_px:.0f}, monotone={mono_px} no-overshoot={no_over_px}, "
                  f"within 5% at {t_settle_px if t_settle_px is None else round(t_settle_px)} ms")
    else:
        sys.exit(2)
    sys.exit(0 if emit(ok, name, detail) else 1)


# ---- "colors" scene: Saturation and Vibrancy, pinned against the closed-
# form formula each effect uses (src/shaders/effects_common.h's grade()) ---
#
# The captures this runs against are always taken with Saturation's
# protect_skin_tones OFF, so `prot` is always 1.0 here and the formula below
# is exact rather than an approximation -- see effects-regression.sh's
# write_config().

def color_regions(img):
    """Every one of the 5 "colors" bands, as its mean (r, g, b) -- sampled
    well inside the band (10 %..90 % of the width) since, unlike the grey
    scenes, this one has no rectangles to dodge."""
    sx = img.width / W
    out = []
    for i in range(5):
        y0, y1 = int(i * img.height / 5), int((i + 1) * img.height / 5)
        out.append(region_mean(img, (int(0.10 * W * sx), y0, int(0.90 * W * sx), y1), inset=6))
    return out


def rgb_luma(rgb):
    r, g, b = rgb
    return 0.299 * r + 0.587 * g + 0.114 * b


def rgb_sat01(rgb):
    """max(c)-min(c) normalised to 0..1, matching grade()'s `sat` exactly
    (c there is 0..1 encoded; here rgb is 0..255)."""
    return (max(rgb) - min(rgb)) / 255.0


def clamp255(x):
    return max(0.0, min(255.0, x))


def expected_saturation(rgb, strength):
    """effects_common.h's grade(), EFFECT_SATURATION block, protect_skin
    OFF (prot = 1.0): m = min(strength,1); boost = max(strength-1,0)*(1-sat);
    out = luma + (c-luma)*(m+boost)."""
    l = rgb_luma(rgb)
    s = rgb_sat01(rgb)
    k = min(strength, 1.0) + max(strength - 1.0, 0.0) * (1.0 - s)
    return tuple(clamp255(l + (c - l) * k) for c in rgb)


def expected_vibrancy(rgb, strength):
    """effects_common.h's grade(), EFFECT_VIBRANCY block (NEW 2026-09-08):
    gain = 1 + strength*sat; out = luma + (c-luma)*gain."""
    l = rgb_luma(rgb)
    s = rgb_sat01(rgb)
    gain = 1.0 + strength * s
    return tuple(clamp255(l + (c - l) * gain) for c in rgb)


def cmd_colorcheck(args):
    image, effect, strength_s = args
    strength = float(strength_s)
    bands = color_regions(load(image))
    name = f"colors-{effect}-{strength_s}"

    if effect == "off":
        worst = max(abs(bands[i][c] - COLOR_BANDS[i][c]) for i in range(5) for c in range(3))
        detail = f"identity, worst deviation {worst:.1f} counts; " + \
            " ".join(f"b{i}=({bands[i][0]:.0f},{bands[i][1]:.0f},{bands[i][2]:.0f})" for i in range(5))
        sys.exit(0 if emit(worst <= 3.0, name, detail) else 1)

    if effect not in ("saturation", "vibrancy"):
        print(f"FAIL\t{name}\tunknown effect '{effect}'", file=sys.stderr)
        sys.exit(2)
    fn = expected_saturation if effect == "saturation" else expected_vibrancy

    checks = []
    worst = 0.0
    for i in range(5):
        exp = fn(COLOR_BANDS[i], strength)
        got = bands[i]
        d = max(abs(got[c] - exp[c]) for c in range(3))
        worst = max(worst, d)
        checks.append((f"band{i} matches the formula (delta {d:.1f})", d <= 4.0))
    # The invariant BOTH effects share: a pure grey pixel (saturation 0) has
    # c == luma exactly, so it must be untouched at ANY strength.
    grey_d = max(abs(bands[0][c] - 128.0) for c in range(3))
    checks.append((f"grey band0 stays ~128 (delta {grey_d:.1f})", grey_d <= 2.0))

    failed = [c for c, ok in checks if not ok]
    detail = ("FAILED: " + "; ".join(failed) + "; " if failed else "") + \
        f"worst delta {worst:.1f} counts; " + \
        " ".join(f"b{i}=({bands[i][0]:.0f},{bands[i][1]:.0f},{bands[i][2]:.0f})" for i in range(5))
    sys.exit(0 if emit(not failed, name, detail) else 1)


def cmd_colorshape(args):
    """THE HEADLINE CHECK for the Saturation/Vibrancy split: the same
    "colors" scene, captured once under Saturation (at a strength <= 1.0,
    the pure "m" regime -- see grade()'s EFFECT_SATURATION block: below
    neutral there is no adaptive "boost" term, only the flat multiplier
    `m`, so this is the genuinely shape-revealing case) and once under
    Vibrancy, compared by the RATIO each band's captured saturation
    (max-min) came out to versus its input saturation. Saturation
    multiplies every pixel's chroma by the SAME factor regardless of how
    saturated it already was, so that ratio is FLAT across every band,
    band 4 included -- shrinking chroma never clips. Vibrancy's gain rises
    with the pixel's own saturation, so its ratio strictly increases across
    bands 1-3; band 4 is deliberately excluded from that assertion and
    reported instead, because band 4 (255, 60, 0) already sits AT the
    sRGB gamut boundary -- one channel at 0, one at 255 -- so growing its
    chroma further is mathematically impossible without clipping a
    channel, and the clamp (grade()'s final `clamp(..., 0.0, 1.0)`, the
    same one every effect in this file ends on) is exactly what stops it.
    That is the "clamped so nothing wraps hue or clips a channel"
    property working as designed, not a measurement artefact."""
    img_sat, img_vib = args
    sat_bands = color_regions(load(img_sat))
    vib_bands = color_regions(load(img_vib))
    input_sats = [max(c) - min(c) for c in COLOR_BANDS]  # 0, 28, 86, 160, 255

    def ratios(bands):
        return [(max(bands[i]) - min(bands[i])) / input_sats[i] for i in range(1, 5)]

    sat_r, vib_r = ratios(sat_bands), ratios(vib_bands)
    sat_spread = max(sat_r) - min(sat_r)
    vib_spread = max(vib_r) - min(vib_r)
    # Bands 1-3 only: band 4 (index 3) is the gamut-clipped case above.
    vib_increasing = all(vib_r[i] < vib_r[i + 1] - 0.01 for i in range(2))
    checks = [
        ("Saturation's per-band ratio is flat (spread <= 0.15)", sat_spread <= 0.15),
        ("Vibrancy's per-band ratio strictly increases (bands 1-3)", vib_increasing),
        ("Vibrancy's band-3 ratio exceeds Saturation's flat one", vib_r[2] > sat_r[0] + 0.1),
    ]
    failed = [c for c, ok in checks if not ok]
    detail = ("FAILED: " + "; ".join(failed) + "; " if failed else "") + \
        "saturation ratios " + " ".join(f"{r:.2f}" for r in sat_r) + \
        " (spread " + f"{sat_spread:.2f}" + "); vibrancy ratios " + \
        " ".join(f"{r:.2f}" for r in vib_r) + " (spread " + f"{vib_spread:.2f}" + ")"
    sys.exit(0 if emit(not failed, "colors-shape", detail) else 1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd, args = sys.argv[1], sys.argv[2:]
    {"regions": cmd_regions, "check": cmd_check, "temporal": cmd_temporal, "ablog": cmd_ablog,
     "split": cmd_split, "splitcmp": cmd_splitcmp, "halo": cmd_halo, "slider": cmd_slider,
     "colorcheck": cmd_colorcheck, "colorshape": cmd_colorshape, "noclip": cmd_noclip,
     "means": cmd_means}[cmd](args)


if __name__ == "__main__":
    main()
