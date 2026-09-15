#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gamescope::config
{
    // Env-var lookup used by ResolveAppId, defaulting to ::getenv. Injectable so
    // tests can exercise the resolution order without mutating the real process
    // environment (setenv/unsetenv is process-global and not safe to rely on
    // across parallel test sections).
    using EnvLookupFn = std::function<const char *( const char * )>;

    // App id resolution order (superdoc/planning/DECISIONS.md #21,
    // superdoc/planning/appid-detection.md):
    //   1. RITZ_GS_APPID       - gamescope-ritz specific, always wins when set.
    //                            Renamed from GS_RITZ_APPID 2026-09-15; that
    //                            name is still accepted as a fallback for one
    //                            release (a warning is logged when only the
    //                            old name is set -- see ResolveAppId's body).
    //   2. STEAM_COMPAT_APP_ID - Proton titles, the more reliable of Steam's two
    //   3. SteamAppId          - native Linux titles, but only if it parses as a
    //                            nonzero integer; a literal "0" is legitimate and
    //                            must be treated as absent, not as app id zero
    //   4. basename(STEAM_COMPAT_DATA_PATH) - covers the Proton-but-SteamAppId=0
    //                            case (compatdata/<appid>/ is named by app id)
    //   5. none found -> global config only, no games/<AppId>.json lookup at all
    //
    // The id RITZ_GS_APPID resolves to is an OPAQUE STRING, not necessarily a
    // Steam app id: this fork also accepts a manually-set, non-numeric id for
    // a non-Steam or otherwise not-auto-detected game (2026-09-15, "Make it
    // fully compatible with string based IDs ... for setting the ID
    // manually"). See SanitizeAppId() below for the one validation rule that
    // id is held to wherever it round-trips through UI or JSON.
    //
    // Meant to be called once, at the very top of main(), before any subsystem
    // init - see SPEC.md's Feature 6 for why that ordering matters (this is a
    // pure env-var read; it does not depend on wlserver/steamcompmgr existing).
    //
    // Deliberately does NOT consult steamcompmgr.cpp's get_appid_from_pid()
    // (the post-startup, per-window /proc scrape for a "reaper" ancestor's
    // "AppId=<n>" argv token) - verified behaviour, not an oversight. In the
    // launch-option-wrapper topology (`gamescope -- %command%`) the env vars
    // above are already present before main() runs, so nothing is gained by
    // also scraping. In the persistent-session topology (gamescope started
    // standalone, a game launched into it afterward) none of these env vars
    // were ever set on gamescope's own process, so this correctly - not as a
    // bug - resolves to std::nullopt and everything falls through to
    // global.json; wiring the scrape in to recover an id there would mean
    // hot-switching config mid-session, which SPEC.md's Feature 6 and
    // DECISIONS.md #21 rejected as confusing (settings visibly changing
    // after launch). See superdoc/planning/appid-detection.md §4 for the
    // topology split and DECISIONS.md #21 for the resolution order this
    // implements - both closed as tested, not just designed.
    std::optional<std::string> ResolveAppId( const EnvLookupFn &lookup = nullptr );

    // Validation rule for a user-supplied app id -- the Profiles area's
    // game-kind Create/Edit modal, and anywhere else an id crosses from
    // free-text UI input into config: trim leading/trailing whitespace, then
    // reject if what is left is empty, or contains '/', '\', or a control
    // character (0x00-0x1F or 0x7F). Everything else is accepted verbatim,
    // digits included -- Steam app ids happen to be numeric strings, but this
    // fork also takes an arbitrary opaque string for a manually-configured or
    // non-Steam game. The id is never used to build a filesystem path today
    // (only a JSON value and a JSON object key under profiles.games in
    // global.json, both of which tolerate any string), but '/' and '\' are
    // refused anyway on the chance a future caller ever does path-join it.
    std::optional<std::string> SanitizeAppId( std::string_view svRawAppId );
}
