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
    halo    <image> <scene> <what> [amp] [label]
                                         halobox/haloinv: the line profile out from the
                                         box's edge; `what` is off (flat), on (a
                                         monotone ramp, amplitude <= amp -- no ring) or
                                         info (the same numbers, printed, not asserted;
                                         `label` names the INFO line)
    temporal <settled> <t1> <t2> <t3> <band>
                                         the middle-band values across a scene switch
                                         must approach the settled value monotonically
    ablog   <file> <what>                per-frame `effects_ab_log` lines (gamescope's
                                         console log); `what` is static | pan | transition
                                         | panlocal | agstatic | agpan | bind | bloomstatic
                                         (agstatic/agpan/bind are Adaptive Gamma's,
                                         2026-09-08: the same stability bars applied to
                                         its ONE exponent, plus an INFO line naming the
                                         binding limit; bloomstatic is Bloom's -- a still
                                         frame must produce a bit-identical output pixel)
    noclip  <image> <scene> <label>      Adaptive Gamma's no-clipping property: bands stay
                                         ordered and apart, near-white highlights stay
                                         below white, black stays black
    means   <label> <img...>             INFO: each capture's frame mean, in order
    agsettle <name> <min-ratio> <label:tau:file>...
                                         ADAPTIVE GAMMA'S SPEED (2026-09-09): the time
                                         each `ab_log` trace takes to come within 5 % of
                                         where it settles after a scene switch, for the
                                         exponent and for the statistic under it. The
                                         traces must settle in the order given and the
                                         last must take >= min-ratio times the first --
                                         i.e. the slider really does change the settling
                                         time. Each trace's own tau= field is checked
                                         against the tau the spec claims, so a slider
                                         that did not take reads as a wrong tau rather
                                         than as a mysterious time
    colorcheck <image> <effect> <strength>
                                         the "colors" scene (2026-09-08): pins one
                                         effect's per-band output against the closed-
                                         form formula; `effect` is off | saturation |
                                         vibrancy
    bloomflat <image>                    BLOOM (2026-09-08): the control -- with the
                                         effect off, haloinv's flat field must come out
                                         flat, which is what makes every profile below
                                         a statement about the glow and not the capture
    bloomline <label> <mode> <img...>    the line profile out from haloinv's bright box,
                                         over one swept knob; `mode` is extent (the glow
                                         must reach further each step), amp-up (it must
                                         get brighter) or amp-down (dimmer)
    bloomnoclip <image> <scene> <label>  Bloom's no-clip property: the bands stay
                                         strictly ordered and nothing whose input was
                                         below white comes out ON white
    bloomsame <image-a> <image-b> <scene>
                                         two captures of the same scene must agree on
                                         every region away from a bright source -- the
                                         "bloom changes nothing it should not" check
    bloomjitter <label> <n-off> <img...> the first n-off captures are bloom-off and the
                                         rest bloom-on, all of one PANNING --periodic
                                         scene: turning bloom on must not widen the
                                         frame mean's frame-to-frame spread (the
                                         threshold-shimmer question, as a number)
    darkfloor <image-off> <image-on> <label>
                                        Dark Floor (2026-09-14) on the `blackout` scene:
                                        graded stays within 3 counts of raw on a coarse
                                        grid, and nothing under 64 comes out at 128+
    colorshape <image-saturation> <image-vibrancy>
                                         the same "colors" capture under Saturation and
                                         under Vibrancy at the same nominal strength:
                                         Saturation's per-band boost RATIO must be flat
                                         across bands, Vibrancy's must strictly increase
                                         -- the headline shape difference between the two
    previewsplit <img-off> <img-on> <img-split> <label>
                                         Preview (split screen) (2026-09-14): the SAME
                                         scene captured effect-off, effect-on (no
                                         split), and effect-on-with-preview-split-on.
                                         The split capture's left half must be
                                         BYTE-EXACT to the off capture's left half and
                                         its right half BYTE-EXACT to the on capture's
                                         right half -- the shader writes the raw input
                                         texel on the left, the fully-graded pixel on
                                         the right, nothing in between.
"""
import re
import math
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
    label = args[4] if len(args) > 4 else f"halo-{scene}-info"
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
    if what == "info":
        # Reported, not asserted -- Adaptive Brightness V2's own STRETCH
        # setting (Max lift 8 / Detail 2 / Clarity 1) uses this. The plan's
        # own "<= 8 codes at any setting" figure for that setting was a
        # prediction with no derivation behind it, and the V2 QC pass
        # (2026-09-14) showed the measured -11 on haloinv is the operator
        # doing exactly what its maths says, not an overshoot: a guided
        # filter's a*Y+b model misses a flat field by (1-a)(Y-mean) in the
        # windows that just touch a 205-code step (~0.7 code here), and the
        # detail term then amplifies that by the secant x Detail x
        # (1+Clarity) -- up to 8 x 2 x 2 = 32x at the stretch (guarantee 1's
        # own stated bound there). A CPU reproduction of the whole pipeline
        # gives -12.7 in float and -10.7 with the 8-bit coefficient buffers,
        # so quantisation is not the cause either (it slightly helps). The
        # DEFAULT-setting bound (<= 4 codes, `on 4.0`) is the guarantee that
        # is actually asserted; this line keeps the stretch number visible
        # so a regression there is still seen, without failing the run on a
        # bound nothing ever justified.
        print(f"INFO\t{label}\tamp={amp:+.1f} counts (not asserted; monotone={monotone}) {body}")
        sys.exit(0)
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


# ---- Bloom (2026-09-08) ---------------------------------------------------
#
# Measured on the `haloinv` scene, which is already exactly what a bloom test
# wants and was built for something else: one flat 220 box, 320x320, centred
# on a flat 15 field. A distinct bright source on a dark field, with a hard
# straight edge at a known x -- so the glow can be walked out from that edge
# along the box's own centre line and reported as a profile rather than as an
# impression.
#
# Its own distance list, finer than the halo checks' near the edge, because
# the quantity of interest is where the glow FALLS OFF: at Radius 0 the blur
# sigma is 8 source pixels and at Radius 1 it is 24, so the interesting range
# is 4..128 px and everything past that is the far field.
BLOOM_DISTANCES = [4, 8, 16, 32, 64, 128, 256, 460]
BLOOM_FLOOR = 2.0   # counts over the far field that still count as "glowing"


def bloom_profile(img):
    """The field's value at each BLOOM_DISTANCES step out from the bright
    box's right edge, along its centre line."""
    sx, sy = img.width / W, img.height / H
    cy = img.height // 2
    out = []
    for d in BLOOM_DISTANCES:
        x = int((HALO_EDGE_X + d) * sx)
        out.append(grey(region_mean(img, (x - 5, cy - 9, x + 5, cy + 9), inset=1)))
    return out


