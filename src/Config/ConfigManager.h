#pragma once

#include <optional>
#include <string>
#include <string_view>

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

    // Deletes games/<AppId>.json ("turn the override back off"). Missing file is
    // treated as success.
    bool ClearPerGameOverride( std::string_view svAppId );

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

    // Blocks until every currently-queued write has been flushed to disk. For
    // orderly shutdown and for tests - not for use on the steamcompmgr thread.
    void FlushPendingWrites();

    // Debug-only: a pretty-printed JSON dump of the effective config for
    // `oAppId` (global.json when std::nullopt), plus which layer won. M0 ships
    // no UI yet (SPEC.md's Build order) - this is the "temporary CLI flag ...
    // that dumps the resolved effective config to stderr" that milestone's
    // acceptance criterion asks for; a real UI/gamescopectl command can reuse
    // it later.
    std::string DebugDumpEffective( const std::optional<std::string> &oAppId );
}
