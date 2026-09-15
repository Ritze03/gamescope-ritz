#include "AppId.h"

#include <cstdlib>
#include <filesystem>

#include "log.hpp"

namespace gamescope::config
{
    namespace
    {
        // A distinct scope name from ConfigManager.cpp's "config" -- two
        // static LogScopes constructed with the same name collide (each
        // registers a "log_<name>" ConVar, and the second one to construct
        // trips ConCommand's duplicate-name assertion at process startup,
        // in unspecified static-init order across translation units).
        LogScope s_AppIdLog( "appid" );

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

        if ( const char *pszRitzAppId = lookup( "RITZ_GS_APPID" ); pszRitzAppId && *pszRitzAppId )
            return std::string{ pszRitzAppId };

        // Renamed from GS_RITZ_APPID 2026-09-15. Accept the old name as a
        // fallback for one release -- only reached when the new name is
        // unset/empty, so when both are set the new one already won above.
        if ( const char *pszOldAppId = lookup( "GS_RITZ_APPID" ); pszOldAppId && *pszOldAppId )
        {
            s_AppIdLog.warnf( "GS_RITZ_APPID is deprecated and will be removed in a future "
                "release; rename it to RITZ_GS_APPID in your launch options." );
            return std::string{ pszOldAppId };
        }

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

    std::optional<std::string> SanitizeAppId( std::string_view svRawAppId )
    {
        constexpr const char *kBlank = " \t\r\n";
        const size_t nStart = svRawAppId.find_first_not_of( kBlank );
        if ( nStart == std::string_view::npos )
            return std::nullopt;
        const size_t nEnd = svRawAppId.find_last_not_of( kBlank );
        const std::string_view svTrimmed = svRawAppId.substr( nStart, nEnd - nStart + 1 );

        for ( unsigned char c : svTrimmed )
        {
            if ( c == '/' || c == '\\' || c < 0x20 || c == 0x7F )
                return std::nullopt;
        }

        return std::string{ svTrimmed };
    }
}
