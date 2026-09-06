#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ConfigSchema.h"

// Load/save/resolve for gamescope-ritz's config directory
// (~/.config/gamescope-ritz, or $XDG_CONFIG_HOME/gamescope-ritz when set):
// global.json (overlay appearance + the profile pointers) and
// profiles/<Name>.json (everything else). Profiles v2, 2026-09-06 -- see
// superdoc/features/profiles.md and superdoc/planning/profiles-concept.md.
//
// The model in one sentence: a profile is the settings file being edited;
// each game remembers which one it selected; `--profile` picks one for the
// session. A GAME profile may inherit from one GENERAL profile, storing only
// the values that differ (ConfigManager.cpp's SparseDiff/DeepMerge).
//
// Threading: everything here is safe to call from any single thread, but
// per-frame reads on the steamcompmgr thread should read an already-resolved
// Settings struct the caller owns (every panel's EnsureConfigLoaded() does),
// not call ResolvedSettings() every frame -- the Load*/Resolve* functions do
// blocking file I/O. The Enqueue*Write functions exist so writes never happen
// inline on that thread either.

namespace gamescope::config
{
    // ---- paths -----------------------------------------------------------------
    // Re-read $XDG_CONFIG_HOME on every call (cheap, never per-frame) so
    // behaviour stays correct under a temporary XDG_CONFIG_HOME in tests.
    std::string ConfigRoot();
    std::string GlobalConfigPath();
    std::string ProfilesDir();
    std::string ProfilePath( std::string_view svSanitizedName );
    // Schema-2 leftover: games/<AppId>.json. Read by the 2 -> 3 migration
    // only, never by anything else; the files are left on disk untouched.
    std::string GamesDir();

    // Profile names come from user input and become a path component
    // directly. Strips everything outside [A-Za-z0-9 _-], trims surrounding
    // spaces, and rejects a name that ends up empty. A name can never escape
    // the profiles directory as a result: the allowlist contains no path
    // separator or '.' at all.
    std::optional<std::string> SanitizeProfileName( std::string_view svName );

    // ---- global.json -------------------------------------------------------------
    // global.json carries `overlay` and the `profiles` pointers, nothing
    // else. LoadGlobal() returns a Settings whose `overlay` is the file's
    // and whose other sections are the struct defaults -- callers that need
    // the running settings want ResolvedSettings() below. Missing file ->
    // defaults; malformed JSON or a schema newer than this build -> logs
    // loudly and falls back to defaults, never blocks startup.
    Settings LoadGlobal();
    // Writes `settings.overlay` (only) plus the current pointers.
    bool SaveGlobal( const Settings &settings );

    // ---- profile files -----------------------------------------------------------
    // Resolved read: a general or standalone profile as stored; an
    // inheriting game profile deep-merged onto its parent, then parsed.
    // std::nullopt if the file is missing or unreadable (already logged).
    std::optional<Settings> LoadProfile( std::string_view svSanitizedName );
    std::optional<ProfileMeta> LoadProfileMeta( std::string_view svSanitizedName );
    bool ProfileExists( std::string_view svSanitizedName );

    // Synchronous atomic write (temp + fsync + rename). For an inheriting
    // game profile only the keys whose JSON value differs from the resolved
    // parent are stored. Not for the steamcompmgr thread -- use
    // EnqueueProfileWrite / EnqueueRoutedWrite there.
    bool SaveProfile( const ProfileMeta &meta, const Settings &settings );

    // Every profile on disk, sorted by name. Blocking directory I/O: callers
    // cache and refresh on an explicit user action, never per frame.
    std::vector<ProfileMeta> ListProfiles();

    // ---- session ------------------------------------------------------------------
    // The app id this session belongs to, resolved once (AppId.h's order)
    // and cached for the life of the process.
    const std::optional<std::string> &SessionAppId();

    // The profile this session is editing:
    //   1. the session override (`--profile` / GS_RITZ_PROFILE / ritz_profile),
    //   2. games[<app id>].selected,
    //   3. last_general,
    //   4. "Default" -- created from the struct defaults if nothing exists.
    // Cached; every function below that can change the answer refreshes it.
    const std::string &SessionProfile();
    // The `--profile`-style override in force, if any.
    const std::optional<std::string> &SessionProfileOverride();
    // The session profile's parent (inherits), or nullopt.
    std::optional<std::string> SessionProfileParent();

    // The resolved settings of the session profile: the in-process mirror
    // of the last write to it if this process wrote it, else a disk read.
    // `overlay` is filled from global.json's mirror so the struct is whole.
    Settings ResolvedSettings();

    // Selecting a profile in the list: loads it AND remembers the choice --
    // games[<app id>].selected when a game is identified, and last_general
    // whenever the profile is general. Clears any session override, bumps
    // ConfigGeneration() so every panel reloads. With no app id a GAME
    // profile can only be selected for the session (there is no game to
    // remember it for). False if no such profile.
    bool SelectProfile( std::string_view svSanitizedName );

    // `--profile <name>` / GS_RITZ_PROFILE / the `ritz_profile` ConCommand:
    // selects a profile for THIS SESSION ONLY -- the assignment on disk is
    // untouched, the next flagless launch is back on it. Any profile, even
    // another game's. A name that does not exist is CREATED as a general
    // profile copied from what the session would otherwise have used
    // (`created` and `copied_from` say so, for the caller's toast). The raw
    // name is sanitized; `ok` is false only when it sanitizes to nothing.
    struct SessionProfileResult
    {
        bool ok = false;
        std::string name;
        bool created = false;
        std::string copied_from;
    };
    SessionProfileResult UseSessionProfile( std::string_view svRawName );

