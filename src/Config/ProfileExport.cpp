#include "ProfileExport.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

#include "ConfigManager.h"

// -fno-exceptions build: every access below is type-checked first and parsing
// uses allow_exceptions=false, same rule as ConfigManager.cpp.

namespace gamescope::config::profile_export
{
    using json = nlohmann::json;

    namespace
    {
        constexpr int kExportVersion = 1;

        // The metadata keys of a profile file; never exported.
        constexpr const char *kMetaKeys[] = { "schema_version", "name", "kind", "app_id", "game_name", "inherits" };

        std::vector<ExportGroup> BuildGroups()
        {
            std::vector<ExportGroup> g;
            auto add = [&]( const char *pszId, const char *pszParent, const char *pszLabel, const char *pszNote,
                            bool bTick, std::vector<std::string> paths )
            {
                g.push_back( ExportGroup{ pszId, pszParent, pszLabel, pszNote, bTick, std::move( paths ) } );
            };

            add( "display.resolution", "Display", "Resolution", "Depends on this machine's display", false,
                 { "gamescope.nested_width", "gamescope.nested_height", "gamescope.nested_refresh_hz",
                   "gamescope.nested_lock_aspect" } );
            add( "display.general", "Display", "General, Upscaling, Frame limiter, HDR", "", true,
                 { "gamescope.filter", "gamescope.scaler", "gamescope.sharpness", "gamescope.vrr_enabled",
                   "gamescope.hdr_enabled", "gamescope.tearing_enabled", "gamescope.fps_limit",
                   "gamescope.force_grab_cursor", "gamescope.low_latency_wakeup",
                   "gamescope.force_windows_fullscreen", "gamescope.force_grab_keyboard",
                   "gamescope.sdr_gamut_wideness", "gamescope.sdr_on_hdr_brightness_nits",
                   "gamescope.hdr_input_gain", "gamescope.sdr_input_gain" } );
            add( "display.shaders", "Display", "Shaders", "", true, { "reshade" } );
            add( "motion.framegen", "Motion", "Frame generation", "", true, { "framegen" } );
            add( "motion.blur", "Motion", "Motion blur", "", true, { "motion_blur" } );
            add( "overlay.hud", "Overlay", "HUD", "", true, { "fps_display" } );
            add( "overlay.crosshair", "Overlay", "Crosshair", "", true, { "crosshair" } );
            add( "overlay.zoom", "Overlay", "Zoom", "", true, { "zoom" } );
            add( "overlay.notifications", "Overlay", "Notifications", "", true, { "notifications" } );
            add( "input.autoclicker", "Input", "Autoclicker", "", true, { "autoclicker" } );
            add( "input.null_binds", "Input", "Null binds", "", true, { "null_binds" } );
            add( "audio.mixer", "Audio", "Mixer and Effects", "", true, { "audio" } );
            add( "misc.system", "Misc", "System", "", true, { "system" } );
            return g;
        }

        // `overlay.*` is global.json's section and can never be in a profile
        // file; listed anyway so a pasted one is "excluded", not "unknown".
        // `lag_buffer`: the Lag Spike Buffer is being withdrawn.
        const std::vector<std::string> &Excluded()
        {
            static const std::vector<std::string> s = { "overlay", "lag_buffer" };
            return s;
        }

        bool PathCovers( const std::string &sPath, const std::string &sKey )
        {
            return sKey == sPath ||
                ( sKey.size() > sPath.size() && sKey.compare( 0, sPath.size(), sPath ) == 0 && sKey[ sPath.size() ] == '.' );
        }

        const ExportGroup *GroupOf( const std::vector<ExportGroup> &groups, const std::string &sKey )
        {
            for ( const ExportGroup &g : groups )
                for ( const std::string &sPath : g.paths )
                    if ( PathCovers( sPath, sKey ) )
                        return &g;
            return nullptr;
        }

        bool IsExcluded( const std::string &sKey )
        {
            for ( const std::string &sPath : Excluded() )
                if ( PathCovers( sPath, sKey ) )
                    return true;
            return false;
        }

        // ---- json helpers -------------------------------------------------------
        void Flatten( const json &j, const std::string &sPrefix, std::map<std::string, json> &out )
        {
            if ( j.is_object() )
            {
                for ( auto it = j.begin(); it != j.end(); ++it )
                    Flatten( *it, sPrefix.empty() ? it.key() : sPrefix + "." + it.key(), out );
            }
            else if ( !sPrefix.empty() )
                out[ sPrefix ] = j;
        }

        void SetPath( json &root, const std::string &sKey, const json &value )
        {
            json *p = &root;
            size_t nStart = 0;
            for ( ;; )
            {
                const size_t nDot = sKey.find( '.', nStart );
                if ( nDot == std::string::npos )
                {
                    ( *p )[ sKey.substr( nStart ) ] = value;
                    return;
                }
                json &sub = ( *p )[ sKey.substr( nStart, nDot - nStart ) ];
                if ( !sub.is_object() )
                    sub = json::object();
                p = &sub;
                nStart = nDot + 1;
            }
        }

