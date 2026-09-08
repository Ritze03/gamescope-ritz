#!/usr/bin/env python3
"""settings_audit.py -- the driver half of scripts/settings-audit.sh.

Never run this directly: settings-audit.sh takes the machine-wide gamescope
test lock, starts the private headless sway this needs, and hands over here
through the AUDIT_* environment variables it sets. See that script's header
for what the audit proves and scripts/README.md for how to read a failure.

WHAT THIS DOES, PER ROUTING SITUATION
  1. Seeds an isolated XDG_CONFIG_HOME (never the user's ~/.config) with the
     situation's global.json and profile files, launches gamescope against
     it, opens the overlay (so the dynamic areas -- Profiles, Mixer -- build
     their rows) and runs `overlay_e2_dump_keys`, the registry's own
     enumeration. Nothing here is a hand-written list of settings: a row
     added to any panel is audited on the next run.
  2. For every settable row: reads the live value (`overlay_e2_get`),
     computes a DIFFERENT valid value (a Switch flips; a Choice steps to the
     next option; a Slider/Stepper moves one step inside its range, away
     from both the current value and the default; a colour composite shifts
     its RGB; an anchor composite moves one row down; a hue composite turns
     37 degrees), writes it (`overlay_e2_set`, the same binding a click
     writes through), reads it back, then watches the config directory
     until the write lands and diffs every JSON file against the snapshot
     taken before the set. That diff is what proves WHICH file the value
     reached and that no other file -- and no other key -- moved.
  3. Restarts gamescope with the identical environment and flags, opens the
     overlay again and reads every row back: the value a user set has to be
     the value they get after a relaunch. The files are snapshotted before
     the stop and after the relaunch too, so a restart that rewrites a value
     is caught as well.
  4. Restores every original value and reads it back, so the restore path
     is exercised (and, for an inheriting game profile, shows the diff
     collapsing again when the value equals the parent's).

Everything is read through gamescope's own control protocol (gamescopectl)
-- no OS input, nothing visible on the desktop.
"""

import argparse
import copy
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Constants -- every threshold in one place
# ---------------------------------------------------------------------------
READY_TIMEOUT_S = 25          # gamescope reporting its display + answering a ConCommand
STOP_TIMEOUT_S = 10           # SIGTERM -> SIGKILL
WRITE_LAND_TIMEOUT_S = 2.5    # the coalescing writer: 50 ms quiet, 500 ms cap (ConfigManager.h)
WRITE_SETTLE_S = 0.25         # after the first file changes, wait for a sibling file/key
SETTLE_AFTER_LAUNCH_S = 1.5   # startup writes (Default creation, --profile copy, geometry) to land
FLOAT_TOL = 2e-3              # ValueToString() prints "%.4g": 4 significant digits
CTL_TIMEOUT_S = 15

APP_ID = "730"
GENERAL_PROFILE = "Base"
GAME_PROFILE = "CS2"
FORCED_PROFILE = "Tourney"

# The three routing situations the brief asks for.
SITUATIONS = {
    "a": dict(
        title="no game identified; editing the general profile '%s'" % GENERAL_PROFILE,
        env={},
        args=[],
        session=GENERAL_PROFILE,
        parent=None,
    ),
    "b": dict(
        title="game %s identified; its game profile '%s' inherits '%s'" % (APP_ID, GAME_PROFILE, GENERAL_PROFILE),
        env={"STEAM_COMPAT_APP_ID": APP_ID},
        args=[],
        session=GAME_PROFILE,
        parent=GENERAL_PROFILE,
    ),
    "c": dict(
        title="game %s identified but --profile %s forces the session" % (APP_ID, FORCED_PROFILE),
        env={"STEAM_COMPAT_APP_ID": APP_ID},
        args=["--profile", FORCED_PROFILE],
        session=FORCED_PROFILE,
        parent=None,
    ),
}

# Rows the round-trip cannot exercise, and why. Kinds that are never a
# setting (facts, meter, action, bank, text) are excluded by kind with a
# generic reason; these are the bound, settable-looking ids that still are
# not a persisted setting. Each names what covers it instead.
NOT_COVERED = {
    "profiles.list": "selecting a line is SelectProfile() -- it switches the session, not a value; "
                     "tests/test_config.cpp pins select/assignment",
    "profiles.inherits": "edits the profile's own metadata (EditProfileMeta), not a settings key; "
                         "tests/test_config.cpp pins it",
    "audio.stream": "PipeWire stream picker; the harness has no audio server; its pick is "
                    "global.json games.<id>.audio_node (SetGameAudioNode), tests/test_config.cpp",
    "audio.stream.volume": "a live PipeWire node volume, not a config value",
    "audio.stream.volume.mute": "a live PipeWire node mute, not a config value",
    "log.autoscroll": "Log view state; deliberately not persisted (profiles.md: the Log's rows "
                      "resolve to no key)",
    # Keybinds (2026-09-08). These ARE persisted -- global.json's
    # overlay.keybinds -- but the generic round trip cannot exercise them:
    # "a different valid value" for a chord is not a step or a next option,
    # and any string that is not a chord is refused by design
    # (src/Keybinds.cpp's SetChord). Naming them here rather than letting the
    # blanket "text" reason below claim them, because that reason ("Log view
    # state, deliberately not persisted") would be false for these.
    "keybinds.shell": "a key chord, not a value with a next step; the round trip is pinned by "
                      "tests/test_keybinds.cpp and, live, by "
                      "build-release/verify-shots/keybinds-2026-09-08/",
    "keybinds.shell_alt": "as keybinds.shell",
    "keybinds.launcher": "as keybinds.shell",
    "keybinds.companion": "as keybinds.shell",
    "keybinds.friends": "as keybinds.shell",
    # The friends list (2026-09-08). Bound to an int index, but that index is
    # not a persisted value: selecting a line ACTS on it (it joins, or says
    # why it cannot), the list's contents come from the running Steam client
    # rather than from a config file, and the harness has no Steam. Covered by
    # tests/test_steam_friends.cpp and, live, by
    # build-release/verify-shots/steam-friends-phase345-2026-09-08/.
    "friends.list": "selecting a line asks Steam to join that friend -- it acts, it does not "
                    "store a value, and the rows come from the Steam client rather than from "
                    "a config file; tests/test_steam_friends.cpp and "
                    "build-release/verify-shots/steam-friends-phase345-2026-09-08/ cover it",
    # The Steam chat overlay's two free-text rows (2026-09-08). Persisted --
    # global.json's overlay.companion_command / companion_url -- but there is
    # no "next value" for a browser command line, so the generic round trip
    # cannot exercise them either. Named here rather than left to the blanket
    # "text" reason, which claims the value is view state and is not persisted.
    "overlay.companion_command": "a browser command line, not a value with a next step; the "
                                 "grammar is pinned by tests/test_steam_companion.cpp and, live, "
                                 "by build-release/verify-shots/steam-companion-2026-09-08/",
    "overlay.companion_url": "as overlay.companion_command",
}
KIND_NOT_COVERED = {
    "facts": "read-only",
    "meter": "read-only",
    "action": "an Action has no value",
    "bank": "Log view state (bit set); deliberately not persisted",
    "text": "needs typed input. The Log's filter is view state and deliberately not "
            "persisted; the Keybinds rows ARE persisted and are named individually in "
            "NOT_COVERED above with what covers them",
}

