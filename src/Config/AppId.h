#pragma once

#include <functional>
#include <optional>
#include <string>

namespace gamescope::config
{
    // Env-var lookup used by ResolveAppId, defaulting to ::getenv. Injectable so
    // tests can exercise the resolution order without mutating the real process
    // environment (setenv/unsetenv is process-global and not safe to rely on
    // across parallel test sections).
    using EnvLookupFn = std::function<const char *( const char * )>;

    // App id resolution order (superdoc/planning/DECISIONS.md #21,
    // superdoc/planning/appid-detection.md):
    //   1. GS_RITZ_APPID       - gamescope-ritz specific, always wins when set
    //   2. STEAM_COMPAT_APP_ID - Proton titles, the more reliable of Steam's two
    //   3. SteamAppId          - native Linux titles, but only if it parses as a
    //                            nonzero integer; a literal "0" is legitimate and
    //                            must be treated as absent, not as app id zero
    //   4. basename(STEAM_COMPAT_DATA_PATH) - covers the Proton-but-SteamAppId=0
    //                            case (compatdata/<appid>/ is named by app id)
    //   5. none found -> global config only, no games/<AppId>.json lookup at all
    //
    // Meant to be called once, at the very top of main(), before any subsystem
    // init - see SPEC.md's Feature 6 for why that ordering matters (this is a
    // pure env-var read; it does not depend on wlserver/steamcompmgr existing).
    std::optional<std::string> ResolveAppId( const EnvLookupFn &lookup = nullptr );
}
