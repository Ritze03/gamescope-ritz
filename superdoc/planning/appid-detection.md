# App-ID Detection — Does Steam's Env Var Actually Reach gamescope?

Scouting doc only. No code, no builds, no launches. Read `superdoc/planning/config-system.md`
first — this doc extends/corrects its §1 ("Q1 open question"), does not edit it. Grep-verified
against this repo's `master` (fcc1341) plus external primary-source evidence, checked
2026-08-21.

## Verdict

The user's belief is **correct on substance, wrong on the exact name**: gamescope's process,
when placed in Steam launch options as `gamescope -- %command%`, **does** inherit real
Steam-set environment variables carrying the app id — but the reliable one for Proton titles is
`STEAM_COMPAT_APP_ID`, not `AppId`, and the native-game one (`SteamAppId`) is documented to be
unreliably `0` in some paths. There is **no environment variable literally named `AppId`** —
`AppId=<n>` is a *command-line argument* Steam's `reaper` wrapper carries (this matches
gamescope's own existing `get_appid_from_pid()` scrape exactly — see §3). This doc could not
get **direct** empirical evidence on this machine (no game is currently running — see §2) so
the env-var-reaches-gamescope claim rests on **documentary/tool evidence**, specifically two
independent, real, widely-used Steam-launch-option wrapper tools whose source code was read
directly (not just their docs) — see §1.

## 1. What Steam actually sets, and whether the wrapper process sees it

**Primary evidence: `steamtinkerlaunch`'s `initAID()` function**, read directly from source
(`steamtinkerlaunch` script, `initAID()`, fetched 2026-08-21 from
`https://raw.githubusercontent.com/sonic2kk/steamtinkerlaunch/master/steamtinkerlaunch`,
lines ~453-489). This tool is invoked in the **exact same topology** as gamescope-as-launch-
wrapper (`steamtinkerlaunch %command%` in Steam's launch-options field), so its behavior is
direct evidence for what env vars a wrapper process in that position receives:

```
if [ -n "$STEAM_COMPAT_APP_ID" ]; then
    AID="$STEAM_COMPAT_APP_ID"          # Proton games — preferred, reliable
fi
if [ -z "$AID" ] || [ "$AID" -eq "0" ]; then
    if [ -n "$SteamAppId" ]; then
        if [ "$SteamAppId" -eq "0" ]; then
            AID="${STEAM_COMPAT_DATA_PATH##*/}"   # fallback: compatdata dir IS the appid
        else
            AID="$SteamAppId"           # native Linux games
        fi
    fi
fi
```

Takeaways:
- **`STEAM_COMPAT_APP_ID`** — set for **Proton/Windows** titles, read directly as a plain env
  var by a process sitting where gamescope would sit in `gamescope -- %command%`. Treated as
  the most trustworthy source, checked first.
- **`SteamAppId`** — set for **native Linux** titles, but the tool's own code proves it can be
  present-but-`"0"`, in which case the tool instead falls back to
  **`STEAM_COMPAT_DATA_PATH`**'s basename (`compatdata/<appid>/` — the directory *is* named
  after the app id, so this is a robust secondary source even without a trustworthy direct var).
- No `AppId` (bare) env var appears anywhere in this tool's logic.

**Second, independent piece of evidence: `ScopeBuddy`** (`bin/scopebuddy`, fetched 2026-08-21
from `https://raw.githubusercontent.com/OpenGamingCollective/ScopeBuddy/main/bin/scopebuddy`),
a gamescope-specific wrapper with the identical stated purpose to this project's config system
(per-game gamescope args/env via `~/.config/scopebuddy/AppID/<id>.conf`, sourced when
`gamescope -- %command%` is the launch option). It does **not** read any env var at all for
app-id detection — it instead regex-scrapes its own `--`-delimited tail (i.e. its own argv, the
literal `%command%` expansion) for `"AppId=(\d+)"` (`bin/scopebuddy:521`,
`perl -pe 's/.+"AppId=(\d+)"\s.+/\1/'`). This is the **same literal string** gamescope's own
`get_appid_from_pid()` looks for (see §3) — strong corroboration that Steam's `%command%`
expansion, for native games, literally contains an `AppId=<n>` **argument token** (passed to the
`reaper` process Steam constructs), not an env var — consistent with steamtinkerlaunch treating
bare `SteamAppId` as sometimes-unreliable-`"0"` and needing a fallback.