    // Persists `settings` (minus `overlay`) into the session profile -- the
    // single funnel every panel's edits go through. Queued on the
    // background writer; for an inheriting game profile the sparse diff is
    // computed here, against a cached copy of the parent.
    void EnqueueRoutedWrite( const Settings &settings );

    // Bumped by everything here that changes which profile is authoritative
    // or what it resolves to (select, create-as-session, edit, delete, reset
    // to inherited) -- never by an ordinary slider edit. Every panel's
    // EnsureConfigLoaded() compares it against the generation it last
    // loaded at and reloads via ResolvedSettings() when they differ.
    uint64_t ConfigGeneration();
    void BumpConfigGeneration();

    // ---- CRUD for the Profiles list ------------------------------------------------
    // Every operation is synchronous and returns why it refused, so the UI
    // can say it. The rules (superdoc/planning/profiles-concept.md, v2):
    //   - only a game profile may inherit, and only from a general profile;
    //   - a general profile with children cannot become a game profile;
    //   - rename follows every pointer (assignments, children's `inherits`,
    //     the session override);
    //   - deleting a parent bakes its children (resolved values written,
    //     `inherits` cleared) rather than orphaning them.
    struct ProfileOp
    {
        bool ok = true;
        std::string error;
        explicit operator bool() const { return ok; }
    };

    // Creates `meta` with `from`'s values, or the current ResolvedSettings()
    // when `from` is null. Does not select it.
    ProfileOp CreateProfile( const ProfileMeta &meta, const Settings *pFrom = nullptr );
    // A copy of `svSource`'s RESOLVED values under `meta`.
    ProfileOp CopyProfile( std::string_view svSource, const ProfileMeta &meta );
    // Rename / kind / app id / game name / inherits, in one call. The
    // profile's RESOLVED values are preserved: switching parents re-diffs
    // them against the new parent; becoming general bakes them in.
    ProfileOp EditProfileMeta( std::string_view svOldName, const ProfileMeta &meta );
    ProfileOp DeleteProfile( std::string_view svSanitizedName );

    // ---- inheritance markers -----------------------------------------------------
    // The dotted keys ("reshade.vibrancy.strength", "fps_display.enabled")
    // the session profile stores itself, i.e. OVERRIDES its parent for.
    // Empty for a general or standalone profile. Cached on the write
    // sequence, so it is free to call every frame.
    //
    // ponytail: diff-based, so a value the user sets EQUAL to the parent's
    // is stored as nothing and reads as inherited (it will follow the parent
    // later). The alternative -- per-key "explicitly set" plumbing through
    // every panel -- was not worth it for this ceiling.
    const std::set<std::string> &OverriddenKeys();
    // Drops one overridden key from the session profile's file so the value
    // follows the parent again. False if the profile does not inherit or the
    // key was not overridden. Synchronous; bumps ConfigGeneration().
    bool ResetKeyToInherited( std::string_view svDottedKey );

    // ---- the per-game entry ---------------------------------------------------------
    GameAssignment GameEntry( std::string_view svAppId );
    void SetGameAudioNode( std::string_view svAppId, std::string_view svBinary );

    // ---- the game's display name -------------------------------------------------
    // "[Game] Rust" needs a name; the code knows the focused window's title
    // (steamcompmgr) and the app id, nothing more. The first title seen for
    // this session's app id is written into any game profile bound to it
    // that has no game_name yet, so the list can show it while the game is
    // not running. SessionGameName() is that title, else the app id, else "".
    void NoteFocusedWindowTitle( std::string_view svTitle );
    std::string SessionGameName();

    // ---- global.json writes ----------------------------------------------------------
    // Queued atomic writes (see EnqueueRoutedWrite). Coalesced: a write for
    // a path that already has one pending replaces it, and the worker waits
    // for kWriteCoalesceMs of quiet (capped at kWriteCoalesceMaxMs) before
    // taking a batch, so a slider drag costs one write per file per pause.
    // FlushPendingWrites() skips the quiet period.
    void EnqueueGlobalWrite( Settings settings );
    void EnqueueProfileWrite( const ProfileMeta &meta, const Settings &settings );
    // Issue #35: writes `overlay` (only) to global.json.
    void EnqueueOverlayWrite( const OverlaySettings &overlay );
    // Issue #35: patches one PanelGeometry entry onto the freshest known
    // `overlay` in memory and writes that, so Chrome.cpp never needs a copy
    // of every other overlay field just to save one panel's position.
    void EnqueueGeometryWrite( const std::string &sPanelKey, const PanelGeometry &geometry );
    // Blocks until every queued write is on disk. Shutdown and tests only.
    void FlushPendingWrites();

    // Test-only: forgets every cached piece of session state (app id,
    // override, session profile, mirrors, migration flag) as if this were a
    // fresh process. catch2 runs every [config] TEST_CASE in one process,
    // each against its own temporary config home.
    void ResetSessionRoutingForTests();

    // --ritz-dump-config: the session profile, its parent, the launch
    // option, which rule chose it, and the resolved settings, as JSON.
    std::string DebugDumpEffective();
}
