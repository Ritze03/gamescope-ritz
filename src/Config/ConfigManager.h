#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ConfigSchema.h"

// Load/save/resolve for gamescope-ritz's config directory
// (~/.config/gamescope-ritz, or $XDG_CONFIG_HOME/gamescope-ritz when set) -
// global.json, profiles/<Name>.json, games/<AppId>.json. See
// superdoc/planning/SPEC.md's "Config schema" / Feature 6 sections; this header
// is the implementation of that design.
//
// Threading (SPEC.md's threading section): everything in this header is safe to
// call from any single thread, but per-frame reads on the steamcompmgr thread
// should read an already-resolved Settings struct the caller owns, not call
// ResolveEffective() every frame - these Load*/Resolve* functions do blocking
// file I/O and JSON parsing, which is fine at startup/profile-apply time but not
// on the vblank-paced render loop. The Enqueue*Write functions below exist
// specifically so writes never happen inline on that thread either.

namespace gamescope::config
{
    // Directory/path helpers. Re-read $XDG_CONFIG_HOME on every call (cheap, and
    // only ever called at startup/save time, never per-frame) rather than
    // caching, so behaviour stays correct under a temporary XDG_CONFIG_HOME in
    // tests.
    std::string ConfigRoot();
    std::string GlobalConfigPath();
    std::string ProfilesDir();
    std::string GamesDir();
    std::string ProfilePath( std::string_view svSanitizedName );
    std::string GamePath( std::string_view svAppId );

    // Profile names come from user input and become a path component directly.
    // Strips everything outside [A-Za-z0-9 _-], trims surrounding spaces, and
    // rejects a name that ends up empty (or is exactly "." or ".." - unreachable
    // via the allowlist today, but checked explicitly as cheap defense in depth).
    // A name can never escape the profiles directory as a result: the allowlist
    // simply contains no path separator or '.' character at all.
    std::optional<std::string> SanitizeProfileName( std::string_view svName );

    // Loads global.json. Missing file -> compiled-in defaults (Settings{}), not
    // an error. Malformed JSON, or a schema_version newer than this build
    // understands -> logs loudly and falls back to defaults too - never blocks
    // startup, never crashes gamescope.
    Settings LoadGlobal();

    // Loads profiles/<svSanitizedName>.json. std::nullopt if it doesn't exist or
    // fails to load (already logged in the latter case).
    std::optional<Settings> LoadProfile( std::string_view svSanitizedName );

    // Loads games/<svAppId>.json, but only returns a value when the file exists
    // AND its own override_global field is true - a missing file, a parse
    // failure, or override_global: false all behave identically (std::nullopt,
    // i.e. "fall through to global"), per SPEC.md's per-layer fallback policy.
    std::optional<Settings> LoadPerGameOverride( std::string_view svAppId );

    // Two-level resolution (SPEC.md Feature 6 - a full snapshot, not a per-key
    // merge): games/<AppId>.json when it exists and override_global is true, OR
    // global.json otherwise. Pass std::nullopt when no app id was resolved.
    Settings ResolveEffective( const std::optional<std::string> &oAppId );

    // Synchronous atomic writes (temp file + fsync + rename, in the same
    // directory as the target) - safe to call from any thread that isn't the
    // steamcompmgr render thread. Use the Enqueue* functions below from there
    // instead.
    bool SaveGlobal( const Settings &settings );
    bool SaveProfile( std::string_view svSanitizedName, const Settings &settings );

    // "Override Global Config" snapshot (SPEC.md decision, DECISIONS.md #19):
    // writes games/<AppId>.json with override_global: true and a full copy of
    // `snapshot` - not a sparse delta. This is the only way a per-game file ever
    // comes into existence; if a game never enables the override, its file is
    // never created.
    bool SnapshotPerGameOverride( std::string_view svAppId, const Settings &snapshot );

    // "Turn the override back off" (DECISIONS.md #19's amendment, issue #43):
    // marks games/<AppId>.json inactive by flipping its own override_global
    // field to false, in place - it deliberately does NOT delete the file.
    // The saved values stay on disk so a later EnableOverride (see
    // RestorePerGameOverride below) can bring them back. Missing file is
    // treated as success (nothing to deactivate). Deleting the file outright
    // is a separate, explicit, user-confirmed action - see
    // DeletePerGameOverride.
    bool ClearPerGameOverride( std::string_view svAppId );