        bool SameValue( const json &a, const json &b )
        {
            if ( a.is_number() && b.is_number() )
            {
                const double x = a.get<double>(), y = b.get<double>();
                return std::fabs( x - y ) <= 1e-6 * std::max( 1.0, std::max( std::fabs( x ), std::fabs( y ) ) );
            }
            return a == b;
        }

        // Would ConfigManager read `v` where the default is `def`? A value of
        // another type is silently replaced by the default there, which is
        // exactly the "unreadable" this layer must not let through.
        bool Readable( const json &def, const json &v )
        {
            if ( def.is_null() )
                return !v.is_object(); // optional value: its type is not in the default
            if ( def.is_number_integer() ) // JGetInt: integers only, and it must fit an int
            {
                if ( v.is_number_unsigned() )
                    return v.get<uint64_t>() <= uint64_t( std::numeric_limits<int>::max() );
                if ( v.is_number_integer() )
                {
                    const int64_t n = v.get<int64_t>();
                    return n >= std::numeric_limits<int>::min() && n <= std::numeric_limits<int>::max();
                }
                return false;
            }
            if ( def.is_number() ) // JGetFloat: any number, as long as it is a finite float
                return v.is_number() && std::isfinite( v.get<float>() );
            return def.type() == v.type(); // bool (JGetBool), string (JGetString)
        }

        // Pasted text comes from other people: refuse the oversize or deeply nested
        // before parsing or flattening it. One linear scan of '{' / '[' outside strings.
        constexpr size_t kMaxTextBytes = 1024 * 1024;
        constexpr int kMaxNesting = 16;

        bool TextWithinLimits( const std::string &sText )
        {
            if ( sText.size() > kMaxTextBytes )
                return false;
            int nDepth = 0;
            bool bInString = false;
            for ( size_t i = 0; i < sText.size(); i++ )
            {
                const char c = sText[ i ];
                if ( bInString )
                {
                    if ( c == '\\' )
                        i++;
                    else if ( c == '"' )
                        bInString = false;
                }
                else if ( c == '"' )
                    bInString = true;
                else if ( c == '{' || c == '[' )
                {
                    if ( ++nDepth > kMaxNesting )
                        return false;
                }
                else if ( ( c == '}' || c == ']' ) && nDepth > 0 )
                    nDepth--;
            }
            return true;
        }

        // ---- json helpers: sections come from ConfigManager ---------------------
        json StripMeta( json j )
        {
            if ( j.is_object() )
                for ( const char *psz : kMetaKeys )
                    j.erase( psz );
            return j;
        }

        std::optional<json> ReadJsonFile( const std::string &sPath )
        {
            std::ifstream f( sPath );
            if ( !f )
                return std::nullopt;
            json j = json::parse( f, nullptr, false );
            if ( j.is_discarded() )
                return std::nullopt;
            return j;
        }

        const std::map<std::string, json> &DefaultsFlat()
        {
            static const std::map<std::string, json> s = []
            {
                std::map<std::string, json> m;
                Flatten( SettingsToSectionsJson( Settings{} ), "", m );
                return m;
            }();
            return s;
        }

        // ---- parsing ---------------------------------------------------------------
        struct Parsed
        {
            ParsedExport info;
            std::map<std::string, json> leaves; // pasted settings, dotted
        };

        Parsed ParseImpl( const std::string &sText )
        {
            Parsed p;
            p.info.text = sText;
            auto fail = [&]( const char *psz ) -> Parsed
            {
                p.info.error = psz;
                return p;
            };
            if ( !TextWithinLimits( sText ) )
                return fail( "This text is too large or too deeply nested to be a settings export." );
            json j = json::parse( sText, nullptr, false );
            if ( j.is_discarded() )
                return fail( "That is not valid JSON." );
            if ( !j.is_object() )
                return fail( "That is not a settings export." );
            auto itVer = j.find( "gamescope_ritz_export" );
            if ( itVer == j.end() || !itVer->is_number() )
                return fail( "That is not a gamescope-ritz settings export." );
            if ( itVer->get<double>() > kExportVersion )
                return fail( "This export is from a newer version. Update gamescope-ritz to import it." );
            auto itSettings = j.find( "settings" );
            if ( itSettings == j.end() || !itSettings->is_object() )
                return fail( "The export has no settings." );
            auto itProfile = j.find( "profile" );
            if ( itProfile != j.end() && itProfile->is_string() )
                p.info.profile = itProfile->get<std::string>();

            Flatten( *itSettings, "", p.leaves );
            const std::map<std::string, json> &defs = DefaultsFlat();
            const std::vector<ExportGroup> &groups = Groups();
            std::set<std::string> present;
            for ( const auto &[ sKey, v ] : p.leaves )
            {
                auto itDef = defs.find( sKey );
                const ExportGroup *pGroup = GroupOf( groups, sKey );
                if ( itDef == defs.end() || !pGroup || !Readable( itDef->second, v ) )
                {
                    p.info.unknown_count++;
                    p.info.unknown_keys.push_back( sKey );
                    continue;
                }
                present.insert( pGroup->id );
            }
            for ( const ExportGroup &g : groups )
                if ( present.count( g.id ) )
                    p.info.groups_present.push_back( g.id );
            p.info.valid = true;
            return p;
        }

