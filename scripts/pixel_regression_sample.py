#!/usr/bin/env python3
"""pixel_regression_sample.py -- pixel measurement/assertion helper for
scripts/pixel-regression.sh. Not meant to be run standalone; every threshold
it compares against arrives as a CLI argument so the THRESHOLDS themselves
stay defined as named constants in the bash script, not buried in here.

Uses PIL (already installed on this machine -- checked with
`python3 -c "import PIL"` before writing this, per the task brief; no new
dependency was added).

Every subcommand prints exactly one line to stdout:

    PASS<TAB>name<TAB>detail
    FAIL<TAB>name<TAB>detail

and exits 0 for PASS, 1 for FAIL, 2 for a usage/measurement error (bad path,
box outside the image, etc.) so the caller can tell "the check failed" from
"the script is broken" -- see the exit code check in the bash caller.
"""
import sys
import argparse
from collections import Counter

try:
    from PIL import Image
except ImportError:
    print("FAIL\tpython-PIL\tPIL is not importable -- see the note in scripts/README.md", file=sys.stderr)
    sys.exit(2)


def emit(ok, name, detail):
    print(f"{'PASS' if ok else 'FAIL'}\t{name}\t{detail}")
    sys.exit(0 if ok else 1)


def load(path):
    try:
        return Image.open(path).convert("RGB")
    except Exception as e:  # noqa: BLE001 -- reported as a hard error, not a check failure
        print(f"FAIL\timage-load\tcould not open {path}: {e}", file=sys.stderr)
        sys.exit(2)