**Reading both together:** Steam supplies the app id to a launch-option wrapper through *two
independent channels simultaneously* — as `STEAM_COMPAT_APP_ID`/`SteamAppId` env vars AND as an
`AppId=<n>` argv token nested inside the `%command%` expansion (visible once you split on the
first `--`). ScopeBuddy chose the argv route (possibly written before the env-var route was
confirmed reliable, or to unify with non-Steam launchers it also supports — Ubisoft/Heroic
URIs, `bin/scopebuddy:393-420`); steamtinkerlaunch uses the env-var route with a documented
`"0"` gotcha and its own fallback. **Neither found a bare `AppId` env var.**

`[web]` Sources checked 2026-08-21:
- steamtinkerlaunch source, `initAID()` — https://raw.githubusercontent.com/sonic2kk/steamtinkerlaunch/master/steamtinkerlaunch
- ScopeBuddy source, `bin/scopebuddy` — https://raw.githubusercontent.com/OpenGamingCollective/ScopeBuddy/main/bin/scopebuddy
- ScopeBuddy README (confirms no env-var-based appid path exists in its docs either) — https://github.com/OpenGamingCollective/ScopeBuddy/blob/main/README.md
- General corroboration for `SteamGameId`/`STEAM_COMPAT_DATA_PATH` being real Proton-adjacent
  env vars (matches `config-system.md`'s prior citation) — https://github.com/sonic2kk/steamtinkerlaunch/wiki/ENV-Variables
  (wiki page itself turned out to document only *user-added* custom vars, not Steam's own — the
  source-code evidence above supersedes it for this doc's purposes)
- WebSearch aggregate on Steam-set env var names (`SteamClientLaunch`, `SteamEnv`, `SteamAppId`,
  `SteamGameId`, `SteamOverlayGameId`, `SteamAppUser`, `SteamUser`, `STEAMID`) — not source-
  verified line-by-line, treated as corroborating-only, not load-bearing

## 2. Empirical check on this machine — inconclusive, documentary evidence only

Steam **is** installed and running on this machine (`~/.local/share/Steam`, `~/.steam` both
present; `pgrep -a -f -i steam` shows a live `steam.sh`/`steamwebhelper`/pressure-vessel tree,
PIDs 396061 etc., checked 2026-08-21). **No game is currently running**: `pgrep -a reaper` finds
only the kernel's unrelated `[oom_reaper]` thread, and `pgrep -a -f SteamLaunch` finds nothing —
i.e. there is no `reaper AppId=... SteamLaunch ...` process tree alive to inspect via
`/proc/<pid>/environ`. Per the task's read-only constraint, app id `3746030` was **not**
launched to manufacture evidence. This section is therefore **documentary evidence only**
(§1), not direct proof on this machine.

**One-command check the user can run themselves**, the moment a game is running (native or
Proton, launched normally from Steam — no gamescope wrapper needed to test this specific
question):

```sh
pid=$(pgrep -f 'reaper.*SteamLaunch' | head -1); tr '\0' '\n' < /proc/$pid/environ | grep -iE '^(SteamAppId|SteamGameId|STEAM_COMPAT_APP_ID|SteamOverlayGameId)='
```

If `gamescope-ritz` itself is the launch-option wrapper at the time (`gamescope -- %command%`),
the more direct check is to grep gamescope's own `/proc/<pid>/environ` instead of `reaper`'s —
same command with `pid=$(pgrep -x gamescope | head -1)`. Either confirms in one shot which
vars are present, their exact casing, and whether gamescope's own PID (not just some descendant)
carries them.

## 3. What gamescope does today with Steam app ids — re-verified

Confirms the sibling scout's finding, re-checked directly against `src/steamcompmgr.cpp`:

- `get_appid_from_pid()` (`src/steamcompmgr.cpp:5285`, returns via `unFoundAppId` at `:5373`)
  walks `/proc/<pid>/stat` parent links to find a `reaper` ancestor, then scans that process's
  `/proc/<pid>/cmdline` (`:5331-5332`) for two literal argv tokens: `"SteamLaunch"`
  (`:5344`, sets a flag) and `"AppId=%u"` (`:5348`, `sscanf`, only accepted if `unAppId != 0`
  and the `SteamLaunch` flag was already seen, `:5352`) — confirming the `AppId=<n>` string
  really is a **command-line argument** to Steam's `reaper`, not an env var, exactly matching
  what ScopeBuddy's argv-regex approach (§1) independently found.
- Called **per-window**, well after startup, from two sites: `wlserver.cpp:1951`
  (`window->appID = get_appid_from_pid( nPid )`, Wayland surface creation, main thread) and
  `steamcompmgr.cpp:5465` (X11 window creation, steamcompmgr thread). Different windows in the
  same gamescope instance can carry different `appID`s (`w->appID`); there is still no single
  "the app id for this process" anywhere — confirmed unchanged from the sibling doc's finding.
- `getenv("SteamAppId")` at `layer/VkLayer_FROG_gamescope_wsi.cpp:87` remains the only literal
  env-var read for a Steam app id anywhere in this repo — but it runs **inside the game's own
  process** (the Vulkan ICD layer, a separate binary loaded by the game), not gamescope's.

**Net: gamescope today has zero code that reads any app-id env var in its own process at
startup.** Everything it has is either post-startup argv-scraping of *other* processes
(`get_appid_from_pid`) or a getenv call that runs in a different process entirely (the WSI
layer). This doc's `STEAM_COMPAT_APP_ID`/`SteamAppId` finding (§1) is therefore new territory
for gamescope's own `main()`, not a reuse of anything already wired up.

## 4. Topology: wrapper vs. persistent session — these genuinely differ, confirmed

Two distinct ways gamescope ends up running a game, with different app-id availability:

**A. `gamescope -- %command%` in a title's Steam launch options (per-launch wrapper).**
Steam constructs the full command line and environment fresh for that launch and execs it — the
`STEAM_COMPAT_APP_ID`/`SteamAppId` env vars (§1) are set on **that process**, i.e. on gamescope
itself, before gamescope's `main()` even starts, and the `AppId=<n>` argv token is nested inside
gamescope's own `%command%` tail. **A `getenv()` at the top of gamescope's `main()` can see the
app id here**, per the documentary evidence in §1 — this is the case the config-system doc's
early-`main()` recommendation is built for, and it holds up.

**B. gamescope started standalone (a session compositor, e.g. Big Picture / Deck Game Mode /
manually run with no title yet chosen) and a game launched into it afterward.** Here gamescope's
process was created **before** Steam knew which title would run — there is no per-game env var
to inherit at gamescope's own startup in this topology, by construction, regardless of what
Steam does downstream. The **only** signal gamescope can get in this topology is exactly the
existing per-window mechanism in §3 (`get_appid_from_pid()`, post-startup, per-window) — which
is why that mechanism exists in the first place. This confirms the config-system doc's own
conclusion in its §2 (mid-session appid arrival "out of scope for v1") from an independent
angle: it's not merely *harder* to wire up for topology B, it's a **different kind of signal**
(late, per-window, not a single "session identity") that the env-var-at-startup design cannot
produce, ever, in this topology.

## 5. Recommended resolution order (beneath `GS_RITZ_APPID`)

User has already fixed `GS_RITZ_APPID` as the always-wins override. Beneath that, at the top of
`main()` (per config-system.md §1's `main.cpp:723` anchor), in order:

1. `GS_RITZ_APPID` (already decided, always wins when set — unchanged).
2. `STEAM_COMPAT_APP_ID` — Proton titles, the more-reliable of the two per §1's evidence.
3. `SteamAppId`, **but only if it parses as a nonzero integer** — per §1, treat literal `"0"` as
   "absent," not as app id zero, matching steamtinkerlaunch's own guard.
4. Optional stretch, not required for v1: fall back to `STEAM_COMPAT_DATA_PATH`'s basename when
   present and numeric — free extra coverage for the Proton-but-`SteamAppId=0` case
   steamtinkerlaunch's code implies is real, at the cost of one more env var read. Low risk to
   add; safe to skip if the config-system doc's author wants to keep step count minimal.
5. Nothing found → identical behavior to config-system.md §2 ("No app id available"): global
   config only, per-game tab shown disabled/informational, no file created or looked up. This
   applies uniformly to topology B (§4) — a standalone gamescope session will always land here
   at startup, by construction, and that is expected/correct, not a bug to work around.

**This changes one word in config-system.md's own recommendation**, worth flagging explicitly
since that doc says "if confirmed live, `getenv("SteamAppId")`" — per this doc's evidence, lead
with `STEAM_COMPAT_APP_ID` first and treat `SteamAppId == "0"` as absent; don't read `SteamAppId`
alone as "the" Steam env var. No code changed to reflect this — flagged here for whoever
implements it.

## Open questions for the user / implementer

- Whether the stretch fallback in §5.4 (`STEAM_COMPAT_DATA_PATH` basename) is worth the extra
  read — not required, cheap either way.
- Confirming §1's evidence with a live read (§2's one-liner) the next time any Steam title is
  launched on this machine — would upgrade this doc's evidence from documentary to direct in
  under a second.