        // ---- the plan shared by Diff and Apply -----------------------------------------
        struct Plan
        {
            bool ok = false;
            std::string error;
            json newSections;
            std::vector<DiffRow> rows;
            int unchanged = 0;
            std::vector<std::string> skipped;
            int touched = 0; // pasted values in ticked groups, whatever became of them
            int unknown = 0;
            std::optional<ProfileMeta> destMeta; // existing destination
        };

        Plan MakePlan( const ParsedExport &parsed, const std::string &sDest, bool bNew, const GroupSet &ticked )
        {
            Plan plan;
            Parsed p = ParseImpl( parsed.text );
            if ( !p.info.valid )
            {
                plan.error = p.info.error;
                return plan;
            }
            if ( ticked.empty() )
            {
                plan.error = "Nothing selected.";
                return plan;
            }
            plan.unknown = p.info.unknown_count;

            Settings destSettings;
            if ( !bNew )
            {
                FlushPendingWrites();
                plan.destMeta = LoadProfileMeta( sDest );
                std::optional<Settings> o = LoadProfile( sDest );
                if ( !plan.destMeta || !o )
                {
                    plan.error = "There is no profile named '" + sDest + "'.";
                    return plan;
                }
                destSettings = *o;
            }
            const json jOld = SettingsToSectionsJson( destSettings );
            std::map<std::string, json> oldFlat;
            Flatten( jOld, "", oldFlat );
            const std::map<std::string, json> &defs = DefaultsFlat();
            const std::vector<ExportGroup> &groups = Groups();

            plan.newSections = jOld;
            for ( const auto &[ sKey, v ] : p.leaves )
            {
                const ExportGroup *pGroup = GroupOf( groups, sKey );
                auto itDef = defs.find( sKey );
                if ( !pGroup || itDef == defs.end() || !ticked.count( pGroup->id ) )
                    continue;
                plan.touched++;
                if ( !Readable( itDef->second, v ) )
                {
                    plan.skipped.push_back( sKey );
                    continue;
                }
                auto itOld = oldFlat.find( sKey );
                const json oldValue = itOld != oldFlat.end() ? itOld->second : itDef->second;
                if ( SameValue( oldValue, v ) )
                {
                    plan.unchanged++;
                    continue;
                }
                plan.rows.push_back( DiffRow{ pGroup->label, sKey, sKey, oldValue.dump(), v.dump() } );
                SetPath( plan.newSections, sKey, v );
            }
            plan.ok = true;
            return plan;
        }

        std::string UniqueName( const std::string &sWanted )
        {
            std::string sBase = SanitizeProfileName( sWanted ).value_or( "" );
            if ( sBase.empty() )
                sBase = "Imported";
            if ( !ProfileExists( sBase ) )
                return sBase;
            for ( int n = 2; n < 10000; n++ )
            {
                const std::string s = sBase + " " + std::to_string( n );
                if ( !ProfileExists( s ) )
                    return s;
            }
            return sBase + " 10000";
        }
    }

    // ---- groups -------------------------------------------------------------------
    const std::vector<ExportGroup> &Groups()
    {
        static const std::vector<ExportGroup> s = BuildGroups();
        return s;
    }

    GroupSet DefaultTicks()
    {
        GroupSet out;
        for ( const ExportGroup &g : Groups() )
            if ( g.default_ticked )
                out.insert( g.id );
        return out;
    }

    const std::vector<std::string> &ExcludedPaths() { return Excluded(); }

    std::vector<std::string> AllSettingsKeys()
    {
        std::vector<std::string> out;
        for ( const auto &[ sKey, v ] : DefaultsFlat() )
            out.push_back( sKey );
        return out;
    }

    std::vector<std::string> UnassignedKeys( const std::vector<ExportGroup> &groups )
    {
        std::vector<std::string> out;
        for ( const auto &[ sKey, v ] : DefaultsFlat() )
            if ( !GroupOf( groups, sKey ) && !IsExcluded( sKey ) )
                out.push_back( sKey );
        return out;
    }