def bloom_extent(prof):
    """How far the glow reaches, in source pixels: the distance at which the
    profile last falls to BLOOM_FLOOR counts above the far field, linearly
    interpolated between the two samples that straddle it. 0 when nothing
    ever rises that far above the field."""
    far = prof[-1]
    amps = [v - far for v in prof]
    if amps[0] < BLOOM_FLOOR:
        return 0.0
    for i in range(1, len(amps)):
        if amps[i] < BLOOM_FLOOR:
            a, b = amps[i - 1], amps[i]
            da, db = BLOOM_DISTANCES[i - 1], BLOOM_DISTANCES[i]
            f = (a - BLOOM_FLOOR) / max(a - b, 1e-6)
            return da + f * (db - da)
    return float(BLOOM_DISTANCES[-1])


def cmd_bloomflat(args):
    """The control. haloinv's field is flat by construction, so with Bloom
    off it must come out flat -- if it does not, every profile below is
    measuring the capture path rather than the effect."""
    prof = bloom_profile(load(args[0]))
    spread = max(prof) - min(prof)
    body = " ".join(f"d{d}={v:.1f}" for d, v in zip(BLOOM_DISTANCES, prof))
    sys.exit(0 if emit(spread <= 1.5, "bloom-off-flat",
                       f"flat field stays flat, spread {spread:.1f} counts; {body}") else 1)


def cmd_bloomline(args):
    """bloomline <label> <mode> <img...> -- one knob swept in increasing
    order, measured as the glow's line profile out from the bright box.

    Three different statements, because the three params do three different
    things and a single "the picture changed" bar would not distinguish them:

      extent    Radius. The glow must reach FURTHER at every step -- the
                distance at which it falls back into the field, not its
                brightness, which barely moves (a wider Gaussian spreads the
                same light).
      amp-up    Intensity. The glow must get BRIGHTER right beside the source
                at every step, while its reach does not have to change.
      amp-down  Threshold. Raising it must make less of the source count as
                emitting, so the glow beside it must get DIMMER at every
                step. Reported alongside the extent so a threshold that
                accidentally acted like a radius would be visible.
    """
    label, mode = args[0], args[1]
    profs = [bloom_profile(load(p)) for p in args[2:]]
    fars = [p[-1] for p in profs]
    near = [p[0] - p[-1] for p in profs]
    extents = [bloom_extent(p) for p in profs]
    body = ("near-edge amplitude " + " -> ".join(f"{v:+.1f}" for v in near)
            + "; extent " + " -> ".join(f"{v:.0f}px" for v in extents)
            + "; far field " + " ".join(f"{v:.1f}" for v in fars)
            + "; profiles " + " | ".join(" ".join(f"{v:.1f}" for v in p) for p in profs))
    if mode == "extent":
        steps = [extents[i + 1] - extents[i] for i in range(len(extents) - 1)]
        ok = all(d > 0.0 for d in steps)
        detail = f"(each step must reach further) {body}"
    elif mode == "amp-up":
        steps = [near[i + 1] - near[i] for i in range(len(near) - 1)]
        ok = all(d > 1.0 for d in steps)
        detail = f"(each step must add > 1 count beside the source) {body}"
    elif mode == "amp-down":
        steps = [near[i + 1] - near[i] for i in range(len(near) - 1)]
        ok = all(d < -1.0 for d in steps)
        detail = f"(each step must remove > 1 count beside the source) {body}"
    else:
        print(f"FAIL\t{label}\tunknown bloomline mode '{mode}'", file=sys.stderr)
        sys.exit(2)
    sys.exit(0 if emit(ok, label, detail) else 1)


def cmd_bloomsame(args):
    """bloomsame <image-a> <image-b> <scene> -- every sampled region of the
    two captures must agree. Used to state, as a measurement rather than as
    an argument, that turning Bloom on leaves the parts of the picture that
    are away from any bright source exactly where they were: the `dark`
    scene's five bands are sampled at x 1150..1270, and its nearest 240
    highlight ends at x = 1070 -- 80 source pixels away, five sigma at the
    default Radius. Its pure-black corner is further still."""
    a, b, scene = load(args[0]), load(args[1]), args[2]
    va, vb = regions(a, scene), regions(b, scene)
    keys = [f"band{i}" for i in range(5)] + (["black"] if "black" in va else [])
    diffs = {k: vb[k] - va[k] for k in keys}
    worst = max(abs(d) for d in diffs.values())
    sys.exit(0 if emit(worst <= 1.0, f"bloom-unchanged-{scene}",
                       f"worst change away from a source {worst:.2f} counts (<= 1.0); "
                       + " ".join(f"{k}={va[k]:.1f}->{vb[k]:.1f}" for k in keys)) else 1)


