#include "AppId.h"

#include <cstdlib>
#include <filesystem>

namespace gamescope::config
{
    namespace
    {
        // Strict "the whole string is a positive decimal integer, and it isn't
        // zero" check - a literal "0" (and anything that isn't purely digits) is
        // treated as absent, matching steamtinkerlaunch's own guard for SteamAppId
        // (see superdoc/planning/appid-detection.md).
        std::optional<std::string> ParseNonzeroUnsignedDecimal( std::string_view svValue )
        {
            if ( svValue.empty() )
                return std::nullopt;

            for ( char c : svValue )
            {
                if ( c < '0' || c > '9' )
                    return std::nullopt;
            }

            // All-zero ("0", "00", ...) is the legitimate-but-absent case.
            bool bAllZero = true;
            for ( char c : svValue )
            {
                if ( c != '0' )
                {
                    bAllZero = false;
                    break;
                }
            }
            if ( bAllZero )
                return std::nullopt;

            return std::string{ svValue };
        }

        std::optional<std::string> AppIdFromCompatDataPath( std::string_view svPath )
        {
            if ( svPath.empty() )
                return std::nullopt;

            std::string sBasename = std::filesystem::path( svPath ).filename().string();
            return ParseNonzeroUnsignedDecimal( sBasename );
        }
    }

    std::optional<std::string> ResolveAppId( const EnvLookupFn &lookupIn )
    {
        EnvLookupFn lookup = lookupIn ? lookupIn : EnvLookupFn{ []( const char *pszName ) { return getenv( pszName ); } };

        if ( const char *pszRitzAppId = lookup( "GS_RITZ_APPID" ); pszRitzAppId && *pszRitzAppId )
            return std::string{ pszRitzAppId };

        if ( const char *pszCompatAppId = lookup( "STEAM_COMPAT_APP_ID" ); pszCompatAppId && *pszCompatAppId )
            return std::string{ pszCompatAppId };

        if ( const char *pszSteamAppId = lookup( "SteamAppId" ); pszSteamAppId && *pszSteamAppId )
        {
            if ( std::optional<std::string> oParsed = ParseNonzeroUnsignedDecimal( pszSteamAppId ) )
                return oParsed;
            // Falls through - a present-but-"0" SteamAppId is the documented
            // Proton case where STEAM_COMPAT_DATA_PATH's basename is the real id.
        }

        if ( const char *pszCompatDataPath = lookup( "STEAM_COMPAT_DATA_PATH" ); pszCompatDataPath && *pszCompatDataPath )
        {
            if ( std::optional<std::string> oParsed = AppIdFromCompatDataPath( pszCompatDataPath ) )
                return oParsed;
        }

        return std::nullopt;
    }
}