    // True when games/<AppId>.json exists and parses, regardless of its own
    // override_global flag - i.e. "is there a saved per-game config to
    // restore or delete", independent of whether it's currently active. Used
    // by the Per-Game tab to decide between "Override enabled: restored" and
    // "Override enabled: snapshotted", and to show the honest "a saved
    // config exists but isn't in use" state (DECISIONS.md #19's amendment).
    bool HasSavedPerGameConfig( std::string_view svAppId );

    // Re-activates an existing games/<AppId>.json by flipping its
    // override_global field back to true IN PLACE, preserving every value
    // already on disk rather than re-snapshotting from global (DECISIONS.md
    // #19's amendment) - "I turned this off and back on" restores what was
    // there, it does not silently discard it. Returns false, writing
    // nothing, if the file doesn't exist or fails to parse; the caller
    // (PanelConfig's EnableOverride) falls back to SnapshotPerGameOverride
    // in that case.
    bool RestorePerGameOverride( std::string_view svAppId );

    // The ONLY function in this file that actually deletes games/<AppId>.json
    // (issue #43: a toggle must never do this - only an explicit,
    // user-confirmed action may). Refuses (returns false, deletes nothing)
    // unless svAppId is a bare id with no path separator and isn't "." or
    // "..", and unless the resulting path's parent directory is exactly
    // GamesDir() - defense in depth in the same spirit as
    // SanitizeProfileName's profiles/ containment, even though app ids come
    // from ResolveAppId()/env vars rather than free-text UI input. Never
    // touches global.json or anything under profiles/. Missing file is
    // treated as success.
    bool DeletePerGameOverride( std::string_view svAppId );

    // Applying a profile copies its values into `target` once (DECISIONS.md
    // #20) - not a live reference. `target.overlay` (a process-level, not a
    // per-game/profile, preference) is left untouched. Returns false, leaving
    // `target` unmodified, if the profile can't be loaded.
    bool ApplyProfile( Settings &target, std::string_view svSanitizedName );

    // Queues an atomic write onto a small dedicated background thread instead of
    // writing inline - the steamcompmgr thread (or any caller that can't afford
    // a blocking fsync()/rename()) should use these instead of the synchronous
    // Save*/Snapshot* functions above.
    //
    // ponytail: no per-slider debounce/coalescing yet - M0 ships no live-edit UI
    // to debounce in the first place. The queue + dedicated worker thread (not a
    // raw cross-thread struct write) is the load-bearing part or SPEC.md's "disk
    // I/O must never happen on the steamcompmgr thread" requirement; a "coalesce
    // edits within N ms of inactivity" policy is a small addition on top of this
    // once a later milestone's overlay actually produces per-frame edits.
    void EnqueueGlobalWrite( Settings settings );
    void EnqueuePerGameSnapshot( std::string sAppId, Settings snapshot );
    void EnqueueProfileWrite( std::string sSanitizedName, Settings settings );

    // Issue #35: writes `overlay` (only) to global.json, without the caller
    // needing to hold a fresh copy of every other section - merges onto the
    // freshest full Settings this process has seen (CurrentFullSettings(),
    // ConfigManager.cpp), the same "don't clobber a section you don't own"
    // protection EnqueueRoutedWrite() already gives `overlay` itself,
    // pointed the other way.
    void EnqueueOverlayWrite( const OverlaySettings &overlay );

    // Issue #35: the call Chrome.cpp's panel-geometry autosave actually
    // makes - patches a single PanelGeometry entry onto the freshest known
    // `overlay` in-memory (see ConfigManager.cpp's CurrentOverlaySettings())
    // and writes that, so Chrome.cpp never needs to keep its own copy of
    // every other General-tab overlay field just to save one panel's
    // position/size. sPanelKey is a stable per-panel string (see
    // ConfigSchema.h's PanelGeometry comment for why not the PanelId enum).
    void EnqueueGeometryWrite( const std::string &sPanelKey, const PanelGeometry &geometry );