def chebyshev(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def luma(c):
    # Rec.709 weights on the encoded (sRGB 8-bit) channel values -- an
    # approximation of encoded Y', matching the quantity
    # src/shaders/alphamode.h's contrast guard judges "too close" on. See
    # superdoc/features/fps-display.md's "Text colour: Fixed vs. Inverted".
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def cmd_pixel(a):
    img = load(a.image)
    if not (0 <= a.x < img.width and 0 <= a.y < img.height):
        print(f"FAIL\tpixel\t({a.x},{a.y}) is outside the {img.width}x{img.height} image", file=sys.stderr)
        sys.exit(2)
    r, g, b = img.getpixel((a.x, a.y))
    emit(True, "pixel", f"{r} {g} {b}")


def cmd_digit(a):
    img = load(a.image)
    box = (a.x0, a.y0, a.x1, a.y1)
    if not (0 <= box[0] < box[2] <= img.width and 0 <= box[1] < box[3] <= img.height):
        print(f"FAIL\tdigit:{a.name}\tbox {box} outside the {img.width}x{img.height} image", file=sys.stderr)
        sys.exit(2)

    bg = (a.bg_r, a.bg_g, a.bg_b)
    pixels = list(img.crop(box).getdata())
    nonbg = [p for p in pixels if chebyshev(p, bg) > a.diff_thresh]
    nonblack = [p for p in nonbg if max(p) > a.black_thresh]
    black_count = len(nonbg) - len(nonblack)

    if not nonblack:
        emit(False, a.name,
             f"no non-background, non-black pixel found in box {box} "
             f"(bg={bg}, {len(nonbg)} non-bg px, {black_count} of them near-black)")

    fill = Counter(nonblack).most_common(1)[0][0]
    fill_count = sum(1 for p in nonblack if p == fill)
    gap = abs(luma(fill) - luma(bg))
    exp = (a.exp_r, a.exp_g, a.exp_b)
    within_tol = chebyshev(fill, exp) <= a.tol
    gap_ok = gap >= a.min_gap
    ok = within_tol and gap_ok
    detail = (f"measured={fill} (n={fill_count}) expected={exp} tol={a.tol} "
              f"gap={gap:.1f} min_gap={a.min_gap} bg={bg} black_px={black_count}")
    emit(ok, a.name, detail)


def cmd_blackcount(a):
    img = load(a.image)
    box = (a.x0, a.y0, a.x1, a.y1)
    if not (0 <= box[0] < box[2] <= img.width and 0 <= box[1] < box[3] <= img.height):
        print(f"FAIL\tblackcount:{a.name}\tbox {box} outside the {img.width}x{img.height} image", file=sys.stderr)
        sys.exit(2)

    bg = (a.bg_r, a.bg_g, a.bg_b)
    pixels = list(img.crop(box).getdata())
    nonbg = [p for p in pixels if chebyshev(p, bg) > a.diff_thresh]
    black_count = sum(1 for p in nonbg if max(p) <= a.black_thresh)
    present = black_count >= a.min_count
    ok = present == bool(a.expect_present)
    detail = (f"black_px={black_count} min_count={a.min_count} "
              f"expect_present={bool(a.expect_present)} bg={bg}")
    emit(ok, a.name, detail)


def srgb_to_linear(x):
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def linear_to_srgb(x):
    return x * 12.92 if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def coverage_blend_expected(color, alpha, bg, bits):
    """What a straight-alpha ImGui draw of `color` at `alpha` composites to
    over the flat `bg` through the HUD layer's blend -- the crosshair.md
    "Known limitation" arithmetic, made explicit so the check pins the
    mechanism rather than an ideal:

      * ImGui blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA onto a texture cleared
        to 0, so the stored texel is PREMULTIPLIED: (c * a, a), quantised to
        `bits` per channel (8 normally; 16 when the Inverted HUD and the
        crosshair share the texture -- see FpsDisplay.cpp's
        ResolveTextureFormat()).
      * The composite (alphamode.h, the coverage blend and the invert
        layer's non-digit path alike) decodes the texel's RGB from sRGB and
        blends it as if it were STRAIGHT alpha, in linear light:
        out = lin(texel) * a + lin(bg) * (1 - a).

    So a 50 % crosshair lands at c * a * a + bg * (1 - a) in linear light --
    (0,255,0) over (51,51,51) measures (35, 99, 35), not the (26,153,26) or
    (35,190,35) an ideal encoded/linear half-blend would give. That is the
    HUD's long-standing look (the backdrop has always been composited the
    same way) and NOT something this check may "fix" by itself.
    """
    q = (1 << bits) - 1
    texel_a = round(q * alpha) / q
    out = []
    for ch, b in zip(color, bg):
        texel = round(q * (ch / 255.0) * alpha) / q
        lin = srgb_to_linear(texel) * texel_a + srgb_to_linear(b / 255.0) * (1.0 - texel_a)
        out.append(int(round(linear_to_srgb(lin) * 255.0)))
    return tuple(out)


def sample_ray(a, img):
    offsets = range(a.start, a.end + 1)
    samples = []
    for off in offsets:
        x = a.cx + a.dx * off
        y = a.cy + a.dy * off
        if not (0 <= x < img.width and 0 <= y < img.height):
            print(f"FAIL\tline:{a.name}\tsample ({x},{y}) outside the {img.width}x{img.height} image",
                  file=sys.stderr)
            sys.exit(2)
        samples.append(((x, y), img.getpixel((x, y))))
    return samples


def assert_ray(a, samples, target, extra=""):
    matches = [(pos, c) for pos, c in samples if chebyshev(c, target) <= a.tol]
    if a.mode == "all":
        ok = len(matches) == len(samples)
    else:  # "any"
        ok = len(matches) > 0

    if ok:
        detail = f"{len(matches)}/{len(samples)} offsets matched {target} (tol {a.tol}){extra}"
    else:
        # Name the first mismatch/absence for a human reading results.txt.
        if a.mode == "all":
            bad_pos, bad_c = next((pos, c) for pos, c in samples if chebyshev(c, target) > a.tol)
            detail = f"offset {bad_pos} = {bad_c}, expected {target} (tol {a.tol}){extra}"
        else:
            detail = f"none of {len(samples)} offsets matched {target} (tol {a.tol}); sample={samples[0][1]}{extra}"
    emit(ok, a.name, detail)


def cmd_line_blend(a):
    # `line`, but the target is computed from a configured colour, its
    # opacity and the flat background via coverage_blend_expected() -- the
    # semi-transparent crosshair check.
    img = load(a.image)
    samples = sample_ray(a, img)
    target = coverage_blend_expected((a.r, a.g, a.b), a.alpha, (a.bg_r, a.bg_g, a.bg_b), a.bits)
    assert_ray(a, samples, target,
               extra=f" [colour ({a.r},{a.g},{a.b}) @ {a.alpha:.2f} over ({a.bg_r},{a.bg_g},{a.bg_b}), {a.bits}-bit texel]")


def cmd_line(a):
    img = load(a.image)
    offsets = range(a.start, a.end + 1)
    target = (a.r, a.g, a.b)
    samples = []
    for off in offsets:
        x = a.cx + a.dx * off
        y = a.cy + a.dy * off
        if not (0 <= x < img.width and 0 <= y < img.height):
            print(f"FAIL\tline:{a.name}\tsample ({x},{y}) outside the {img.width}x{img.height} image",
                  file=sys.stderr)
            sys.exit(2)
        samples.append(((x, y), img.getpixel((x, y))))

    matches = [(pos, c) for pos, c in samples if chebyshev(c, target) <= a.tol]
    if a.mode == "all":
        ok = len(matches) == len(samples)
    else:  # "any"
        ok = len(matches) > 0

    if ok:
        detail = f"{len(matches)}/{len(samples)} offsets matched {target} (tol {a.tol})"
    else:
        # Name the first mismatch/absence for a human reading results.txt.
        if a.mode == "all":
            bad_pos, bad_c = next((pos, c) for pos, c in samples if chebyshev(c, target) > a.tol)
            detail = f"offset {bad_pos} = {bad_c}, expected {target} (tol {a.tol})"
        else:
            detail = f"none of {len(samples)} offsets matched {target} (tol {a.tol}); sample={samples[0][1]}"
    emit(ok, a.name, detail)



# ---------------------------------------------------------------------------
# 2026-09-06: the crosshair's Shrink rates, Animate back, and Apply Scaling
# (requests #11, #13, #14 -- superdoc/features/crosshair.md)
# ---------------------------------------------------------------------------

def arm_run(img, cx, cy, dx, dy, target, tol, max_off):
    """The first run of `target`-coloured pixels along a ray from (cx, cy),
    skipping the centre pixel itself (offset 0). Returns (first, last)
    offsets, or (None, None). The pixel path is exact, so a plain colour
    match is the right test here (the scaled path has its own, coverage
    based, scan below)."""
    first = last = None
    for off in range(1, max_off + 1):
        x, y = cx + dx * off, cy + dy * off
        if not (0 <= x < img.width and 0 <= y < img.height):
            break
        hit = chebyshev(img.getpixel((x, y)), target) <= tol
        if hit:
            if first is None:
                first = off
            last = off
        elif first is not None:
            break
    return first, last


def gap_len(img, cx, cy, dx, dy, target, tol, max_off):
    """(gap, length) in pixels along one arm's ray: gap = pixels between the
    centre pixel and the arm (the centre column/row's own edge is the
    centre pixel for the 1 px-wide line these checks use, and the centre
    square joins the arms at gap 0 -- crosshair.md, Geometry)."""
    first, last = arm_run(img, cx, cy, dx, dy, target, tol, max_off)
    if first is None:
        return None, 0
    return first - 1, last - first + 1


def shrink_model(t_ms, hide_ms, gap_px, len_px):
    """Expected (gap, length) at t after the press: the visible edge moves at
    (gap + length) / hide_ms px per ms through both phases -- gap first,
    then length (crosshair::ShrinkSplit)."""
    travel = max(0.0, min(1.0, t_ms / hide_ms)) * (gap_px + len_px)
    gap = max(0.0, gap_px - travel)
    length = len_px - max(0.0, travel - gap_px)
    return gap, length


def cmd_shrink_rate(a):
    """Five captures at known times after a right-click press in Shrink mode,
    Animate back off: asserts (1) the gap-closing rate over the two phase-1
    captures equals the arm-shortening rate over the two phase-2 captures
    within `tol` px per the phase-1 interval, and (2) the 50 % capture sits
    on the model within `tol` px. Reports every measurement."""
    target = (a.r, a.g, a.b)
    shots = [(a.img1, a.t1), (a.img2, a.t2), (a.img3, a.t3), (a.img4, a.t4), (a.img5, a.t5)]
    meas = []
    for path, t in shots:
        img = load(path)
        g_r, l_r = gap_len(img, a.cx, a.cy, 1, 0, target, a.tol_color, 200)
        g_d, l_d = gap_len(img, a.cx, a.cy, 0, 1, target, a.tol_color, 200)
        meas.append((t, g_r, l_r, g_d, l_d))
    detail = " ".join(f"t={t}ms:gap={g}/{gd},len={l}/{ld}" for t, g, l, gd, ld in meas)
    if any(m[1] is None or m[3] is None for m in meas[:4]):
        emit(False, a.name, "an arm vanished in a phase capture; " + detail)
    (t1, g1, _, _, _), (t2, g2, _, _, _), (t3, _, l3, _, _), (t4, _, l4, _, _), (t5, g5, l5, _, _) = meas
    if t2 <= t1 or t4 <= t3:
        emit(False, a.name, "captures are not in time order; " + detail)
    gap_rate = (g1 - g2) / (t2 - t1)   # px per ms, phase 1
    len_rate = (l3 - l4) / (t4 - t3)   # px per ms, phase 2
    ref_ms = t2 - t1
    diff_px = abs(gap_rate - len_rate) * ref_ms
    eg5, el5 = shrink_model(t5, a.hide_ms, a.gap_px, a.len_px)
    mid_ok = g5 is not None and abs(g5 - eg5) <= a.tol and abs(l5 - el5) <= a.tol
    ok = diff_px <= a.tol and mid_ok and gap_rate > 0 and len_rate > 0
    detail = (f"gap_rate={gap_rate * 1000:.2f}px/s len_rate={len_rate * 1000:.2f}px/s "
              f"diff_over_{ref_ms}ms={diff_px:.2f}px (tol {a.tol}); "
              f"50%: gap={g5} len={l5} expected gap={eg5:.1f} len={el5:.1f}; " + detail)
    emit(ok, a.name, detail)


def cmd_reverse(a):
    """Animate back: pressed, released at t_release, captured at t_mid and at
    t_end (both since the press). At t_mid the crosshair must be PART way
    back on the Shrink model run backwards (gap within `tol` of the model,
    full length); at t_end it must be fully shown."""
    target = (a.r, a.g, a.b)
    mid = load(a.img_mid)
    end = load(a.img_end)
    g_m, l_m = gap_len(mid, a.cx, a.cy, 1, 0, target, a.tol_color, 200)
    g_e, l_e = gap_len(end, a.cx, a.cy, 1, 0, target, a.tol_color, 200)
    f_release = min(1.0, a.t_release / a.hide_ms)
    f_mid = max(0.0, f_release - (a.t_mid - a.t_release) / a.hide_ms)
    eg, el = shrink_model(f_mid * a.hide_ms, a.hide_ms, a.gap_px, a.len_px)
    mid_ok = (g_m is not None and abs(g_m - eg) <= a.tol and abs(l_m - el) <= a.tol
              and 0 < g_m < a.gap_px)   # intermediate: neither closed nor fully back
    end_ok = g_e == a.gap_px and l_e == a.len_px
    ok = mid_ok and end_ok
    detail = (f"mid t={a.t_mid}ms (released at {a.t_release}ms): gap={g_m} len={l_m} "
              f"expected gap={eg:.1f} len={el:.1f} (tol {a.tol}); end t={a.t_end}ms: gap={g_e} len={l_e} "
              f"expected {a.gap_px}/{a.len_px}")
    emit(ok, a.name, detail)


def coverage_of(px, color, bg):
    """The straight-alpha coverage that, blended in linear light over `bg`,
    gives `px` -- read off the channel where `color` differs most from
    the background. Inverse of the coverage blend alphamode.h applies to a
    HUD texel (crosshair::ResampleToOutput's contract)."""
    ch = max(range(3), key=lambda i: abs(color[i] - bg[i]))
    lo, hi, v = srgb_to_linear(bg[ch] / 255.0), srgb_to_linear(color[ch] / 255.0), srgb_to_linear(px[ch] / 255.0)
    if abs(hi - lo) < 1e-6:
        return 0.0
    return max(0.0, min(1.0, (v - lo) / (hi - lo)))


def cmd_scaled_axis(a):
    """Apply Scaling on, a game stretched by an integer factor: along the row
    (dx=1,dy=0) or column (dx=0,dy=1) through the centre, the >= 50 %-coverage
    runs must be exactly two arms, each `exp_len` long (+/- tol), their inner
    ends `exp_sep` apart (+/- tol), each `exp_width` wide across (+/- tol),
    and each arm must have a SOFT edge: a pixel of 10..90 % coverage just
    past either end and just beside it. `span` is how far from the centre
    to scan."""
    img = load(a.image)
    color, bg = (a.r, a.g, a.b), (a.bg_r, a.bg_g, a.bg_b)

    def cov_at(x, y):
        if not (0 <= x < img.width and 0 <= y < img.height):
            return 0.0
        return coverage_of(img.getpixel((x, y)), color, bg)

    covs = []
    for off in range(-a.span, a.span + 1):
        covs.append((off, cov_at(a.cx + a.dx * off, a.cy + a.dy * off)))
    runs, cur = [], None
    for off, c in covs:
        if c >= 0.5:
            if cur is None:
                cur = [off, off]
            else:
                cur[1] = off
        elif cur is not None:
            runs.append(tuple(cur)); cur = None
    if cur is not None:
        runs.append(tuple(cur))
    if len(runs) != 2:
        emit(False, a.name, f"expected 2 arms along the axis, found {len(runs)} runs: {runs}")
    (l0, l1), (r0, r1) = runs
    len_l, len_r = l1 - l0 + 1, r1 - r0 + 1
    sep = r0 - l1 - 1
    # width across each arm, at its middle
    def width_at(off):
        x, y = a.cx + a.dx * off, a.cy + a.dy * off
        px, py = a.dy, a.dx   # perpendicular
        n = 0
        for k in range(-a.span, a.span + 1):
            if cov_at(x + px * k, y + py * k) >= 0.5:
                n += 1
        return n
    w_l, w_r = width_at((l0 + l1) // 2), width_at((r0 + r1) // 2)
    # softness: a partial pixel past each end (along) and beside the middle (across)
    def soft(c):
        return 0.1 <= c <= 0.9
    ends = [cov_at(a.cx + a.dx * (l0 - 1), a.cy + a.dy * (l0 - 1)),
            cov_at(a.cx + a.dx * (l1 + 1), a.cy + a.dy * (l1 + 1)),
            cov_at(a.cx + a.dx * (r0 - 1), a.cy + a.dy * (r0 - 1)),
            cov_at(a.cx + a.dx * (r1 + 1), a.cy + a.dy * (r1 + 1))]
    def beside(off):
        x, y = a.cx + a.dx * off, a.cy + a.dy * off
        px, py = a.dy, a.dx
        ks = [k for k in range(-a.span, a.span + 1) if cov_at(x + px * k, y + py * k) >= 0.5]
        if not ks:
            return [0.0, 0.0]
        return [cov_at(x + px * (min(ks) - 1), y + py * (min(ks) - 1)),
                cov_at(x + px * (max(ks) + 1), y + py * (max(ks) + 1))]
    sides = beside((l0 + l1) // 2) + beside((r0 + r1) // 2)
    soft_ok = all(soft(c) for c in ends) and all(soft(c) for c in sides)
    ok = (abs(len_l - a.exp_len) <= a.tol and abs(len_r - a.exp_len) <= a.tol
          and abs(sep - a.exp_sep) <= a.tol
          and abs(w_l - a.exp_width) <= a.tol and abs(w_r - a.exp_width) <= a.tol
          and soft_ok)
    detail = (f"arms len={len_l}/{len_r} (exp {a.exp_len}) sep={sep} (exp {a.exp_sep}) "
              f"width={w_l}/{w_r} (exp {a.exp_width}) tol={a.tol}; "
              f"edge coverage past ends={[round(c, 2) for c in ends]} beside={[round(c, 2) for c in sides]} "
              f"(soft = 0.10..0.90){'' if soft_ok else ' NOT SOFT'}")
    emit(ok, a.name, detail)


def cmd_armscan(a):
    """Measurement only: prints gap and length along one ray (pixel path)."""
    img = load(a.image)
    g, l = gap_len(img, a.cx, a.cy, a.dx, a.dy, (a.r, a.g, a.b), a.tol, 200)
    emit(True, a.name, f"gap={g} len={l}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("pixel", help="print R G B at one pixel")
    sp.add_argument("image")
    sp.add_argument("x", type=int)
    sp.add_argument("y", type=int)
    sp.set_defaults(func=cmd_pixel)

    sp = sub.add_parser("digit", help="find a glyph's fill colour in a box and assert it")
    sp.add_argument("image")
    sp.add_argument("x0", type=int); sp.add_argument("y0", type=int)
    sp.add_argument("x1", type=int); sp.add_argument("y1", type=int)
    sp.add_argument("bg_r", type=int); sp.add_argument("bg_g", type=int); sp.add_argument("bg_b", type=int)
    sp.add_argument("diff_thresh", type=int)
    sp.add_argument("black_thresh", type=int)
    sp.add_argument("exp_r", type=int); sp.add_argument("exp_g", type=int); sp.add_argument("exp_b", type=int)
    sp.add_argument("tol", type=int)
    sp.add_argument("min_gap", type=float)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_digit)

    sp = sub.add_parser("blackcount", help="assert whether a box contains an outline-sized patch of black")
    sp.add_argument("image")
    sp.add_argument("x0", type=int); sp.add_argument("y0", type=int)
    sp.add_argument("x1", type=int); sp.add_argument("y1", type=int)
    sp.add_argument("bg_r", type=int); sp.add_argument("bg_g", type=int); sp.add_argument("bg_b", type=int)
    sp.add_argument("diff_thresh", type=int)
    sp.add_argument("black_thresh", type=int)
    sp.add_argument("min_count", type=int)
    sp.add_argument("expect_present", type=int)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_blackcount)

    sp = sub.add_parser("line", help="sample a ray of offsets from a centre point and assert their colour")
    sp.add_argument("image")
    sp.add_argument("cx", type=int); sp.add_argument("cy", type=int)
    sp.add_argument("dx", type=int); sp.add_argument("dy", type=int)
    sp.add_argument("start", type=int); sp.add_argument("end", type=int)
    sp.add_argument("mode", choices=["all", "any"])
    sp.add_argument("r", type=int); sp.add_argument("g", type=int); sp.add_argument("b", type=int)
    sp.add_argument("tol", type=int)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_line)

    sp = sub.add_parser("line_blend", help="as `line`, with the target computed from colour x opacity over the background "
                                           "through the HUD layer's premultiplied-then-coverage blend")
    sp.add_argument("image")
    sp.add_argument("cx", type=int); sp.add_argument("cy", type=int)
    sp.add_argument("dx", type=int); sp.add_argument("dy", type=int)
    sp.add_argument("start", type=int); sp.add_argument("end", type=int)
    sp.add_argument("mode", choices=["all", "any"])
    sp.add_argument("r", type=int); sp.add_argument("g", type=int); sp.add_argument("b", type=int)
    sp.add_argument("alpha", type=float)
    sp.add_argument("bg_r", type=int); sp.add_argument("bg_g", type=int); sp.add_argument("bg_b", type=int)
    sp.add_argument("bits", type=int, choices=[8, 16])
    sp.add_argument("tol", type=int)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_line_blend)

    sp = sub.add_parser("armscan", help="print gap and length along one ray (pixel path)")
    sp.add_argument("image")
    sp.add_argument("cx", type=int); sp.add_argument("cy", type=int)
    sp.add_argument("dx", type=int); sp.add_argument("dy", type=int)
    sp.add_argument("r", type=int); sp.add_argument("g", type=int); sp.add_argument("b", type=int)
    sp.add_argument("tol", type=int)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_armscan)

    sp = sub.add_parser("shrink_rate", help="Shrink hide: equal edge speed in both phases, and the 50 % state")
    for i in range(1, 6):
        sp.add_argument(f"img{i}"); sp.add_argument(f"t{i}", type=float)
    sp.add_argument("cx", type=int); sp.add_argument("cy", type=int)
    sp.add_argument("r", type=int); sp.add_argument("g", type=int); sp.add_argument("b", type=int)
    sp.add_argument("tol_color", type=int)
    sp.add_argument("hide_ms", type=float)
    sp.add_argument("gap_px", type=float); sp.add_argument("len_px", type=float)
    sp.add_argument("tol", type=float)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_shrink_rate)

    sp = sub.add_parser("reverse", help="Animate back: part way back at t_mid, fully shown at t_end")
    sp.add_argument("img_mid"); sp.add_argument("img_end")
    sp.add_argument("t_release", type=float); sp.add_argument("t_mid", type=float); sp.add_argument("t_end", type=float)
    sp.add_argument("cx", type=int); sp.add_argument("cy", type=int)
    sp.add_argument("r", type=int); sp.add_argument("g", type=int); sp.add_argument("b", type=int)
    sp.add_argument("tol_color", type=int)
    sp.add_argument("hide_ms", type=float)
    sp.add_argument("gap_px", type=float); sp.add_argument("len_px", type=float)
    sp.add_argument("tol", type=float)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_reverse)

    sp = sub.add_parser("scaled_axis", help="Apply Scaling: two arms of the expected stretched length/separation/width, with soft edges")
    sp.add_argument("image")
    sp.add_argument("cx", type=int); sp.add_argument("cy", type=int)
    sp.add_argument("dx", type=int); sp.add_argument("dy", type=int)
    sp.add_argument("span", type=int)
    sp.add_argument("r", type=int); sp.add_argument("g", type=int); sp.add_argument("b", type=int)
    sp.add_argument("bg_r", type=int); sp.add_argument("bg_g", type=int); sp.add_argument("bg_b", type=int)
    sp.add_argument("exp_len", type=float); sp.add_argument("exp_sep", type=float); sp.add_argument("exp_width", type=float)
    sp.add_argument("tol", type=float)
    sp.add_argument("name")
    sp.set_defaults(func=cmd_scaled_axis)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