    // ---- export -------------------------------------------------------------------
    ExportResult ExportSettings( const std::string &sProfile, const GroupSet &ticked, bool bResolved )
    {
        ExportResult r;
        FlushPendingWrites();
        std::optional<ProfileMeta> oMeta = LoadProfileMeta( sProfile );
        if ( !oMeta )
        {
            r.error = "There is no profile named '" + sProfile + "'.";
            return r;
        }
        std::optional<json> oSections;
        const bool bInherits = oMeta->kind == ProfileKind::Game && !oMeta->inherits.empty();
        if ( bInherits && !bResolved )
        {
            oSections = ReadJsonFile( ProfilePath( oMeta->name ) );
            if ( oSections )
                oSections = StripMeta( std::move( *oSections ) );
        }
        else if ( std::optional<Settings> o = LoadProfile( oMeta->name ) )
            oSections = SettingsToSectionsJson( *o );
        if ( !oSections )
        {
            r.error = "Could not read profile '" + sProfile + "'.";
            return r;
        }

        std::map<std::string, json> flat;
        Flatten( *oSections, "", flat );
        json jSettings = json::object();
        for ( const auto &[ sKey, v ] : flat )
        {
            const ExportGroup *pGroup = GroupOf( Groups(), sKey );
            if ( pGroup && !IsExcluded( sKey ) && ticked.count( pGroup->id ) )
                SetPath( jSettings, sKey, v );
        }
        json j = json::object();
        j[ "gamescope_ritz_export" ] = kExportVersion;
        j[ "profile" ] = oMeta->name;
        j[ "settings" ] = std::move( jSettings );
        r.text = j.dump( 2 );
        r.ok = true;
        return r;
    }

    // ---- parse / diff / apply -------------------------------------------------------------
    ParsedExport ParseExport( const std::string &sText )
    {
        return ParseImpl( sText ).info;
    }

    DiffResult DiffImport( const ParsedExport &parsed, const std::string &sDestProfile, const GroupSet &ticked )
    {
        DiffResult d;
        Plan plan = MakePlan( parsed, sDestProfile, sDestProfile.empty(), ticked );
        if ( !plan.ok )
            return d;
        d.rows = std::move( plan.rows );
        d.count = int( d.rows.size() );
        d.unchanged = plan.unchanged;
        d.skipped_keys = std::move( plan.skipped );
        return d;
    }

    std::string CheckImport( const ParsedExport &parsed, const std::string &sDest, bool bNewProfile,
                             const GroupSet &ticked )
    {
        Plan plan = MakePlan( parsed, sDest, bNewProfile, ticked );
        if ( !plan.ok )
            return plan.error.empty() ? std::string( "Could not import" ) : plan.error;
        if ( plan.touched == 0 )
            return "Nothing in the selected groups is in this export.";
        return {};
    }

    ApplyResult ApplyImport( const ParsedExport &parsed, const std::string &sDest, bool bNewProfile,
                             const GroupSet &ticked )
    {
        ApplyResult r;
        Plan plan = MakePlan( parsed, sDest, bNewProfile, ticked );
        if ( !plan.ok )
        {
            r.error = plan.error;
            return r;
        }
        if ( plan.touched == 0 )
        {
            r.error = "Nothing in the selected groups is in this export.";
            return r;
        }
        const Settings settingsNew = SettingsFromSectionsJson( plan.newSections );
        r.skipped = int( plan.skipped.size() );
        r.skipped_keys = plan.skipped;
        r.unknown = plan.unknown;

        if ( bNewProfile )
        {
            ProfileMeta meta;
            meta.name = UniqueName( sDest );
            meta.kind = ProfileKind::General;
            ProfileOp op = CreateProfile( meta, &settingsNew );
            if ( !op )
            {
                r.error = op.error;
                return r;
            }
            r.destination = meta.name;
            r.applied = int( plan.rows.size() );
            r.success = true;
            return r;
        }

        r.destination = plan.destMeta->name;
        if ( plan.rows.empty() )
        {
            r.success = true; // everything pasted already matches: nothing to write
            return r;
        }
        // D10: one .bak per profile, an older one overwritten, made BEFORE the change.
        const std::string sFile = ProfilePath( plan.destMeta->name );
        std::error_code ec;
        std::filesystem::copy_file( sFile, sFile + ".bak", std::filesystem::copy_options::overwrite_existing, ec );
        if ( ec )
        {
            r.error = "Could not back up '" + plan.destMeta->name + "' first, so nothing was changed.";
            return r;
        }
        if ( !SaveProfile( *plan.destMeta, settingsNew ) )
        {
            r.error = "Could not write '" + plan.destMeta->name + "'.";
            return r;
        }
        BumpConfigGeneration();
        r.applied = int( plan.rows.size() );
        r.success = true;
        return r;
    }
}