def cmd_bloomnoclip(args):
    """bloomnoclip <image> <scene> <label> -- BLOOM's no-clip property,
    stated as exactly what the composite promises and no more.

    It is deliberately NOT `noclip` above, which Adaptive Gamma uses: that
    check also demands every band stay >= 2 counts from its neighbour, which
    is a statement about an exponent that leaves the ends alone, not about an
    operator whose whole job is to ADD light. Bloom at the top of its
    Intensity slider legitimately pushes a 245 band to within a count of the
    255 one; what it must never do is put something that was below white ON
    white. So this asserts:

      * the bands stay strictly ordered -- nothing is compressed INTO its
        neighbour, even where the gap narrows;
      * every region whose INPUT was below 255 reads at most 254 -- the
        no-clip statement itself;
      * the `dark` scene's 240 highlights stay above every band and its pure
        black corner stays black (a glow far from any source adds nothing);
      * the `bright` scene's 30-code shadows stay below every band.
    """
    image, scene, label = args
    sc = SCENES[scene]
    vals = regions(load(image), scene)
    bands = [vals[f"band{i}"] for i in range(5)]
    checks = []
    for i in range(4):
        checks.append((f"band{i} < band{i + 1} ({bands[i]:.1f} vs {bands[i + 1]:.1f})",
                       bands[i + 1] > bands[i]))
    for i in range(5):
        if sc["bands"][i] < 255:
            checks.append((f"band{i} (input {sc['bands'][i]}) stays off white "
                           f"({bands[i]:.1f} <= 254)", bands[i] <= 254.0))
    if "rect" in vals:
        if sc["rect"] > max(sc["bands"]):
            checks.append((f"the {sc['rect']} highlights stay off white ({vals['rect']:.1f} <= 254)",
                           vals["rect"] <= 254.0))
            checks.append((f"the {sc['rect']} highlights stay above every band "
                           f"({vals['rect']:.1f} > {bands[4]:.1f})", vals["rect"] > bands[4]))
        else:
            checks.append((f"the {sc['rect']} shadows stay below every band "
                           f"({vals['rect']:.1f} < {bands[0]:.1f})", vals["rect"] < bands[0]))
    if "black" in vals:
        checks.append((f"pure black stays black ({vals['black']:.1f} <= 2)", vals["black"] <= 2.0))

    failed = [c for c, ok in checks if not ok]
    detail = ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(vals)
    sys.exit(0 if emit(not failed, f"bloom-noclip-{scene}-{label}", detail) else 1)


def cmd_bloomjitter(args):
    """bloomjitter <label> <n-off> <img...> -- THE SHIMMER QUESTION, as a
    number. A threshold effect can flicker when a pixel sits on the boundary:
    under camera motion the pixel crosses the cut, its whole contribution
    switches, and a field of such switches crawls.

    The captures are of one PANNING --periodic scene, so the frame's true
    light population is identical every frame and only the layout moves. The
    first <n-off> are with Bloom off and are the baseline: they carry the
    capture path's own noise plus whatever the pan itself does to the frame
    mean. The rest are with Bloom on. If the bright pass shimmered, the "on"
    spread would be the larger of the two -- so the check is that it is not
    (within half a count of tolerance), which is a statement about the
    effect rather than about the harness."""
    label, n_off = args[0], int(args[1])
    vals = [frame_mean(p) for p in args[2:]]
    off, on = vals[:n_off], vals[n_off:]
    s_off = max(off) - min(off)
    s_on = max(on) - min(on)
    ok = s_on <= s_off + 0.5
    emit(ok, label,
         f"{len(off)} frames off: spread {s_off:.3f} counts ("
         + " ".join(f"{v:.2f}" for v in off) + "); "
         + f"{len(on)} frames on: spread {s_on:.3f} counts ("
         + " ".join(f"{v:.2f}" for v in on) + ") -- on must not exceed off + 0.5")


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
    r"(?: bind=(\d+) \(([^)]*)\))?"
    # The two EMA time constants the frame actually used (2026-09-09), so a
    # settling-time measurement can prove it ran at the speeds it claims.
    # Optional like everything after `px`: an older binary's line lacks it.
    r"(?: tau=([\d.]+)/([\d.]+))?")


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
            row.update(dt=float(g[2]) / 1000.0,
                       tau_up=float(g[27]) if g[27] else 0.0,
                       tau_down=float(g[28]) if g[28] else 0.0)
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


# How much of the measurement's own sampling noise an EMA lets through, as a
# multiple of what it lets through at tau = 1 s (the speed every threshold in
# this file was calibrated at). Needed since 2026-09-09, when Adaptive Gamma
# got its own adaptation speeds and the stability checks started being run at
# the FAST end of the slider as well as the default.
#
# WHY A FORMULA AND NOT A FIXED NUMBER. For x' = (1 - a) x + a m with white
# measurement noise, the settled output's standard deviation is
# sigma * sqrt(a / (2 - a)) -- so a smoothed statistic's spread is a property
# of the SMOOTHING, not of the estimator underneath it. Holding a tau = 0.1 s
# trace to a tau = 1 s bound would therefore fail the EMA for doing exactly
# what the slider asked, and would say "pulse" about a number that is not the
# picture. What must not move at any speed is the picture -- the exponent and
# the output pixel -- and those keep their absolute bounds. The RAW spread
# keeps its absolute bound too: it is the estimator's, and the EMA is not in
# it. See shader-effects.md's "Stability at the fast end".
def ema_noise_gain(tau, dt, tau_ref=1.0):
    if tau <= 0.0 or dt <= 0.0:
        return 1.0
    a = 1.0 - math.exp(-dt / tau)
    a_ref = 1.0 - math.exp(-dt / tau_ref)
    return math.sqrt((a / (2.0 - a)) / (a_ref / (2.0 - a_ref)))


def cmd_ablog(args):
    # The optional third argument is a name suffix, for the cases where the
    # same check is run more than once in a session at different settings and
    # the results table would otherwise carry two rows with one name.
    path, what = args[0], args[1]
    suffix = args[2] if len(args) > 2 else ""
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
            # The smoothed statistic's bound scales with the speed the trace
            # ran at (ema_noise_gain above); the exponent's and the raw
            # estimator's do not, because neither is a function of the
            # smoothing. At the default 1 s this is exactly the old 3 codes.
            tau = (last["tau_up"] + last["tau_down"]) / 2.0 or 1.0
            dt = sum(r["dt"] for r in rows) / len(rows)
            gain = ema_noise_gain(tau, dt)
            p98_bound = 3.0 * gain
            ok = d["gamma"] <= 0.06 and d["p98"] <= p98_bound and d["rp98"] * 255.0 <= 40.0
            bounds = (f"(gamma <= 0.06, smoothed p98 <= {p98_bound:.1f} codes "
                      f"= 3 x {gain:.2f} for tau {tau:.2f}s, raw p98 <= 40 codes)")
            d["rp98"] *= 255.0
        detail = (f"{len(rows)} frames, mode={last['mode']}: raw p98 p2p={d['rp98']:.6f}, "
                  f"gamma p2p={d['gamma']:.6f}, smoothed p98 p2p={d['p98']:.4f} codes, "
                  f"px p2p={d['px']:.1f} {bounds}; gamma={last['gamma']:.4f} px={last['px']:.0f}; "
                  f"local={last['local']:.2f} map {last['lmin'] * 255:.1f}..{last['lmax'] * 255:.1f} codes "
                  f"exponent {last['gainlo']:.3f}..{last['gainhi']:.3f}; bind={last['bindtext']}")
    elif what == "bloomstatic":
        # A SPATIAL effect on a perfectly still frame. Bloom reads none of
        # the measure pass's statistics, so nothing here is about adaptation
        # -- what is being asserted is that the three extra dispatches are a
        # deterministic function of the frame: same input, same output,
        # every composite, to the code value. A downsample that sampled the
        # source sparsely, or a blur whose weights depended on anything but
        # the uniform, would show up here as a moving probe pixel.
        last = rows[-1]
        d = dict(rp98=p2p(rows, "rp98"), px=p2p(rows, "px"))
        name = "bloom-stability-static" + suffix
        ok = d["rp98"] <= 0.0 and d["px"] <= 0.0
        detail = (f"{len(rows)} frames: raw p98 p2p={d['rp98']:.6f} (must be 0), "
                  f"px p2p={d['px']:.1f} (must be 0); px={last['px']:.0f}")
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


