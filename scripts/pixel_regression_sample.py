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

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