    // Issue #40: same shape as EnqueueGeometryWrite() immediately above --
    // patches OverlaySettings::system_monitor_tab onto the freshest known
    // `overlay` in-memory and writes that, rather than going through
    // EnqueueRoutedWrite() (FpsDisplay.cpp's usual persistence path for
    // every OTHER System Monitor setting), which deliberately discards
    // any caller-supplied `overlay` and substitutes the freshest known
    // copy instead (see that function's own comment) -- exactly the
    // "don't own this field" protection that would otherwise silently
    // drop a tab-selection edit made through it. sTab is "modules" or
    // "statistics" (ConfigSchema.h's OverlaySettings::system_monitor_tab).
    void EnqueueSystemMonitorTabWrite( const std::string &sTab );

    // Blocks until every currently-queued write has been flushed to disk. For
    // orderly shutdown and for tests - not for use on the steamcompmgr thread.
    void FlushPendingWrites();

    // Directory listing for the Config/Profiles panel (SPEC.md's UI structure
    // section) - sanitized profile names / game app ids currently on disk,
    // sorted, with no ".json" suffix. Empty when the respective directory
    // doesn't exist yet. Still blocking directory I/O like every other
    // function in this header - callers should cache and refresh on an
    // explicit user action (panel open, profile created/applied), not call
    // every frame a panel is drawn.
    std::vector<std::string> ListProfiles();
    std::vector<std::string> ListGameIds();

    // Session-wide "where do a live-edited panel's writes belong" routing,
    // shared by every panel that keeps its own locally-cached Settings and
    // persists it on every edit (PanelDisplay, PanelShaders, FpsDisplay,
    // PanelConfig - see each file's own EnsureConfigLoaded()/QueueSave()
    // pair). SessionAppId() resolves once and is cached for the rest of the
    // process's life, matching SPEC.md's "no live app-id reload" decision
    // (Feature 6). IsSessionOverrideActive() is a small cached bool, lazily
    // seeded from an on-disk check the first time anything asks and from then
    // on kept current only by SetSessionOverrideActive() (PanelConfig calls
    // it right after it enables/clears/replaces the per-game snapshot) - so
    // answering "which file does this edit belong in?" on every keystroke
    // never needs a blocking disk read (see this header's threading note).
    const std::optional<std::string> &SessionAppId();
    bool IsSessionOverrideActive();
    void SetSessionOverrideActive( bool bActive );

    // Bumped by PanelConfig whenever one of its own actions (override
    // enabled/cleared, profile applied, another game's config copied in)
    // changes what's authoritative for the current session - never by an
    // ordinary per-slider edit in another panel. Every panel's own
    // EnsureConfigLoaded() compares this against the generation it last
    // loaded at and reloads via ResolveEffective() when they differ - the
    // whole mechanism "pick a profile, other already-open panels pick it up"
    // relies on, without a shared observer/event system.
    uint64_t ConfigGeneration();
    void BumpConfigGeneration();

    // Persists `settings` to whichever file SessionAppId()/
    // IsSessionOverrideActive() currently say is authoritative for this
    // session - games/<AppId>.json (as a full snapshot, DECISIONS.md #19)
    // when a per-game override is active, global.json otherwise. Every panel
    // with a locally-cached Settings struct should call this instead of
    // EnqueueGlobalWrite() directly so "Override Global Config" routes every
    // panel's writes the same way.
    void EnqueueRoutedWrite( const Settings &settings );

    // Test-only: resets every piece of the session-routing cache above
    // (SessionAppId/IsSessionOverrideActive/ConfigGeneration) as if this
    // were a fresh process. Production code never calls this - an app id is
    // resolved once for the life of the real process by design (SPEC.md's
    // "no live app-id reload" decision, Feature 6); tests/test_config.cpp
    // needs it purely because catch2 runs every [config]-tagged TEST_CASE in
    // one shared process, and each test wants its own session identity.
    void ResetSessionRoutingForTests();

    // Debug-only: a pretty-printed JSON dump of the effective config for
    // `oAppId` (global.json when std::nullopt), plus which layer won. M0 ships
    // no UI yet (SPEC.md's Build order) - this is the "temporary CLI flag ...
    // that dumps the resolved effective config to stderr" that milestone's
    // acceptance criterion asks for; a real UI/gamescopectl command can reuse
    // it later.
    std::string DebugDumpEffective( const std::optional<std::string> &oAppId );
}