# ---- ADAPTIVE GAMMA's adaptation speed (2026-09-09) --------------------
#
# "For adaptive gamma, there should also be some value, to adjust the speed
# of it." The control is only real if the settling time actually MOVES with
# it, so that is what is measured -- on the same per-frame `effects_ab_log`
# trace across the same scene switch the Adaptive Brightness `transition`
# check uses, and against the same 5 %-of-final definition of "settled".
#
# WHAT SETTLING TIME MEANS HERE. Adaptive Gamma's entire state is the one
# exponent, so the exponent is what is timed (the Adaptive Brightness checks
# time the gain). t0 is the frame the raw statistics jump -- the scene
# switch itself, found in the data rather than assumed from a sleep -- and
# t95 is the first frame after it whose exponent is within 5 % of where the
# trace ends up. The closed form is 3 tau (tests/test_effects_curve.cpp
# asserts that on ema_alpha directly), so the measured numbers are reported
# beside it rather than only against each other.


def settle_ms(rows, key="gamma"):
    """(t95_ms, first, settled, tau_up, tau_down) for one transition trace."""
    i0 = next((i for i, r in enumerate(rows) if abs(r["rp98"] - rows[0]["rp98"]) > 0.1), 0)
    t0 = rows[i0]["t"]
    rows = rows[i0:]
    vals = [r[key] for r in rows]
    settled = vals[-1]
    span = abs(vals[0] - settled)
    t95 = next((r["t"] - t0 for r in rows if abs(r[key] - settled) <= 0.05 * span), None)
    return t95, vals[0], settled, rows[-1]["tau_up"], rows[-1]["tau_down"]


def cmd_agsettle(args):
    # agsettle <name> <min-ratio> <label:tau:file> ...
    # The traces must settle in the order given, and the last must take at
    # least <min-ratio> times as long as the first.
    name, min_ratio, specs = args[0], float(args[1]), args[2:]
    table, times, bad = [], [], []
    for spec in specs:
        label, tau, path = spec.split(":", 2)
        rows = parse_ablog(path)
        if len(rows) < 50:
            sys.exit(0 if emit(False, name, f"only {len(rows)} ab_log lines in {path}") else 1)
        t95, first, settled, tau_up, tau_down = settle_ms(rows, "gamma")
        # The smoothed median the exponent is derived from -- the EMA's own
        # output, with no clamp on it. Reported beside the exponent because
        # the two answer different questions: p50's t95 IS the closed form
        # (3 tau), while the exponent can arrive EARLIER because it saturates
        # against Max lift / Max darken on the way. A table showing only the
        # exponent would look like the EMA was faster than its own maths.
        t95_stat = settle_ms(rows, "p50")[0]
        times.append(t95)
        if t95 is None:
            bad.append(f"{label} never settled")
        # The trace's own tau, so a mis-set slider shows up as a wrong
        # number here instead of as a mysterious settling time.
        if abs(tau_up - float(tau)) > 1e-3 and abs(tau_down - float(tau)) > 1e-3:
            bad.append(f"{label} ran at tau={tau_up:.2f}/{tau_down:.2f}, expected {tau}")
        table.append(f"{label} (tau {tau}s, 3tau={3000 * float(tau):.0f} ms): "
                     f"exponent {first:.3f} -> {settled:.3f}, "
                     f"t95={'--' if t95 is None else round(t95)} ms "
                     f"(statistic {'--' if t95_stat is None else round(t95_stat)} ms)")
    ok = not bad and all(t is not None for t in times)
    if ok:
        ok = all(b > a for a, b in zip(times, times[1:])) and times[-1] >= min_ratio * times[0]
        if not ok:
            bad.append(f"expected strictly increasing and last >= {min_ratio}x first")
    detail = "; ".join(table) + ("" if not bad else "  -- " + "; ".join(bad))
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


def cmd_previewsplit(args):
    """Preview (split screen) (2026-09-14). Three captures of the SAME
    scene/settings: effect off, effect on (no split), effect on with
    Preview (split screen) also on. The shader's own contract
    (cs_effects_layer0.comp's final store) is that the split capture's left
    half is the raw input texel and its right half is the ordinary graded
    pixel -- i.e. BYTE-EXACT to the other two captures' matching halves,
    not merely close. A max abs per-channel diff of 0 is the assertion;
    anything else means either half saw the wrong pixels."""
    off_img, on_img, split_img = load(args[0]), load(args[1]), load(args[2])
    label = args[3] if len(args) > 3 else "preview-split"

    if off_img.size != split_img.size or on_img.size != split_img.size:
        sys.exit(0 if emit(False, label,
                           f"size mismatch off={off_img.size} on={on_img.size} "
                           f"split={split_img.size}") else 1)

    w, h = split_img.size
    mid = w // 2

    def max_diff(a, b, box):
        pa = list(a.crop(box).getdata())
        pb = list(b.crop(box).getdata())
        return max(max(abs(x - y) for x, y in zip(p1, p2)) for p1, p2 in zip(pa, pb))

    left_diff = max_diff(split_img, off_img, (0, 0, mid, h))
    right_diff = max_diff(split_img, on_img, (mid, 0, w, h))
    checks = [
        ("left half byte-exact to the raw (effect-off) capture", left_diff == 0),
        ("right half byte-exact to the full-effect capture", right_diff == 0),
    ]
    failed = [c for c, ok in checks if not ok]
    detail = ("FAILED: " + "; ".join(failed) + "; " if failed else "") + \
        f"left max diff {left_diff}, right max diff {right_diff}"
    sys.exit(0 if emit(not failed, label, detail) else 1)