# Gate-openers: a row whose SETTER only applies while another row is in a
# given state. The generic "next option" rule would leave the gate shut and
# the row would look unpersisted for a reason the UI already greys it for.
# Each entry is (id-to-set, option LABEL to pick), resolved against the
# registry's own option list at run time. Deliberately tiny and explicit.
GATES = {
    "display.resolution.size": [("display.resolution.aspect", "16:9")],   # a size list exists only for a named shape
    "display.resolution.width": [("display.resolution.aspect", "Custom")],  # SetCustomWidth() applies only while Custom
    "display.resolution.height": [("display.resolution.aspect", "Custom")],
    "display.refresh.custom": [("display.refresh", "Custom")],             # SetCustomRefreshHz() applies only while Custom
}

# A row whose own write legitimately moves a second key.
SIBLING_KEYS = {
    "display.resolution.width": {"gamescope.nested_height"},   # lock_aspect on: the height follows
    "display.resolution.height": {"gamescope.nested_width"},
    "display.resolution.size": {"gamescope.nested_width", "gamescope.nested_height"},
    "display.resolution.aspect": {"gamescope.nested_width", "gamescope.nested_height"},
    # Adaptive Brightness and Adaptive Gamma are mutually exclusive (2026-09-08):
    # both aim the frame's mid-tones at a Target from the same statistics, so
    # turning either ON turns the other OFF. That cross-write is the feature --
    # see superdoc/features/shader-effects.md's "Adaptive Gamma vs Adaptive
    # Brightness" -- and only ever happens on the ON edge, so declaring it here
    # keeps the audit a real gate instead of reporting the same intentional
    # write as a clobber on every future run.
    "image.shaders.adaptive_brightness": {"reshade.adaptive_gamma.enabled"},
    "image.shaders.adaptive_gamma": {"reshade.adaptive_brightness.enabled"},
}

ANCHOR_NAMES = [
    ["top-left", "top", "top-right"],
    ["left", "centre", "right"],
    ["bottom-left", "bottom", "bottom-right"],
]

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def log(msg):
    sys.stderr.write("[settings-audit] %s\n" % msg)
    sys.stderr.flush()


def flatten(obj, prefix=""):
    out = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            key = prefix + "." + k if prefix else k
            if isinstance(v, dict):
                out.update(flatten(v, key))
            else:
                out[key] = v
        if not obj and prefix:
            out[prefix] = {}
    return out


def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


class ConfigHome:
    """The isolated config directory and its snapshots."""

    def __init__(self, root):
        self.root = root
        self.dir = os.path.join(root, "gamescope-ritz")

    def files(self):
        out = []
        for base, _dirs, names in os.walk(self.dir):
            for n in names:
                if n.endswith(".json"):
                    p = os.path.join(base, n)
                    out.append(os.path.relpath(p, self.dir))
        return sorted(out)

    def snapshot(self):
        snap = {}
        for rel in self.files():
            j = read_json(os.path.join(self.dir, rel))
            snap[rel] = flatten(j) if j is not None else {"<unreadable>": True}
        return snap

    @staticmethod
    def diff(before, after):
        """{file: {key: (before, after)}} for every key that appeared, vanished or changed."""
        out = {}
        for rel in sorted(set(before) | set(after)):
            a = before.get(rel, {})
            b = after.get(rel, {})
            changed = {}
            for k in sorted(set(a) | set(b)):
                va, vb = a.get(k, None), b.get(k, None)
                if (k in a) != (k in b) or va != vb:
                    changed[k] = (va if k in a else "<absent>", vb if k in b else "<absent>")
            if changed:
                out[rel] = changed
        return out

    def copy_to(self, dest):
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        shutil.copytree(self.dir, dest)


