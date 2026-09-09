# Scripting & ConVars — Lua console and runtime-tunable variables

Two tightly linked pieces: a Lua scripting console/runtime embedded via
[sol2](https://github.com/ThePhD/sol2) (`sol::sol.hpp`), and a ConVar/ConCommand system —
Gamescope's equivalent of Source-engine-style console variables/commands — used throughout
the codebase to expose runtime-tunable state and debug commands, optionally bound into Lua.

## How it works

- `ConCommand` (`src/convar.h`) is the base type: a named, described command backed by a
  `ConCommandFunc` callback taking `std::span<std::string_view>` args, registered in a
  global `Dict<ConCommand*>` (`GetCommands()`). `ConVar<T>` (`src/convar.h:127 class ConVar`)
  extends it — a typed value (`T`) with an optional change callback
  (`ConVarCallbackFunc`), constructed with a name, default value, description, and flags
  for whether to run the callback at construction and whether to auto-register with the
  scripting layer.
- Real usage example: `src/steamcompmgr.cpp:932 cc_focus_info` declares
  `static gamescope::ConCommand cc_focus_info( "focus_info", "Dump debug info about the
  focus state", ... )` whose lambda just flips an `std::atomic<bool>` flag
  (`g_bPendingFocusInfo`); the comment above it explains *why*: "The main loop dumps this
  on the steamcompmgr thread, which owns the focus state and the X connections" — the
  command itself can be invoked from any thread, but the actual dump must happen on the
  thread that owns the data, so it just requests work rather than doing it inline.
- When compiled with scripting support (`#if HAVE_SCRIPTING` in `src/convar.h` and
  `src/convar_script.cpp`), every `ConCommand`/`ConVar` can register itself into the Lua
  environment: `ConCommand::RegisterScript` / `ConVar<T>::RegisterScript`
  (`src/convar_script.cpp:34`, `:40`) install a `sol::usertype` binding exposing `name`,
  `description`, `call`, and (for `ConVar`) `value`, so Lua code can read/write the same
  variables the C++ console commands operate on. *Why:* one registration path keeps the
  Lua-visible surface and the native ConVar table in sync automatically instead of
  hand-maintaining a separate Lua API.
- `CScriptManager` (`src/Script/Script.h:37`) owns the `sol::state`, a `GamescopeScript_t`
  struct caching frequently used Lua tables (`Base`, `Convars.Base`, `Config.Base`,
  `Config.KnownDisplays`), and a hook system: `CallHook<Args...>(name, args...)` invokes
  every Lua function registered under that hook name via `m_Hooks`, a `MultiDict<Hook_t>`
  keyed by hook name so multiple scripts can bind the same hook. `RunScriptText`, `RunFile`,
  and `RunFolder` load Lua source from a string, a single file, or (optionally recursively)
  a directory tree; `RunDefaultScripts()` runs whatever Gamescope treats as its built-in
  script set, and `RunBundledScripts()` runs the copies compiled into the binary.

## Where the default scripts come from (precedence, 2026-09-09)

`scripts/00-gamescope/` — the known-displays database (`gamescope.config.known_displays`,
which `DRMBackend.cpp` looks a panel up in) and the `gamescope.modegen` helpers those
display files call — is **compiled into the binary** by `src/Script/embed_scripts.py` and
needs nothing installed beside it. `CScriptManager::RunDefaultScripts()` resolves, in
order:

1. `script_use_local_scripts` (convar, dev builds) → `../scripts`
2. `$GAMESCOPE_SCRIPT_PATH` → each `:`-separated directory
3. `SCRIPT_DIR` → `$prefix/share/gamescope-ritz/scripts`
4. **the copies compiled into the binary** — the default

1 and 2 are mutually exclusive with 3; whichever applies, the embedded set runs **only**
if that source produced no readable directory. So a real tree on disk (or an env var
pointing at one) replaces the bundled data wholesale, and an existing-but-empty directory
means "run no defaults" and is honoured as such rather than backfilled from the binary.
`/etc/gamescope-ritz/scripts`, `/etc/gamescope/scripts` and the two user config
directories are read *after* all of the above (in the no-override case) exactly as
before: those are additions, not replacements.

**Why embedded rather than installed.** Until 2026-09-09 the tree reached disk only via
`default_extras_install.sh`, an install step `install.sh` prompted for and defaulted to
*declining* — so the ordinary install produced a binary with no display database at all,
and said so only in a `scriptmgr` warning. The load order is preserved by construction:
`embed_scripts.py` walks the tree in `RunFolder`'s own order (files sorted, then
subdirectories sorted, recursively), so `common/` still runs before `displays/` and an
on-disk replacement behaves identically. Chunk names are the script's path within the
tree, so a Lua error still names the file it came from.

**They can run from memory because they are self-contained.** Every file under
`scripts/00-gamescope/` was checked for `require`, `dofile`, `loadfile` or a relative
include: the only hit is `common/inspect.lua`'s `pcall(require, 'compat53.module')`, a
`pcall`-guarded optional shim for Lua < 5.3 that fails harmlessly under LuaJIT and is not
a filesystem dependency. Nothing else touches `io`, `os.getenv` or `package.path`. So
there was no need for the auto-export alternative (writing the copies into
`XDG_DATA_HOME` on first run and loading from there), which would have added a
first-run write, a corruption-recovery path and a directory to keep in step with the
binary.

## Using it

Console commands/ConVars are declared inline wherever a subsystem needs a runtime knob
(as with `cc_focus_info` above) — just construct a `static gamescope::ConCommand` or
`gamescope::ConVar<T>` at namespace scope near the code it controls. With scripting built
in, the same variable is automatically reachable from Lua as
`gamescope.convars.<name>.value` (via the `Convars.Base` table) without extra glue code.

## Options

| Config key | Default | Meaning |
| --- | --- | --- |
| `HAVE_SCRIPTING` (compile define) | build-dependent | Gates Lua/sol2 integration and script-registration of ConVars/ConCommands. |

## Related links

- [build-and-tooling](build-and-tooling.md) — how `HAVE_SCRIPTING` and other feature flags are wired through Meson.
- [steamcompmgr-focus](steamcompmgr-focus.md) — owner of the focus-state data that `cc_focus_info` dumps.