def cmd_darkfloor(args):
    """darkfloor <image-off> <image-on> <label> -- the DARK FLOOR headline
    property (2026-09-14) on the near-black `blackout` scene
    (tests/effects_scene_client.c): with the floor active at its shipped
    default, the graded frame must be close to identity -- sampled on a
    coarse grid (stride 8, ~14400 samples of a 1280x720 frame) rather than
    every pixel, for speed -- and nothing that started under 64 may come out
    at 128 or above, which is the "binarised toward white" failure this
    feature exists to stop (superdoc/features/shader-effects.md has the real
    capture that reported it)."""
    off_path, on_path, label = args
    off, on = load(off_path), load(on_path)
    w, h = off.size
    stride = 8
    po, pn = off.load(), on.load()
    worst = 0.0
    violation = None
    for y in range(0, h, stride):
        for x in range(0, w, stride):
            go, gn = grey(po[x, y]), grey(pn[x, y])
            d = abs(gn - go)
            if d > worst:
                worst = d
            if go < 64.0 and gn >= 128.0 and violation is None:
                violation = (x, y, go, gn)
    ok = worst <= 3.0 and violation is None
    detail = f"worst deviation {worst:.1f} counts"
    if violation:
        x, y, go, gn = violation
        detail += f"; BINARISED at {x},{y}: raw={go:.1f} graded={gn:.1f}"
    sys.exit(0 if emit(ok, f"dark-floor-{label}", detail) else 1)


# ---- Adaptive Brightness V2 (NEW 2026-09-14) ------------------------------
#
# tests/effects_scene_client.c's own `silhouette`, `skyfore` and `flash`
# scenes, mirrored here exactly -- change one, change both. All three are
# authored in the same 1280x720 reference frame every scene above uses.
#
#   silhouette  70% of the frame literal 0; a 30%-of-frame band (y 252..468)
#               of a 6-code field with 1-code hashed noise; an 8%-of-frame
#               "lit floor" patch (40..60, textured) at x 100..484,
#               y 264..456; a 24x48 3-code figure at x 544..568, y 272..320;
#               a 24x48 10-code figure at x 644..668, y 272..320.
#   skyfore     top 55% (y 0..396) a 225 sky with cloud bands at 235
#               (y 108..144) and 215 (y 252..288); bottom 45% ground, 14
#               (x 0..640) / 20 (x 640..1280); an 8-code 30x30 figure at
#               200,436 and a 26-code 60x60 figure at 900,436.
#   flash       alternates the silhouette frame above with a 230-code frame
#               carrying two 200-code 40x40 shapes at (300,300)/(900,400).
#
# Regions below are sampled with a margin inside each shape's true edges
# (region_mean()'s own `inset` plus a further pull-in here), the same
#"never sample right at a boundary" discipline halo_profile()/regions()
# already follow.
SILHOUETTE = dict(
    field=(700, 300, 1000, 400),
    patch=(150, 300, 430, 400),
    fig3=(546, 276, 566, 316),
    fig10=(646, 276, 666, 316),
    void=(50, 50, 200, 150),
)

SKYFORE = dict(
    sky=(100, 50, 1180, 100),
    cloud1=(100, 112, 1180, 140),
    cloud2=(100, 256, 1180, 284),
    ground_l=(100, 620, 550, 690),
    ground_r=(750, 620, 1180, 690),
    fig8=(204, 440, 226, 462),
    fig26=(904, 440, 956, 492),
)


def abv2_sample(img, layout):
    sx, sy = img.width / W, img.height / H
    out = {}
    for name, (x0, y0, x1, y1) in layout.items():
        out[name] = grey(region_mean(img, (int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy)), inset=2))
    return out


def cmd_abv2_nobinarise(args):
    """abv2-nobinarise <image> -- the silhouette scene under V2 at its
    shipped defaults (Lift 0.5, Target 0.35, Max lift 4). The anti-
    binarisation guarantee stated qualitatively on a scene built for it: the
    three known-ordered raw values (fig3=3 < field=6 < fig10=10) must STAY
    ordered (a toe-gamma is monotone by construction, so this is "did
    anything invert", not a tuning question), and none of the lifted content
    may be slammed all the way to white -- the failure mode x^0.25 produces
    on the real capture (plan section 3.1: code 4 -> 90, i.e. next to white)."""
    v = abv2_sample(load(args[0]), SILHOUETTE)
    checks = [
        ("fig3 < field (raw order kept)", v["fig3"] < v["field"]),
        ("field < fig10 (raw order kept)", v["field"] < v["fig10"]),
        ("fig10 not slammed to white (< 220)", v["fig10"] < 220.0),
        ("field not slammed to white (< 220)", v["field"] < 220.0),
    ]
    failed = [c for c, ok in checks if not ok]
    sys.exit(0 if emit(not failed, "abv2-nobinarise",
                       ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(v)) else 1)


def cmd_abv2_silhouette(args):
    """abv2-silhouette <image-maxlift1> <image-default> -- "can I see the
    enemy", as a number: the fig3 figure's Weber contrast against its own
    surround (the field) must not DECREASE between Max lift 1 (an exact
    identity -- abv2_toe(x,g,1) === x, the guarantee's own zero point) and
    the shipped default. A lift that helped exposure but flattened the
    figure into its background would fail this while passing every other
    check here."""
    off, on = abv2_sample(load(args[0]), SILHOUETTE), abv2_sample(load(args[1]), SILHOUETTE)
    def weber(v):
        return (v["field"] - v["fig3"]) / max(v["field"], 1e-3)
    w_off, w_on = weber(off), weber(on)
    ok = w_on >= w_off - 0.02
    sys.exit(0 if emit(ok, "abv2-silhouette",
                       f"Weber contrast off(maxlift1)={w_off:.3f} on(default)={w_on:.3f} "
                       f"(raw fig3={off['fig3']:.1f} field={off['field']:.1f}; "
                       f"lifted fig3={on['fig3']:.1f} field={on['field']:.1f})") else 1)