# ---------------------------------------------------------------------------
# The gamescope instance and its control socket
# ---------------------------------------------------------------------------
class Instance:
    def __init__(self, binary, ctl, rundir, sway_socket, cfg, out_dir, backend):
        self.binary = binary
        self.ctl_bin = ctl
        self.rundir = rundir
        self.sway_socket = sway_socket
        self.cfg = cfg
        self.out_dir = out_dir
        self.backend = backend
        self.proc = None
        self.log_path = None
        self.display = None
        self.launches = 0

    def base_env(self, extra):
        env = dict(os.environ)
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "STEAM_COMPAT_APP_ID", "SteamAppId",
                  "STEAM_COMPAT_DATA_PATH", "GS_RITZ_PROFILE", "GAMESCOPE_WAYLAND_DISPLAY"):
            env.pop(k, None)
        env["XDG_RUNTIME_DIR"] = self.rundir
        env["XDG_CONFIG_HOME"] = self.cfg.root
        env["DISABLE_LSFG"] = "1"
        if self.sway_socket:
            env["WAYLAND_DISPLAY"] = self.sway_socket
        env.update(extra)
        return env

    def start(self, label, extra_env, extra_args):
        assert self.proc is None
        self.launches += 1
        self.log_path = os.path.join(self.out_dir, "gamescope-%s-%d.log" % (label, self.launches))
        # No -w/-h on purpose: main.cpp seeds the nested size and refresh
        # from the profile BEFORE getopt so an explicit CLI flag still wins
        # (ConfigSchema.h, nested_width) -- passing them here would make the
        # Resolution rows look unpersisted on the restart leg when they are
        # merely overridden, by design. -W/-H (the output window) is never
        # persisted, so it is safe to pin.
        # A client is needed: without one nothing is composited, the overlay
        # layer never draws a frame, and the dynamic areas (Profiles, Mixer)
        # -- which build on the shell's first draw -- never get their rows.
        # kitty with a flat background is pixel-regression.sh's own client.
        argv = [self.binary, "--backend", self.backend, "-W", "1280", "-H", "720"] + list(extra_args) + [
            "--", "kitty", "-c", "NONE", "-o", "background=#333333", "-o", "foreground=#333333",
            "-o", "cursor=#333333", "-o", "remember_window_size=no", "sleep", "600"]
        env = self.base_env(extra_env)
        log("launch %s: %s" % (label, " ".join(argv[1:])))
        logf = open(self.log_path, "wb")
        # start_new_session: our own process group, so a stop never reaches
        # anything but the PID we started. close_fds drops the lock's fd 9.
        self.proc = subprocess.Popen(argv, env=env, stdout=logf, stderr=subprocess.STDOUT,
                                     close_fds=True, start_new_session=True)
        logf.close()
        self.display = None
        deadline = time.time() + READY_TIMEOUT_S
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("gamescope exited during startup (see %s)" % self.log_path)
            try:
                with open(self.log_path, "r", errors="replace") as f:
                    m = re.search(r"wayland display '([^']+)'", f.read())
            except OSError:
                m = None
            if m:
                self.display = m.group(1)
                break
            time.sleep(0.1)
        if not self.display:
            raise RuntimeError("gamescope never reported its wayland display (see %s)" % self.log_path)
        # Then wait until the registry answers -- a ConCommand round trip is
        # the readiness signal, never a fixed sleep.
        while time.time() < deadline:
            out = self.ctl("overlay_e2_get", "hud.enabled")
            if out and out.startswith("hud.enabled"):
                log("ready: pid %d, socket %s" % (self.proc.pid, self.display))
                return
            time.sleep(0.1)
        raise RuntimeError("gamescope never answered overlay_e2_get (see %s)" % self.log_path)

    def ctl(self, *args):
        env = dict(os.environ)
        env["XDG_RUNTIME_DIR"] = self.rundir
        env["GAMESCOPE_WAYLAND_DISPLAY"] = self.display or ""
        try:
            r = subprocess.run([self.ctl_bin] + list(args), env=env, capture_output=True,
                               text=True, timeout=CTL_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return ""
        return (r.stdout or "") + (r.stderr or "")

    def stop(self):
        if self.proc is None:
            return
        p = self.proc
        self.proc = None
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                log("SIGTERM ignored for %ds, SIGKILL pid %d" % (STOP_TIMEOUT_S, p.pid))
                p.kill()
                p.wait()
        log("stopped (exit %s)" % p.returncode)


# ---------------------------------------------------------------------------
# Registry rows (from overlay_e2_dump_keys) and values
# ---------------------------------------------------------------------------
GET_RE = re.compile(r"^(\S+)\s+(\S+)\s*(.*)$")


def parse_dump(text):
    cols = None
    rows = []
    for line in text.splitlines():
        if line.startswith("#area"):
            cols = line[1:].split("\t")
            continue
        if line.startswith("#") or not cols or "\t" not in line:
            continue
        cells = line.split("\t")
        if len(cells) != len(cols):
            continue
        row = dict(zip(cols, cells))
        for k in list(row):
            if row[k] == "-":
                row[k] = ""
        opts = []
        if row["options"]:
            for item in row["options"].split("|"):
                v, _, label = item.partition("=")
                opts.append((int(v), label))
        row["opts"] = opts
        rows.append(row)
    return rows


def parse_get(text):
    for line in text.splitlines():
        m = GET_RE.match(line.strip())
        if m:
            return m.group(1), m.group(2), m.group(3).strip()
    return None, None, None


def strip_unit(text, unit):
    t = text.strip()
    if unit and t.endswith(unit):
        t = t[: -len(unit)].strip()
    return t


def parse_num(text):
    try:
        return float(text.replace(",", "."))
    except ValueError:
        return None


def anchor_of(text):
    name = text.split()[0] if text.strip() else ""
    for v, rowl in enumerate(ANCHOR_NAMES):
        for h, n in enumerate(rowl):
            if n == name:
                return v, h
    return None


def num_close(a, b):
    if a is None or b is None:
        return False
    return abs(a - b) <= max(FLOAT_TOL, FLOAT_TOL * abs(b))


def fmt_num(x, is_int):
    if is_int:
        return str(int(round(x)))
    s = "%.4f" % x
    s = s.rstrip("0").rstrip(".")
    return s if s not in ("", "-") else "0"


class Plan:
    """What to set for one row, and what the readback must say afterwards."""

    def __init__(self, set_arg, expect_text, matcher, note=""):
        self.set_arg = set_arg
        self.expect_text = expect_text
        self.matcher = matcher
        self.note = note


def plan_for(row, cur_text):
    """A different, valid value for `row` given its current readback text."""
    kind = row["kind"]
    unit = row["unit"]
    if kind == "switch":
        cur = cur_text.strip() == "on"
        new = "off" if cur else "on"
        return Plan(new, new, lambda got: got.strip() == new)
    if kind == "choice":
        opts = row["opts"]
        if not opts:
            return None
        idx = next((i for i, (_v, l) in enumerate(opts) if l == cur_text.strip()), None)
        if idx is None:
            return None
        v, label = opts[(idx + 1) % len(opts)]
        return Plan(str(v), label, lambda got: got.strip() == label)
    if kind in ("slider", "stepper"):
        is_int = row["type_a"] == "int"
        cur = parse_num(strip_unit(cur_text, unit))
        if cur is None or not row["lo"] or not row["hi"]:
            return None
        lo, hi = float(row["lo"]), float(row["hi"])
        step = float(row["step"]) if row["step"] else (1.0 if is_int else (hi - lo) / 20.0)
        default = parse_num(strip_unit(row["default"], unit)) if row["default"] else None
        for k in (1, -1, 2, -2, 3, -3, 5, -5, 10, -10):
            cand = cur + k * step
            if is_int:
                cand = round(cand)
            if cand < lo - 1e-9 or cand > hi + 1e-9:
                continue
            if num_close(cand, cur) or (default is not None and num_close(cand, default)):
                continue
            text = fmt_num(cand, is_int)
            return Plan(text, text + (" " + unit if unit else ""),
                        lambda got, c=cand: num_close(parse_num(strip_unit(got, unit)), c))
        return None
    if kind == "composite":
        comp = row["composite"]
        if comp == "color":
            m = re.match(r"#([0-9A-Fa-f]{6})", cur_text.strip())
            if not m:
                return None
            cur = int(m.group(1), 16)
            new = (cur + 0x123456) & 0xFFFFFF
            default = int(row["default"]) if row["default"].lstrip("-").isdigit() else None
            if default is not None and new == default:
                new = (new + 1) & 0xFFFFFF
            exp = "#%06X" % new
            return Plan(str(new), exp, lambda got: got.strip().upper().startswith(exp))
        if comp == "anchor":
            vh = anchor_of(cur_text)
            if vh is None:
                return None
            v, h = vh
            nv = (v + 1) % 3
            exp = ANCHOR_NAMES[nv][h]
            return Plan(str(nv), exp, lambda got: got.split()[0] == exp if got.strip() else False)
        if comp == "hue":
            cur = parse_num(cur_text.strip().rstrip("°"))
            if cur is None:
                return None
            new = (int(round(cur)) + 37) % 360
            exp = "%d°" % new
            return Plan(str(new), exp,
                        lambda got: num_close(parse_num(got.strip().rstrip("°")), new))
        return None
    return None


def restore_plan(row, orig_text):
    """The set argument that puts `row` back to `orig_text`, and its matcher."""
    kind = row["kind"]
    unit = row["unit"]
    if kind == "switch":
        v = orig_text.strip()
        return Plan(v, v, lambda got: got.strip() == v)
    if kind == "choice":
        for value, label in row["opts"]:
            if label == orig_text.strip():
                return Plan(str(value), label, lambda got, l=label: got.strip() == l)
        return None
    if kind in ("slider", "stepper"):
        n = parse_num(strip_unit(orig_text, unit))
        if n is None:
            return None
        text = fmt_num(n, row["type_a"] == "int")
        return Plan(text, orig_text, lambda got: num_close(parse_num(strip_unit(got, unit)), n))
    if kind == "composite":
        comp = row["composite"]
        if comp == "color":
            m = re.match(r"#([0-9A-Fa-f]{6})", orig_text.strip())
            if not m:
                return None
            n = int(m.group(1), 16)
            exp = "#%06X" % n
            return Plan(str(n), exp, lambda got: got.strip().upper().startswith(exp))
        if comp == "anchor":
            vh = anchor_of(orig_text)
            if vh is None:
                return None
            exp = ANCHOR_NAMES[vh[0]][vh[1]]
            return Plan(str(vh[0]), exp, lambda got: got.split()[0] == exp if got.strip() else False)
        if comp == "hue":
            n = parse_num(orig_text.strip().rstrip("°"))
            if n is None:
                return None
            return Plan(str(int(round(n))), orig_text,
                        lambda got: num_close(parse_num(got.strip().rstrip("°")), n))
    return None


def scope_of(row):
    """'global' rows must land in global.json's overlay section and nowhere else."""
    i = row["id"]
    if i.startswith("overlay.") or i.startswith("cursor.") or i.startswith("profiles."):
        return "global"
    return "profile"


def settable(row):
    if row["bound"] != "yes" or row["readonly"] == "yes":
        return False
    return row["kind"] in ("switch", "slider", "stepper", "choice", "composite")


def json_literal_matches(plan, row, disk_value):
    """Whether the on-disk JSON value is literally the value we set (bool/number only)."""
    if isinstance(disk_value, bool):
        return plan.set_arg in ("on", "off") and disk_value == (plan.set_arg == "on")
    if isinstance(disk_value, (int, float)) and row["kind"] in ("slider", "stepper"):
        n = parse_num(plan.set_arg)
        return n is not None and num_close(float(disk_value), n)
    if isinstance(disk_value, (int, float)) and row["kind"] == "composite" and row["composite"] == "color":
        return int(disk_value) == int(plan.set_arg)
    return False


# ---------------------------------------------------------------------------
# One routing situation
# ---------------------------------------------------------------------------
class Result:
    def __init__(self, sit, row):
        self.sit = sit
        self.id = row["id"]
        self.key = row["resolved_key"] if row["settings_key"] == "yes" else (row["declared_key"] or "(none)")
        self.kind = row["kind"] + ("/" + row["composite"] if row["composite"] else "")
        self.set_value = ""
        self.live = "-"
        self.disk = "-"
        self.file = "-"
        self.restart = "-"
        self.restore = "-"
        self.notes = []
        self.covered = True
        self.reason = ""
        self.enabled_at_set = ""

    def verdict(self):
        if not self.covered:
            return "NOT COVERED"
        cells = (self.live, self.disk, self.file, self.restart)
        if any(c.startswith("FAIL") for c in cells):
            return "FAIL"
        return "PASS"


def seed_config(cfg, sit_key):
    """A fresh isolated config home for the situation."""
    if os.path.isdir(cfg.dir):
        shutil.rmtree(cfg.dir)
    os.makedirs(os.path.join(cfg.dir, "profiles"))
    games = {}
    if sit_key in ("b", "c"):
        games[APP_ID] = {"selected": GAME_PROFILE, "audio_node": ""}
    with open(os.path.join(cfg.dir, "global.json"), "w") as f:
        json.dump({"schema_version": 3, "profiles": {"last_general": GENERAL_PROFILE, "games": games}},
                  f, indent=2)
    with open(os.path.join(cfg.dir, "profiles", GENERAL_PROFILE + ".json"), "w") as f:
        json.dump({"schema_version": 3, "name": GENERAL_PROFILE, "kind": "general"}, f, indent=2)
    if sit_key in ("b", "c"):
        with open(os.path.join(cfg.dir, "profiles", GAME_PROFILE + ".json"), "w") as f:
            json.dump({"schema_version": 3, "name": GAME_PROFILE, "kind": "game", "app_id": APP_ID,
                       "game_name": "Counter-Strike 2", "inherits": GENERAL_PROFILE}, f, indent=2)


def wait_for_write(cfg, before):
    """Poll until any JSON file differs from `before`, then let siblings settle."""
    deadline = time.time() + WRITE_LAND_TIMEOUT_S
    t0 = time.time()
    while time.time() < deadline:
        now = cfg.snapshot()
        if ConfigHome.diff(before, now):
            time.sleep(WRITE_SETTLE_S)
            return cfg.snapshot(), time.time() - t0
        time.sleep(0.05)
    return cfg.snapshot(), None


def current_profile(inst):
    out = inst.ctl("ritz_profile")
    m = re.search(r"current: '([^']*)'", out)
    return m.group(1) if m else ""


def open_overlay(inst):
    inst.ctl("settings_overlay_visible", "1")
    # The dynamic areas build on the shell's first draw; give it a few frames.
    deadline = time.time() + 5
    while time.time() < deadline:
        out = inst.ctl("overlay_e2_get", "profiles.filter")
        if out.startswith("profiles.filter"):
            return True
        time.sleep(0.1)
    return False


def run_situation(sit_key, inst, cfg, out_dir, only_ids, steps):
    sit = SITUATIONS[sit_key]
    sit_dir = os.path.join(out_dir, "situation-" + sit_key)
    os.makedirs(sit_dir, exist_ok=True)
    log("=== situation %s: %s" % (sit_key, sit["title"]))
    seed_config(cfg, sit_key)

    session_file = "profiles/%s.json" % sit["session"]
    parent_file = ("profiles/%s.json" % sit["parent"]) if sit["parent"] else None
    other_profile_files = set()
    for name in (GENERAL_PROFILE, GAME_PROFILE, FORCED_PROFILE):
        rel = "profiles/%s.json" % name
        if rel != session_file:
            other_profile_files.add(rel)

    inst.start("sit%s" % sit_key, sit["env"], sit["args"])
    time.sleep(SETTLE_AFTER_LAUNCH_S)
    prof = current_profile(inst)
    header = {"situation": sit_key, "title": sit["title"], "session_profile_reported": prof,
              "session_profile_expected": sit["session"]}
    steps.append({"step": "launch", **header})
    if prof != sit["session"]:
        log("WARNING: the session profile is '%s', expected '%s'" % (prof, sit["session"]))
    overlay_ok = open_overlay(inst)
    if not overlay_ok:
        log("WARNING: the overlay did not open (dynamic areas may be missing from the dump)")

    dump_text = inst.ctl("overlay_e2_dump_keys")
    with open(os.path.join(sit_dir, "dump.tsv"), "w") as f:
        f.write(dump_text)
    rows = parse_dump(dump_text)
    if not rows:
        raise RuntimeError("overlay_e2_dump_keys returned nothing -- is this binary built with it?")
    by_id = {r["id"]: r for r in rows}

    # Priming write: a no-op set makes the session profile's file complete
    # (a general profile is written whole), so the baseline snapshot does
    # not show every key "appearing" on the first real edit.
    # The same for global.json's `overlay` section (the Appearance area's
    # first write fills the whole section in, which would otherwise be
    # credited to whichever overlay.* row happens to be audited first).
    for prime_id in ("hud.enabled", "overlay.display_scale"):
        _i, _k, cur = parse_get(inst.ctl("overlay_e2_get", prime_id))
        if cur is not None and prime_id in by_id:
            plan = restore_plan(by_id[prime_id], cur)
            if plan:
                inst.ctl("overlay_e2_set", "%s %s" % (prime_id, plan.set_arg))
    time.sleep(1.0)
    baseline = cfg.snapshot()
    cfg.copy_to(os.path.join(sit_dir, "cfg-baseline"))

    results = []
    originals = {}     # id -> readback text before the audit touched it
    final_live = {}    # id -> readback text after every set (pre-restart)
    landed = {}        # (file, key) -> (value this audit put there, id that put it)
    result_by_id = {}

    def set_and_get(row_id, arg):
        inst.ctl("overlay_e2_set", "%s %s" % (row_id, arg))
        _i, _k, v = parse_get(inst.ctl("overlay_e2_get", row_id))
        return v if v is not None else ""

    def label_value(row_id, label):
        for v, l in by_id[row_id]["opts"]:
            if l == label:
                return str(v)
        return None

    audited = []
    missing = sorted(only_ids - set(by_id)) if only_ids else []
    if missing:
        log("WARNING: --only names ids the registry did not list: %s" % ", ".join(missing))
        steps.append({"step": "only-missing", "situation": sit_key, "ids": missing})
    for row in rows:
        r = Result(sit_key, row)
        if only_ids and row["id"] not in only_ids:
            continue
        # An id-specific reason wins over the blanket kind one: a kind that
        # is usually not a setting can still contain rows that are (the
        # Keybinds chords are Kind::Text and genuinely persisted), and
        # reporting those under the kind's generic reason would print
        # something false about them.
        if row["id"] in NOT_COVERED:
            r.covered = False
            r.reason = NOT_COVERED[row["id"]]
            results.append(r)
            continue
        if row["kind"] in KIND_NOT_COVERED:
            r.covered = False
            r.reason = KIND_NOT_COVERED[row["kind"]]
            results.append(r)
            continue
        if row["id"].startswith("audio."):
            r.covered = False
            r.reason = "PipeWire-backed Mixer row; no audio server in the harness"
            results.append(r)
            continue
        if not settable(row):
            r.covered = False
            r.reason = "unbound or read-only"
            results.append(r)
            continue
        if row["area_available"] != "yes":
            r.covered = False
            r.reason = "area not available under this backend (AvailableWhen)"
            results.append(r)
            continue
        results.append(r)
        audited.append((row, r))
        result_by_id[row["id"]] = r

    # ---- the set pass ------------------------------------------------------
    for row, r in audited:
        rid = row["id"]
        # gate-openers first, then a fresh baseline so their own writes are
        # not credited to this row
        for gate_id, gate_label in GATES.get(rid, []):
            if gate_id in by_id:
                gv = label_value(gate_id, gate_label)
                if gv is not None:
                    inst.ctl("overlay_e2_set", "%s %s" % (gate_id, gv))
                    r.notes.append("gate: %s=%s" % (gate_id, gate_label))
            time.sleep(0.8)
        before = cfg.snapshot()

        _i, _k, cur = parse_get(inst.ctl("overlay_e2_get", rid))
        cur = cur if cur is not None else ""
        # re-read the row's enabled state after the gates
        dump_now = parse_dump(inst.ctl("overlay_e2_dump_keys"))
        now_row = next((x for x in dump_now if x["id"] == rid), row)
        r.enabled_at_set = "enabled" if not now_row["disabled_reason"] else "disabled: " + now_row["disabled_reason"]
        originals[rid] = cur
        plan = plan_for(now_row, cur)
        if plan is None:
            r.live = "FAIL: could not compute a value (current '%s')" % cur
            steps.append({"step": "plan-failed", "situation": sit_key, "id": rid, "current": cur})
            continue
        r.set_value = plan.set_arg + (" (expect '%s')" % plan.expect_text if plan.expect_text != plan.set_arg else "")

        got = set_and_get(rid, plan.set_arg)
        live_ok = plan.matcher(got)
        r.live = "OK" if live_ok else "FAIL: got '%s', expected '%s'" % (got, plan.expect_text)

        after, latency = wait_for_write(cfg, before)
        d = ConfigHome.diff(before, after)
        entry = {"step": "set", "situation": sit_key, "id": rid, "key": r.key, "before": cur,
                 "set": plan.set_arg, "got": got, "live_ok": live_ok, "latency_s": latency,
                 "diff": {k: {kk: list(vv) for kk, vv in v.items()} for k, v in d.items()},
                 "enabled": r.enabled_at_set}
        steps.append(entry)

        scope = scope_of(row)
        expected_file = "global.json" if scope == "global" else session_file
        own = d.get(expected_file, {})
        keyed = row["settings_key"] == "yes"
        own_keys = set(own)
        allowed = {row["resolved_key"]} | SIBLING_KEYS.get(rid, set())
        if keyed:
            if row["resolved_key"] in own:
                b, a = own[row["resolved_key"]]
                literal = json_literal_matches(plan, row, a)
                r.disk = "OK" if literal else "OK (%s -> %s)" % (json.dumps(b), json.dumps(a))
                if not literal and row["kind"] not in ("choice",) and not (row["kind"] == "composite" and row["composite"] == "anchor"):
                    r.notes.append("disk value is a transform of the UI value")
            else:
                r.disk = "FAIL: %s unchanged in %s" % (row["resolved_key"], expected_file)
            extra = own_keys - allowed
            # a general-profile write may also fill in a key the seed file
            # lacked (the priming write already did that); anything else is
            # a collateral edit of another setting
            if extra:
                r.notes.append("collateral keys in %s: %s" % (expected_file, ", ".join(sorted(extra))))
        else:
            if own:
                r.disk = "OK (no key declared; landed at %s)" % ", ".join(
                    "%s=%s" % (k, json.dumps(v[1])) for k, v in sorted(own.items()))
            else:
                r.disk = "FAIL: nothing changed in %s (no key declared)" % expected_file
        # correct file: nothing moved anywhere else
        wrong = {}
        for rel, changed in d.items():
            if rel == expected_file:
                if scope == "global":
                    bad = [k for k in changed if not k.startswith("overlay.")]
                    if bad:
                        wrong[rel] = bad
                continue
            wrong[rel] = sorted(changed)
        if parent_file and parent_file in d:
            wrong[parent_file] = sorted(d[parent_file])
        # A write that puts back a value an EARLIER row set is a stale-copy
        # clobber (the class of bug the routed-write merge fixed for profile
        # files): this row's panel wrote its whole section from a copy it
        # loaded before the other rows' edits. Reported here, on the row
        # that did the clobbering, and noted on every row it undid.
        clobbered = []
        for rel, changed in d.items():
            for k, (b, a) in changed.items():
                if k in allowed and rel == expected_file:
                    continue
                prev = landed.get((rel, k))
                if prev is not None and prev[1] != rid and a != prev[0]:
                    clobbered.append("%s:%s (set by %s to %s, now %s)" % (rel, k, prev[1], json.dumps(prev[0]), json.dumps(a)))
                    if prev[1] in result_by_id:
                        result_by_id[prev[1]].notes.append("later reverted on disk by %s" % rid)
        if wrong:
            r.file = "FAIL: also changed " + "; ".join("%s: %s" % (rel, ", ".join(ks)) for rel, ks in sorted(wrong.items()))
        elif clobbered:
            r.file = "FAIL: clobbered earlier edits: " + "; ".join(clobbered)
        elif not own and r.disk.startswith("FAIL"):
            r.file = "-"
        else:
            r.file = "OK"
        for k, (b, a) in own.items():
            landed[(expected_file, k)] = (a, rid)
        if sit["parent"] and keyed and row["resolved_key"] in own:
            r.notes.append("stored as a diff in the child")

    # the final live state, read once more after every set (rows interact)
    for row, r in audited:
        _i, _k, v = parse_get(inst.ctl("overlay_e2_get", row["id"]))
        final_live[row["id"]] = v if v is not None else ""
    time.sleep(1.0)
    pre_stop = cfg.snapshot()
    cfg.copy_to(os.path.join(sit_dir, "cfg-after-set"))

    # ---- restart ------------------------------------------------------------
    inst.stop()
    post_stop = cfg.snapshot()
    d = ConfigHome.diff(pre_stop, post_stop)
    if d:
        steps.append({"step": "shutdown-wrote", "situation": sit_key, "diff": {k: {kk: list(vv) for kk, vv in v.items()} for k, v in d.items()}})
        log("note: shutdown changed %s" % ", ".join(sorted(d)))
    inst.start("sit%s-restart" % sit_key, sit["env"], sit["args"])
    time.sleep(SETTLE_AFTER_LAUNCH_S)
    prof2 = current_profile(inst)
    if prof2 != sit["session"]:
        log("WARNING: after restart the session profile is '%s', expected '%s'" % (prof2, sit["session"]))
    open_overlay(inst)
    for row, r in audited:
        if r.live.startswith("FAIL: could not"):
            r.restart = "-"
            continue
        _i, _k, v = parse_get(inst.ctl("overlay_e2_get", row["id"]))
        v = v if v is not None else ""
        want = final_live[row["id"]]
        # numeric readbacks compare as numbers (a "%.4g" print is stable, but
        # be tolerant of a trailing unit or spacing)
        ok = v.strip() == want.strip()
        if not ok and row["kind"] in ("slider", "stepper"):
            ok = num_close(parse_num(strip_unit(v, row["unit"])), parse_num(strip_unit(want, row["unit"])))
        r.restart = "OK" if ok else "FAIL: got '%s' after restart, was '%s' before" % (v, want)
        steps.append({"step": "restart-get", "situation": sit_key, "id": row["id"], "before_restart": want, "after_restart": v, "ok": ok})
    # A second read of every row that failed, after every other row has been
    # read once: a getter that reads a compositor global which only some
    # OTHER row's load pushes ("late apply") answers the saved value now and
    # not before -- which is its own finding, distinct from "never restored".
    for row, r in audited:
        if not r.restart.startswith("FAIL"):
            continue
        _i, _k, v = parse_get(inst.ctl("overlay_e2_get", row["id"]))
        v = v if v is not None else ""
        want = final_live[row["id"]]
        ok = v.strip() == want.strip()
        if not ok and row["kind"] in ("slider", "stepper"):
            ok = num_close(parse_num(strip_unit(v, row["unit"])), parse_num(strip_unit(want, row["unit"])))
        if ok:
            r.notes.append("late apply: read '%s' right after the relaunch, '%s' once other rows had been read" % (
                r.restart.split("'")[1], v))
            steps.append({"step": "restart-get-late", "situation": sit_key, "id": row["id"], "late_value": v})
    time.sleep(1.0)
    post_restart = cfg.snapshot()
    d = ConfigHome.diff(post_stop, post_restart)
    restart_rewrote = {}
    if d:
        restart_rewrote = {k: sorted(v) for k, v in d.items()}
        steps.append({"step": "restart-rewrote", "situation": sit_key, "diff": {k: {kk: list(vv) for kk, vv in v.items()} for k, v in d.items()}})
        log("note: the relaunch rewrote %s" % ", ".join(sorted(d)))
        for row, r in audited:
            for rel, keys in d.items():
                if row["resolved_key"] in keys:
                    b, a = d[rel][row["resolved_key"]]
                    r.notes.append("restart rewrote %s in %s: %s -> %s" % (row["resolved_key"], rel, json.dumps(b), json.dumps(a)))
    cfg.copy_to(os.path.join(sit_dir, "cfg-after-restart"))

    # ---- restore ------------------------------------------------------------
    for row, r in reversed(audited):
        rid = row["id"]
        if rid not in originals or r.live.startswith("FAIL: could not"):
            continue
        # gate-openers again, so a gated setter can take the restore
        for gate_id, gate_label in GATES.get(rid, []):
            if gate_id in by_id:
                gv = label_value(gate_id, gate_label)
                if gv is not None:
                    inst.ctl("overlay_e2_set", "%s %s" % (gate_id, gv))
        plan = restore_plan(row, originals[rid])
        if plan is None:
            r.restore = "-"
            continue
        got = set_and_get(rid, plan.set_arg)
        r.restore = "OK" if plan.matcher(got) else "FAIL: got '%s', wanted '%s'" % (got, originals[rid])
    # the gates themselves back to where the seed left them (last, so the
    # rows they gate were restored while open)
    for gate_id, label in (("display.resolution.aspect", None), ("display.refresh", None)):
        if gate_id in originals:
            plan = restore_plan(by_id[gate_id], originals[gate_id])
            if plan:
                got = set_and_get(gate_id, plan.set_arg)
                for row, r in audited:
                    if row["id"] == gate_id:
                        r.restore = "OK" if plan.matcher(got) else "FAIL: got '%s', wanted '%s'" % (got, originals[gate_id])
    time.sleep(1.5)
    restored = cfg.snapshot()
    cfg.copy_to(os.path.join(sit_dir, "cfg-after-restore"))
    inst.stop()

    d_restore = ConfigHome.diff(baseline, restored)
    summary = {
        "situation": sit_key,
        "session_profile": prof,
        "session_profile_after_restart": prof2,
        "overlay_opened": overlay_ok,
        "restart_rewrote": restart_rewrote,
        "restore_vs_baseline": {k: sorted(v) for k, v in d_restore.items()},
    }
    steps.append({"step": "situation-summary", **summary})
    return results, summary


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
def write_report(out_dir, all_results, summaries, meta):
    path = os.path.join(out_dir, "results.txt")
    lines = []
    W = lines.append
    W("settings-audit -- does every setting actually persist?")
    W("date: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    W("binary: %s" % meta["binary"])
    W("backend: %s   host: %s" % (meta["backend"], "private headless sway" if meta["sway"] else "none"))
    W("commit: %s   binary sha256: %s" % (meta["commit"], meta["sha256"]))
    W("")
    W("Columns: situation | id | key | kind | set-value | live-get | on-disk | correct-file | survives-restart | restore | verdict | enabled-at-set | notes")
    W("Situations: " + "; ".join("%s = %s" % (k, SITUATIONS[k]["title"]) for k in sorted(SITUATIONS) if k in summaries))
    W("")

    total_pass = total_fail = total_nc = 0
    ids = set()
    failures = []
    not_covered = {}
    for sit_key in sorted(summaries):
        W("## situation %s: %s" % (sit_key, SITUATIONS[sit_key]["title"]))
        s = summaries[sit_key]
        W("session profile reported: '%s' (expected '%s'); after restart: '%s'" % (
            s["session_profile"], SITUATIONS[sit_key]["session"], s["session_profile_after_restart"]))
        if s["restart_rewrote"]:
            W("restart rewrote: " + "; ".join("%s: %s" % (k, ", ".join(v)) for k, v in s["restart_rewrote"].items()))
        W("restore vs baseline (what still differs after restoring every value): " + (
            "; ".join("%s: %s" % (k, ", ".join(v)) for k, v in s["restore_vs_baseline"].items()) or "nothing"))
        W("")
        W("\t".join(["sit", "id", "key", "kind", "set-value", "live-get", "on-disk", "correct-file",
                     "survives-restart", "restore", "verdict", "enabled-at-set", "notes"]))
        for r in all_results[sit_key]:
            ids.add(r.id)
            v = r.verdict()
            if not r.covered:
                total_nc += 1
                not_covered.setdefault(r.id, r.reason)
                W("\t".join([sit_key, r.id, r.key, r.kind, "", "", "", "", "", "", "NOT COVERED", "", r.reason]))
                continue
            if v == "PASS":
                total_pass += 1
            else:
                total_fail += 1
                failures.append((sit_key, r))
            W("\t".join([sit_key, r.id, r.key, r.kind, r.set_value, r.live, r.disk, r.file, r.restart,
                         r.restore, v, r.enabled_at_set, "; ".join(r.notes)]))
        W("")

    W("## failures")
    if not failures:
        W("none")
    for sit_key, r in failures:
        stages = []
        for name, cell in (("live-get", r.live), ("on-disk", r.disk), ("correct-file", r.file), ("survives-restart", r.restart)):
            if cell.startswith("FAIL"):
                stages.append("%s: %s" % (name, cell[6:]))
        W("- [%s] %s (%s, %s): %s" % (sit_key, r.id, r.key, r.kind, " | ".join(stages)))
    W("")
    W("## not covered")
    for i in sorted(not_covered):
        W("- %s: %s" % (i, not_covered[i]))
    W("")
    n_rows = total_pass + total_fail + total_nc
    W("SUMMARY: %d settings audited (%d unique ids across %d situations, %d rows), %d passed, %d failed, %d not covered" % (
        n_rows, len(ids), len(summaries), n_rows, total_pass, total_fail, total_nc))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path, total_fail


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--situations", default="a,b,c")
    ap.add_argument("--only", default="", help="comma-separated ids to audit (others listed as skipped)")
    args = ap.parse_args()

    binary = os.environ["AUDIT_GAMESCOPE"]
    ctl = os.environ["AUDIT_GAMESCOPECTL"]
    rundir = os.environ["AUDIT_RUNDIR"]
    sway = os.environ.get("AUDIT_SWAY_WL", "")
    cfg_root = os.environ["AUDIT_CONFIGHOME"]
    out_dir = os.environ["AUDIT_OUT"]
    backend = os.environ.get("AUDIT_BACKEND", "wayland" if sway else "headless")
    commit = os.environ.get("AUDIT_COMMIT", "?")
    os.makedirs(out_dir, exist_ok=True)

    cfg = ConfigHome(cfg_root)
    inst = Instance(binary, ctl, rundir, sway, cfg, out_dir, backend)
    only_ids = set(x for x in args.only.split(",") if x)
    steps = []
    all_results = {}
    summaries = {}
    status = 0
    try:
        for sit_key in args.situations.split(","):
            sit_key = sit_key.strip()
            if sit_key not in SITUATIONS:
                continue
            try:
                results, summary = run_situation(sit_key, inst, cfg, out_dir, only_ids, steps)
            finally:
                inst.stop()
            all_results[sit_key] = results
            summaries[sit_key] = summary
    except Exception as e:  # a setup failure, not a verdict
        log("FATAL: %s" % e)
        status = 2
    finally:
        inst.stop()
        with open(os.path.join(out_dir, "steps.jsonl"), "w") as f:
            for s in steps:
                f.write(json.dumps(s) + "\n")
    if all_results:
        path, n_fail = write_report(out_dir, all_results, summaries,
                                    {"binary": binary, "backend": backend, "sway": bool(sway), "commit": commit,
                                     "sha256": os.environ.get("AUDIT_SHA256", "?")})
        log("report: %s" % path)
        with open(path) as f:
            tail = f.read().splitlines()[-1]
        log(tail)
        if status == 0 and n_fail:
            status = 1
    sys.exit(status)


if __name__ == "__main__":
    main()
