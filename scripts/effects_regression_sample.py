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
    ablog   <file> <what>                per-frame `effects_ab_log` lines (gamescope's
                                         console log); `what` is static | pan | transition
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
    r"gain=([\d.]+) gamma=([\d.]+) px\((\d+),(\d+)\)=(\d+),(\d+),(\d+)")


def parse_ablog(path):
    rows = []
    for line in open(path):
        m = AB_LOG_RE.search(line)
        if m:
            g = m.groups()
            rows.append(dict(n=int(g[0]), t=float(g[1]), rp98=float(g[7]), p50=float(g[10]),
                             p98=float(g[11]), gain=float(g[12]), gamma=float(g[13]),
                             px=(int(g[16]) + int(g[17]) + int(g[18])) / 3.0))
    return rows


def p2p(rows, key):
    v = [r[key] for r in rows]
    return max(v) - min(v)


def cmd_ablog(args):
    path, what = args
    rows = parse_ablog(path)
    name = f"stability-{what}" if what != "transition" else "transition-gain"
    if len(rows) < 100:
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
        detail = (f"{len(rows)} frames: raw p98 p2p={d['rp98']:.6f} (must be 0), gain p2p={d['gain']:.6f} (<= 0.002), "
                  f"smoothed p98 p2p={d['p98']:.4f} codes (<= 0.1), px p2p={d['px']:.1f} (must be 0); "
                  f"gain={rows[-1]['gain']:.4f} px={rows[-1]['px']:.0f}")
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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd, args = sys.argv[1], sys.argv[2:]
    {"regions": cmd_regions, "check": cmd_check, "temporal": cmd_temporal, "ablog": cmd_ablog}[cmd](args)


if __name__ == "__main__":
    main()