def cmd_abv2_slope(args):
    """abv2-slope <image> <S> <detail> -- guarantee 1 on the existing `dark`
    scene's five KNOWN bands (5/8/12/16/20 raw): every adjacent pair's
    amplification ratio must be <= S * Detail + 0.05, the exact statement
    the plan's own v2-slope check makes, reusing a scene already in the
    default ring rather than a new one."""
    image, s_str, detail_str = args
    S, Detail = float(s_str), float(detail_str)
    v = regions(load(image), "dark")
    raw = [5, 8, 12, 16, 20]
    # The plan's "+ 0.05" is slack on the (dimensionless) slope itself. An
    # earlier version compared d_out in CODES against S*Detail + 0.05*255,
    # i.e. asserted "the band step grew by at most ~16.75 codes" -- a much
    # looser (and differently-shaped) test than the slope the message names.
    # Fixed by the V2 QC pass (2026-09-14) to assert exactly what it prints.
    # A further ~0.25 of slope slack covers the 8-bit screenshot's own
    # rounding of a 3-code input step (each band mean is quantised to
    # ~0.5 code, so a 3-code step's measured slope carries +-0.17 of noise
    # before anything the operator did).
    bound = S * Detail + 0.05 + 0.25
    checks = []
    for i in range(4):
        d_in = raw[i + 1] - raw[i]
        d_out = v[f"band{i + 1}"] - v[f"band{i}"]
        slope = d_out / d_in
        checks.append((f"band{i}->{i + 1} slope {slope:.2f} <= {bound:.2f}", slope <= bound))
    failed = [c for c, ok in checks if not ok]
    sys.exit(0 if emit(not failed, "abv2-slope",
                       ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(v)) else 1)


def cmd_abv2_black(args):
    """abv2-black <image> -- guarantee 2 on a coarse grid of the silhouette
    scene's own void (70% of the frame, literal 0): every sampled texel must
    read EXACTLY 0, not merely close -- the black floor (ABV2_BLACK) is a
    hard floor, not a soft one."""
    img = load(args[0])
    sx, sy = img.width / W, img.height / H
    x0, y0, x1, y1 = int(20 * sx), int(20 * sy), int(1260 * sx), int(230 * sy)   # well above the band
    px = img.load()
    worst = 0
    for y in range(y0, y1, 8):
        for x in range(x0, x1, 8):
            worst = max(worst, grey(px[x, y]))
    sys.exit(0 if emit(worst == 0, "abv2-black", f"worst void pixel = {worst:.0f} (must be exactly 0)") else 1)


def cmd_abv2_sky(args):
    """abv2-sky <image> -- the highlight guard on `skyfore`: the two cloud
    bands must stay separated from the sky and from each other after V2's
    toe compresses the whole upper range by ~g. A monotone toe cannot
    invert this order; the check is that it does not also MERGE it."""
    v = abv2_sample(load(args[0]), SKYFORE)
    checks = [
        ("cloud1 (235) stays above sky (225) by >= 3", v["cloud1"] - v["sky"] >= 3.0),
        ("sky (225) stays above cloud2 (215) by >= 3", v["sky"] - v["cloud2"] >= 3.0),
        ("ground figures still ordered (8 < fig8, 26 < fig26 raw; "
         "fig8 < fig26 lifted)", v["fig8"] < v["fig26"]),
        ("ground_l (14) < ground_r (20) order kept", v["ground_l"] < v["ground_r"]),
    ]
    failed = [c for c, ok in checks if not ok]
    sys.exit(0 if emit(not failed, "abv2-sky",
                       ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(v)) else 1)


def cmd_abv2_darken_bright(args):
    """abv2-darken-bright <image-off> <image-on> -- DARKENING (2026-09-14):
    on `bright`, Adaptation Scene deepens the darken past its static floor
    on a scene that reads brighter than Target (the mirror of Dynamic's own
    dark-scene lift-deepening). `off` is V2 at defaults (Max darken 1, i.e.
    darkening disabled); `on` is the same scene with Adaptation Scene and
    Max darken 2 -- the scene the task itself names."""
    off, on = regions(load(args[0]), "bright"), regions(load(args[1]), "bright")
    raw = SCENES["bright"]["bands"]
    D = 2.0
    checks = [
        ("order preserved (on)", on["band0"] < on["band1"] < on["band2"] < on["band3"] <= on["band4"]),
        ("245 band (band3) comes down vs Max darken 1", on["band3"] < off["band3"] - 1.0),
        # band4 (raw 255, pure white) is NOT expected to move: f(1) == 1 is
        # an exact fixed point of the two-sided curve by construction (both
        # halves pin white), so "comes down" does not apply to it -- see
        # effects_curve.h's own closed-form proof. Only that it stays <= 255.
        ("255 band (band4) still <= 255 -- the fixed point, not clipped", on["band4"] <= 255.0 + 0.5),
    ]
    # Nothing below raw/D (guarantee: secant >= 1/D everywhere), 1 code of
    # 8-bit rounding slack, the same style bright-dynamic's own min_gain
    # floor check uses.
    for i, rawv in enumerate(raw):
        checks.append((f"band{i} ({rawv}) not darkened past raw/D={rawv / D:.1f}",
                       on[f"band{i}"] >= rawv / D - 1.0))
    failed = [c for c, ok in checks if not ok]
    v = {"off_" + k: val for k, val in off.items()}
    v.update(on)
    sys.exit(0 if emit(not failed, "abv2-darken-bright",
                       ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(v)) else 1)


def cmd_abv2_darken_sky(args):
    """abv2-darken-sky <image-off> <image-on> -- DARKENING (2026-09-14):
    on `skyfore`, a static Darken (0.5) with Lift at its own default. `off`
    is V2 at Lift default with Darken 0 (Max darken 1); `on` is the same
    Lift default with Darken 0.5, Max darken >= 2. Checks the SAME frame's
    two halves move in OPPOSITE directions (sky down, ground/figures up,
    both from raw) and that the below-Target half (ground, figures) is
    UNCHANGED by Darken -- the pivot construction's own claim (effects_
    curve.h's abv2_curve2(): the x <= Target branch is the untouched lift
    curve, never rescaled by the darken side)."""
    off, on = abv2_sample(load(args[0]), SKYFORE), abv2_sample(load(args[1]), SKYFORE)
    raw = dict(sky=225.0, cloud1=235.0, cloud2=215.0, ground_l=14.0, ground_r=20.0, fig8=8.0, fig26=26.0)
    checks = [
        ("sky (225) comes down further with Darken 0.5", on["sky"] < off["sky"] - 1.0),
        ("cloud1 (235) comes down further with Darken 0.5", on["cloud1"] < off["cloud1"] - 1.0),
        ("cloud2 (215) comes down further with Darken 0.5", on["cloud2"] < off["cloud2"] - 1.0),
        ("sky still above raw ground/figures (order preserved)",
         on["sky"] > on["ground_r"] and on["sky"] > on["fig26"]),
    ]
    # Below Target (ground, figures): Darken must be a NO-OP, within 8-bit
    # capture rounding -- the pivot's x <= Target branch is byte-identical
    # to the Lift-only curve regardless of Darken/Max darken.
    for name in ("ground_l", "ground_r", "fig8", "fig26"):
        checks.append((f"{name} unchanged by Darken (below Target, pivot's own claim)",
                       abs(on[name] - off[name]) <= 2.0))
        checks.append((f"{name} lifted above its raw value ({raw[name]:.0f}) by Lift",
                       on[name] > raw[name] + 3.0))
    failed = [c for c, ok in checks if not ok]
    v = {"off_" + k: val for k, val in off.items()}
    v.update(on)
    sys.exit(0 if emit(not failed, "abv2-darken-sky",
                       ("FAILED: " + "; ".join(failed) + "; " if failed else "") + fmt(v)) else 1)


def cmd_abv2_static(args):
    """abv2-static <label> <img...> -- guarantee 5's still-frame half: N
    captures of the SAME still frame (Scene mode, motion paused) must be
    close to BYTE IDENTICAL. Scene-agnostic (a coarse whole-image grid,
    like darkfloor's), so it runs on whichever still scene the caller
    already has on screen rather than needing its own.

    Bound is <= 2 counts, not 0: unlike `stability-static`'s own RAW
    statistic (read as an exact float via `effects_ab_log`, which this
    check does not use), this reads the SMOOTHED anchor's effect on the
    picture through an 8-bit PNG screenshot. The measure pass's raw
    measurement of a truly still scene is bit-identical every frame, but
    the EMA that tracks it (effects_curve.h's mix()) only approaches that
    value asymptotically -- it does not reach it exactly in finite time --
    so a residual sub-code drift can still tip one 8-bit output pixel
    across a rounding boundary between two captures even after the
    ADAPT_SETTLE_S wait. That is exactly why the codebase's OWN existing
    checks on the SMOOTHED (not raw) side of this same EMA use a small
    nonzero tolerance too (`stability-static`'s "smoothed p98 p2p <= 0.1
    codes", "gain p2p <= 0.002") rather than demanding exact 0 -- this
    check is that same family, just observed through a screenshot instead
    of the float trace."""
    label, paths = args[0], args[1:]
    imgs = [load(p) for p in paths]
    w, h = imgs[0].size
    stride = 8
    base = imgs[0].load()
    worst = 0.0
    for img in imgs[1:]:
        px = img.load()
        for y in range(0, h, stride):
            for x in range(0, w, stride):
                worst = max(worst, abs(grey(px[x, y]) - grey(base[x, y])))
    sys.exit(0 if emit(worst <= 2.0, f"abv2-static-{label}", f"worst pixel delta {worst:.2f} over {len(paths)} captures") else 1)


def cmd_abv2_pan(args):
    """abv2-pan <label> <img...> -- the 2026-09-07 pulse must not return, on
    `texdark` panning under --periodic (its true statistics never change).
    A snap (the scene-cut detector firing on ordinary camera motion) would
    show up here as a jump much bigger than sampling noise between
    consecutive captures; this is the same frame-to-frame SPREAD statement
    bloomjitter makes for Bloom, applied to V2's own content anchor via the
    frame mean."""
    label, paths = args[0], args[1:]
    means = [grey(region_mean(load(p), (0, 0, 1280, 720), inset=0)) for p in paths]
    spread = max(means) - min(means)
    # A real cut only fires on a >= 2x brightness swing (the plan's own
    # |ln(raw/smoothed)| > ln 2); a pan's frame-to-frame mean noise is a
    # fraction of a count once the anchor has settled, so a generous 6-count
    # budget still catches a cut firing by mistake without being a hair-
    # trigger on capture noise.
    sys.exit(0 if emit(spread <= 6.0, f"abv2-pan-{label}",
                       f"frame-mean spread {spread:.2f} counts over {len(paths)} captures (means: "
                       + " ".join(f"{m:.1f}" for m in means) + ")") else 1)


def cmd_abv2_cut(args):
    """abv2-cut <image-before> <image-first-after> <image-settled> -- the
    scene-cut detector SNAPS instead of sliding across `flash`'s dark ->
    bright transition. Screenshot round-trips in this harness (a `stat`
    poll every 50ms) cannot resolve individual composited frames, so this
    checks the property the plan's "<= 2 composites" bound exists to state
    for a human: the FIRST post-switch capture must already be close to the
    settled value, not partway through a multi-second slide the way the
    pre-bounded operators' 1s EMA would produce."""
    before, first, settled = (grey(region_mean(load(p), (0, 0, 1280, 720), inset=0)) for p in args)
    total_move = settled - before
    first_move = first - before
    ratio = first_move / total_move if abs(total_move) > 1.0 else 1.0
    ok = ratio >= 0.8
    sys.exit(0 if emit(ok, "abv2-cut",
                       f"before={before:.1f} first-after={first:.1f} settled={settled:.1f} "
                       f"({ratio * 100:.0f}% of the move already present in the first capture)") else 1)


def cmd_abv2_colour(args):
    """abv2-colour <image-off> <image-on> -- plan 4.7's hue-preservation: V2
    reconstructs colour by luma RATIO, so every band's HUE ANGLE must be
    unchanged (within capture rounding) even though its brightness moved.
    Reuses the `colors` scene's own five bands."""
    off, on = color_regions(load(args[0])), color_regions(load(args[1]))
    def hue_deg(rgb):
        r, g, b = (c / 255.0 for c in rgb)
        mx, mn = max(r, g, b), min(r, g, b)
        d = mx - mn
        if d < 1e-4:
            return None   # grey has no hue
        if mx == r:
            h = 60.0 * (((g - b) / d) % 6.0)
        elif mx == g:
            h = 60.0 * (((b - r) / d) + 2.0)
        else:
            h = 60.0 * (((r - g) / d) + 4.0)
        return h
    checks = []
    for i in range(1, 5):   # band 0 is grey -- no hue to compare
        h0, h1 = hue_deg(off[i]), hue_deg(on[i])
        if h0 is None or h1 is None:
            continue
        d = min(abs(h0 - h1), 360.0 - abs(h0 - h1))
        checks.append((f"band{i} hue shift {d:.1f} deg <= 2", d <= 2.0))
    failed = [c for c, ok in checks if not ok]
    sys.exit(0 if emit(not failed, "abv2-colour", ("FAILED: " + "; ".join(failed) + "; " if failed else "")
                       + "; ".join(f"band{i}: {hue_deg(off[i])}->{hue_deg(on[i])}" for i in range(1, 5))) else 1)


CAPTURE_ZOMBIE = (440, 346, 520, 506)   # (660..780, 520..760) in the 1920x1080 original, x2/3 to 1280x720
# "The floor beside it": measured directly on the source PNG rather than
# guessed -- (950..1150, 580..650) in the 1920x1080 original is the bright
# grated walkway visible between the two zombie clusters (mean 70.1 raw vs
# the zombie's own 8.5, a clean floor read with no character or blood-decal
# pixels in it), x2/3 to this harness's 1280x720 capture size.
CAPTURE_FLOOR = (633, 387, 767, 433)


def _frac_ge(img, thresh, stride=4):
    px = img.load()
    w, h = img.size
    n = ge = 0
    for y in range(0, h, stride):
        for x in range(0, w, stride):
            n += 1
            if grey(px[x, y]) >= thresh:
                ge += 1
    return 100.0 * ge / max(n, 1)


def cmd_abv2_capture(args):
    """abv2-capture <image-off> <image-on> <which> -- the Stage-1 acceptance
    bar (plan section 6), on the user's own capture, through the REAL GPU
    path (not a CPU re-application of the formula). `which` is one of two
    (the plan's own "13 checks" count treats them separately, since they
    measure two different failure modes, and run_sampler()/record_line() in
    effects-regression.sh -- like every OTHER sampler command here -- expect
    exactly one PASS/FAIL line per python invocation, so this is called
    TWICE rather than printing two lines from one call: a first version of
    this check printed both from a single call and record_line()'s `read`
    silently dropped the second, a bug caught only by inspecting the raw
    results file after a real run rather than by the exit code):

      nobinarise  pixels >= 128 <= 6% (raw 1.1%, x^0.25 today 21%) AND the
                 zombie/floor separation >= 35 codes (raw 24.7) -- "is the
                 corridor readable without turning grey-white".
      noclip     the >= 250 population adds <= 0.05 percentage points over
                 RAW's own -- the plan's own corrected bar, since the
                 capture already has a lit lamp at ~0.09% >= 250 and
                 "0% >= 250" is therefore not the right bar for THIS frame.
    """
    off_path, on_path, which = args
    off, on = load(off_path), load(on_path)

    if which == "nobinarise":
        pct128_on = _frac_ge(on, 128.0)
        sep_off = grey(region_mean(off, CAPTURE_FLOOR)) - grey(region_mean(off, CAPTURE_ZOMBIE))
        sep_on = grey(region_mean(on, CAPTURE_FLOOR)) - grey(region_mean(on, CAPTURE_ZOMBIE))
        checks = [
            (f"pixels >= 128 <= 6% (got {pct128_on:.1f}%)", pct128_on <= 6.0),
            (f"zombie/floor separation >= 35 codes (got {sep_on:.1f}, raw {sep_off:.1f})", sep_on >= 35.0),
        ]
        failed = [c for c, ok in checks if not ok]
        sys.exit(0 if emit(not failed, "abv2-capture-nobinarise",
                           ("FAILED: " + "; ".join(failed) + "; " if failed else "")
                           + f"pct128={pct128_on:.2f}% sep off={sep_off:.1f} on={sep_on:.1f}") else 1)

    if which == "noclip":
        pct250_off = _frac_ge(off, 250.0)
        pct250_on = _frac_ge(on, 250.0)
        sys.exit(0 if emit(pct250_on <= pct250_off + 0.05, "abv2-capture-noclip",
                           f"pixels >= 250: raw {pct250_off:.2f}% -> V2 {pct250_on:.2f}% (adds "
                           f"{pct250_on - pct250_off:+.2f}pp, budget <= 0.05pp)") else 1)

    print(f"FAIL\tabv2-capture\tunknown which '{which}'", file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd, args = sys.argv[1], sys.argv[2:]
    {"regions": cmd_regions, "check": cmd_check, "temporal": cmd_temporal, "ablog": cmd_ablog,
     "split": cmd_split, "splitcmp": cmd_splitcmp, "halo": cmd_halo, "slider": cmd_slider,
     "colorcheck": cmd_colorcheck, "colorshape": cmd_colorshape, "noclip": cmd_noclip,
     "means": cmd_means, "agsettle": cmd_agsettle,
     "bloomflat": cmd_bloomflat, "bloomline": cmd_bloomline,
     "bloomsame": cmd_bloomsame, "bloomjitter": cmd_bloomjitter,
     "bloomnoclip": cmd_bloomnoclip,
     "darkfloor": cmd_darkfloor,
     "abv2nobinarise": cmd_abv2_nobinarise, "abv2silhouette": cmd_abv2_silhouette,
     "abv2slope": cmd_abv2_slope, "abv2black": cmd_abv2_black, "abv2sky": cmd_abv2_sky,
     "abv2static": cmd_abv2_static, "abv2pan": cmd_abv2_pan, "abv2cut": cmd_abv2_cut,
     "abv2colour": cmd_abv2_colour, "abv2capture": cmd_abv2_capture,
     "abv2darkenbright": cmd_abv2_darken_bright, "abv2darkensky": cmd_abv2_darken_sky,
     "previewsplit": cmd_previewsplit,
     }[cmd](args)


if __name__ == "__main__":
    main()
