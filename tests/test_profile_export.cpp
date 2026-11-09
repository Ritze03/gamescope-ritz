#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "Config/ConfigManager.h"
#include "Config/ProfileExport.h"

using namespace gamescope::config;
namespace pe = gamescope::config::profile_export;

namespace
{
    // Same isolation as test_config.cpp: a fresh XDG_CONFIG_HOME per test.
    struct TempHome
    {
        std::filesystem::path dir;
        TempHome()
        {
            std::string sTemplate = ( std::filesystem::temp_directory_path() / "gamescope-ritz-pe-XXXXXX" ).string();
            char *psz = mkdtemp( sTemplate.data() );
            REQUIRE( psz != nullptr );
            dir = psz;
            setenv( "XDG_CONFIG_HOME", dir.c_str(), 1 );
            ResetSessionRoutingForTests();
        }
        ~TempHome()
        {
            FlushPendingWrites();
            std::error_code ec;
            std::filesystem::remove_all( dir, ec );
            unsetenv( "XDG_CONFIG_HOME" );
            ResetSessionRoutingForTests();
        }
    };

    pe::GroupSet AllGroups()
    {
        pe::GroupSet s;
        for ( const pe::ExportGroup &g : pe::Groups() )
            s.insert( g.id );
        return s;
    }

    Settings Modified()
    {
        Settings s;
        s.framegen.multiplier = 3;
        s.motion_blur.samples = 6;
        s.zoom.factor = 3.0f;
        s.gamescope.fps_limit = 90;
        s.gamescope.nested_width = 1280;
        s.crosshair.enabled = !s.crosshair.enabled;
        return s;
    }

    void MakeGeneral( const std::string &sName, const Settings &s )
    {
        ProfileMeta m;
        m.name = sName;
        REQUIRE( SaveProfile( m, s ) );
    }

    ProfileMeta MakeGame( const std::string &sName, const std::string &sParent, const Settings &s )
    {
        ProfileMeta m;
        m.name = sName;
        m.kind = ProfileKind::Game;
        m.app_id = "4242";
        m.inherits = sParent;
        REQUIRE( SaveProfile( m, s ) );
        return m;
    }

    nlohmann::json ReadFile( const std::string &sPath )
    {
        std::ifstream f( sPath );
        REQUIRE( f );
        nlohmann::json j = nlohmann::json::parse( f, nullptr, false );
        REQUIRE( !j.is_discarded() );
        return j;
    }

    std::string Slurp( const std::string &sPath )
    {
        std::ifstream f( sPath );
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string ExportOf( const std::string &sName, const pe::GroupSet &g, bool bResolved = true )
    {
        pe::ExportResult r = pe::ExportSettings( sName, g, bResolved );
        REQUIRE( r.ok );
        return r.text;
    }
}

TEST_CASE( "export groups partition the serialised settings", "[profile_export]" )
{
    TempHome home;
    const std::vector<std::string> keys = pe::AllSettingsKeys();
    REQUIRE( keys.size() > 100 );

    // A key in no group and not excluded: add the setting to a group or to
    // ExcludedPaths() in src/Config/ProfileExport.cpp.
    CHECK( pe::UnassignedKeys( pe::Groups() ).empty() );

    // Never both, never two groups.
    for ( const std::string &k : keys )
    {
        int n = 0;
        for ( const pe::ExportGroup &g : pe::Groups() )
        {
            std::vector<pe::ExportGroup> one{ g };
            // UnassignedKeys() is empty for k iff `g` or the excluded list covers k.
            const std::vector<std::string> u = pe::UnassignedKeys( one );
            if ( std::find( u.begin(), u.end(), k ) == u.end() )
                n++;
        }
        const bool bExcluded = k.rfind( "lag_buffer.", 0 ) == 0 || k.rfind( "overlay.", 0 ) == 0;
        if ( bExcluded )
            CHECK( n == int( pe::Groups().size() ) ); // the excluded list covers it in every single-group probe
        else
            CHECK( n == 1 );
    }

    // The tripwire itself: take a group away and keys become unassigned.
    std::vector<pe::ExportGroup> fewer = pe::Groups();
    fewer.erase( fewer.begin() + 3 ); // Frame generation
    CHECK( !pe::UnassignedKeys( fewer ).empty() );

    // lag_buffer is serialised, so it must be excluded, not grouped.
    bool bLag = false;
    for ( const std::string &k : keys )
        bLag = bLag || k.rfind( "lag_buffer.", 0 ) == 0;
    CHECK( bLag );
    for ( const pe::ExportGroup &g : pe::Groups() )
        for ( const std::string &p : g.paths )
            CHECK( p.rfind( "lag_buffer", 0 ) != 0 );
}

TEST_CASE( "export default ticks: only Resolution is unticked", "[profile_export]" )
{
    const pe::GroupSet t = pe::DefaultTicks();
    CHECK( t.count( "display.resolution" ) == 0 );
    CHECK( t.size() + 1 == pe::Groups().size() );
}

TEST_CASE( "export: round trip into a new profile", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Src", Modified() );
    const std::string sText = ExportOf( "Src", AllGroups() );

    pe::ParsedExport p = pe::ParseExport( sText );
    REQUIRE( p.valid );
    CHECK( p.profile == "Src" );
    CHECK( p.unknown_count == 0 );

    pe::ApplyResult r = pe::ApplyImport( p, "Copy", true, AllGroups() );
    REQUIRE( r.success );
    CHECK( r.destination == "Copy" );
    CHECK( r.skipped == 0 );

    std::optional<Settings> a = LoadProfile( "Src" ), b = LoadProfile( "Copy" );
    REQUIRE( a );
    REQUIRE( b );
    CHECK( b->framegen.multiplier == 3 );
    CHECK( b->motion_blur.samples == 6 );
    CHECK( b->zoom.factor == 3.0f );
    CHECK( b->gamescope.fps_limit == 90 );
    CHECK( b->gamescope.nested_width == 1280 );
    CHECK( b->crosshair.enabled == a->crosshair.enabled );
    // Same text again: the copy exports identically apart from the profile name.
    nlohmann::json j1 = nlohmann::json::parse( sText ), j2 = nlohmann::json::parse( ExportOf( "Copy", AllGroups() ) );
    CHECK( j1[ "settings" ] == j2[ "settings" ] );
}

TEST_CASE( "export: subset, and the file is pretty-printed", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Src", Modified() );
    const std::string sText = ExportOf( "Src", { "motion.framegen" } );
    CHECK( sText.find( "\n  " ) != std::string::npos );
    nlohmann::json j = nlohmann::json::parse( sText );
    CHECK( j[ "gamescope_ritz_export" ] == 1 );
    REQUIRE( j[ "settings" ].is_object() );
    CHECK( j[ "settings" ].size() == 1 );
    CHECK( j[ "settings" ][ "framegen" ][ "multiplier" ] == 3 );

    pe::ParsedExport p = pe::ParseExport( sText );
    REQUIRE( p.valid );
    REQUIRE( p.groups_present.size() == 1 );
    CHECK( p.groups_present[ 0 ] == "motion.framegen" );

    // Resolution is its own group: not in a General-only export.
    j = nlohmann::json::parse( ExportOf( "Src", { "display.general" } ) );
    CHECK( j[ "settings" ][ "gamescope" ].contains( "fps_limit" ) );
    CHECK( !j[ "settings" ][ "gamescope" ].contains( "nested_width" ) );

    // Never exported, however ticked.
    j = nlohmann::json::parse( ExportOf( "Src", AllGroups() ) );
    CHECK( !j[ "settings" ].contains( "overlay" ) );
    CHECK( !j[ "settings" ].contains( "lag_buffer" ) );
    CHECK( !j.contains( "schema_version" ) );
    CHECK( !j[ "settings" ].contains( "name" ) );
}

TEST_CASE( "export: nothing ticked gives an empty settings object", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Src", Modified() );
    nlohmann::json j = nlohmann::json::parse( ExportOf( "Src", {} ) );
    REQUIRE( j[ "settings" ].is_object() );
    CHECK( j[ "settings" ].empty() );
    CHECK( pe::ExportSettings( "Nope", {}, true ).ok == false );
}

TEST_CASE( "parse: invalid input", "[profile_export]" )
{
    TempHome home;
    pe::ParsedExport p = pe::ParseExport( "{ this is not json" );
    CHECK( !p.valid );
    CHECK( !p.error.empty() );

    p = pe::ParseExport( "[1,2]" );
    CHECK( !p.valid );

    p = pe::ParseExport( R"({"gamescope_ritz_export":1,"profile":"X"})" );
    CHECK( !p.valid );
    CHECK( p.error == "The export has no settings." );

    p = pe::ParseExport( R"({"gamescope_ritz_export":1,"settings":[]})" );
    CHECK( !p.valid );

    p = pe::ParseExport( R"({"settings":{}})" );
    CHECK( !p.valid );

    p = pe::ParseExport( R"({"gamescope_ritz_export":"1","settings":{}})" );
    CHECK( !p.valid );

    p = pe::ParseExport( R"({"gamescope_ritz_export":2,"settings":{}})" );
    CHECK( !p.valid );
    CHECK( p.error.find( "newer version" ) != std::string::npos );

    p = pe::ParseExport( R"({"gamescope_ritz_export":1,"settings":{}})" );
    CHECK( p.valid );
}

TEST_CASE( "apply: destination untouched on failure", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Dest", Modified() );
    const std::string sBefore = Slurp( ProfilePath( "Dest" ) );

    // invalid text
    pe::ParsedExport bad = pe::ParseExport( "nope" );
    pe::ApplyResult r = pe::ApplyImport( bad, "Dest", false, AllGroups() );
    CHECK( !r.success );
    CHECK( !r.error.empty() );

    // valid, nothing ticked
    MakeGeneral( "Src", Settings{} );
    pe::ParsedExport good = pe::ParseExport( ExportOf( "Src", AllGroups() ) );
    REQUIRE( good.valid );
    r = pe::ApplyImport( good, "Dest", false, {} );
    CHECK( !r.success );

    // ticked group that the paste does not hold
    pe::ParsedExport part = pe::ParseExport( ExportOf( "Src", { "motion.blur" } ) );
    r = pe::ApplyImport( part, "Dest", false, { "motion.framegen" } );
    CHECK( !r.success );

    // missing destination
    r = pe::ApplyImport( good, "Ghost", false, AllGroups() );
    CHECK( !r.success );
    CHECK( !ProfileExists( "Ghost" ) );

    CHECK( Slurp( ProfilePath( "Dest" ) ) == sBefore );
    CHECK( !std::filesystem::exists( ProfilePath( "Dest" ) + ".bak" ) );
}

TEST_CASE( "apply: backup is written first, overwritten, and ignored by ListProfiles", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Src", Modified() );
    MakeGeneral( "Dest", Settings{} );
    const std::string sOrig = Slurp( ProfilePath( "Dest" ) );
    const std::string sBak = ProfilePath( "Dest" ) + ".bak";

    pe::ParsedExport p = pe::ParseExport( ExportOf( "Src", AllGroups() ) );
    pe::ApplyResult r = pe::ApplyImport( p, "Dest", false, AllGroups() );
    REQUIRE( r.success );
    CHECK( r.destination == "Dest" );
    CHECK( r.applied > 0 );
    CHECK( Slurp( sBak ) == sOrig );
    CHECK( LoadProfile( "Dest" )->framegen.multiplier == 3 );
    const std::string sFirstNew = Slurp( ProfilePath( "Dest" ) );

    // second import: the .bak becomes the state before THIS import
    MakeGeneral( "Src2", Settings{} );
    pe::ParsedExport p2 = pe::ParseExport( ExportOf( "Src2", { "motion.framegen" } ) );
    r = pe::ApplyImport( p2, "Dest", false, { "motion.framegen" } );
    REQUIRE( r.success );
    CHECK( Slurp( sBak ) == sFirstNew );
    CHECK( LoadProfile( "Dest" )->framegen.multiplier == 2 );
    // untouched groups kept
    CHECK( LoadProfile( "Dest" )->motion_blur.samples == 6 );

    for ( const ProfileMeta &m : ListProfiles() )
        CHECK( m.name.find( "bak" ) == std::string::npos );
    CHECK( ListProfiles().size() == 3 ); // Src, Src2, Dest
}

TEST_CASE( "apply: unreadable values are skipped, the destination's kept", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Dest", Modified() );
    const std::string sText = R"({
      "gamescope_ritz_export": 1, "profile": "X",
      "settings": {
        "framegen": { "multiplier": "lots", "target_fps": 144, "no_such_key": 1 },
        "no_such_section": { "a": 1 },
        "overlay": { "accent_hue": 3 }
      } })";
    pe::ParsedExport p = pe::ParseExport( sText );
    REQUIRE( p.valid );
    CHECK( p.unknown_count == 4 ); // multiplier (wrong type), no_such_key, no_such_section.a, overlay.accent_hue
    REQUIRE( p.groups_present.size() == 1 );

    pe::ApplyResult r = pe::ApplyImport( p, "Dest", false, AllGroups() );
    REQUIRE( r.success );
    CHECK( r.skipped == 1 );
    REQUIRE( r.skipped_keys.size() == 1 );
    CHECK( r.skipped_keys[ 0 ] == "framegen.multiplier" );
    CHECK( r.unknown == 4 );
    std::optional<Settings> s = LoadProfile( "Dest" );
    CHECK( s->framegen.multiplier == 3 ); // kept
    CHECK( s->framegen.target_fps == 144 );
    CHECK( s->motion_blur.samples == 6 );
}

TEST_CASE( "diff: only pasted keys count, defaults never do", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Dest", Modified() );
    const std::string sText = R"({"gamescope_ritz_export":1,"settings":{
        "framegen":{"multiplier":4,"target_fps":0},
        "zoom":{"factor":3.0},
        "crosshair":{"enabled":true}}})";
    pe::ParsedExport p = pe::ParseExport( sText );
    REQUIRE( p.valid );
    CHECK( p.profile.empty() );

    pe::DiffResult d = pe::DiffImport( p, "Dest", AllGroups() );
    // multiplier 3 -> 4 changes; target_fps default 0 == stored 0; zoom 3.0 same;
    // crosshair.enabled is rows or unchanged depending on Modified()'s flip.
    bool bMult = false;
    for ( const pe::DiffRow &row : d.rows )
    {
        if ( row.key == "framegen.multiplier" )
        {
            bMult = true;
            CHECK( row.old_value == "3" );
            CHECK( row.new_value == "4" );
            CHECK( row.group == "Frame generation" );
            CHECK( row.label == row.key );
        }
        CHECK( row.key != "framegen.target_fps" );
        CHECK( row.key != "zoom.factor" );
        CHECK( row.key.rfind( "motion_blur", 0 ) != 0 ); // not pasted
    }
    CHECK( bMult );
    CHECK( d.count == int( d.rows.size() ) );
    CHECK( d.count + d.unchanged == 4 );

    // Unticked group: no rows from it.
    d = pe::DiffImport( p, "Dest", { "overlay.zoom" } );
    CHECK( d.count == 0 );
    CHECK( d.unchanged == 1 );

    // Into a new profile, the baseline is the defaults.
    d = pe::DiffImport( p, "", AllGroups() );
    bool bMultNew = false;
    for ( const pe::DiffRow &row : d.rows )
        if ( row.key == "framegen.multiplier" )
        {
            bMultNew = true;
            CHECK( row.old_value == std::to_string( Settings{}.framegen.multiplier ) );
        }
    CHECK( bMultNew );

    // Diff changes nothing.
    CHECK( LoadProfile( "Dest" )->framegen.multiplier == 3 );
    CHECK( !std::filesystem::exists( ProfilePath( "Dest" ) + ".bak" ) );
}

TEST_CASE( "apply: new profile naming", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Src", Modified() );
    pe::ParsedExport p = pe::ParseExport( ExportOf( "Src", AllGroups() ) );
    REQUIRE( p.valid );

    CHECK( pe::ApplyImport( p, "Friend", true, AllGroups() ).destination == "Friend" );
    CHECK( pe::ApplyImport( p, "Friend", true, AllGroups() ).destination == "Friend 2" );
    CHECK( pe::ApplyImport( p, "Friend", true, AllGroups() ).destination == "Friend 3" );
    CHECK( pe::ApplyImport( p, "", true, AllGroups() ).destination == "Imported" );
    CHECK( pe::ApplyImport( p, "../../", true, AllGroups() ).destination == "Imported 2" );
    CHECK( ProfileExists( "Friend 3" ) );
    CHECK( LoadProfileMeta( "Friend 2" )->kind == ProfileKind::General );
    // A partial import into a new profile starts from defaults.
    pe::ParsedExport part = pe::ParseExport( ExportOf( "Src", { "motion.framegen" } ) );
    pe::ApplyResult r = pe::ApplyImport( part, "Partial", true, { "motion.framegen" } );
    REQUIRE( r.success );
    CHECK( LoadProfile( "Partial" )->framegen.multiplier == 3 );
    CHECK( LoadProfile( "Partial" )->motion_blur.samples == Settings{}.motion_blur.samples );
    CHECK( !std::filesystem::exists( ProfilePath( "Partial" ) + ".bak" ) );
}

TEST_CASE( "export: resolved vs stored on an inheriting game profile", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Parent", Modified() );
    Settings game = Modified();
    game.framegen.multiplier = 4; // the one difference
    MakeGame( "GameG", "Parent", game );
    REQUIRE( LoadProfileMeta( "GameG" )->inherits == "Parent" );

    nlohmann::json stored = nlohmann::json::parse( ExportOf( "GameG", AllGroups(), false ) );
    CHECK( stored[ "settings" ][ "framegen" ][ "multiplier" ] == 4 );
    CHECK( !stored[ "settings" ][ "framegen" ].contains( "target_fps" ) ); // inherited: not stored
    CHECK( !stored[ "settings" ].contains( "zoom" ) );

    nlohmann::json resolved = nlohmann::json::parse( ExportOf( "GameG", AllGroups(), true ) );
    CHECK( resolved[ "settings" ][ "framegen" ][ "multiplier" ] == 4 );
    CHECK( resolved[ "settings" ][ "framegen" ].contains( "target_fps" ) );
    CHECK( resolved[ "settings" ][ "zoom" ][ "factor" ] == 3.0 );

    // A general profile is full either way.
    nlohmann::json gen = nlohmann::json::parse( ExportOf( "Parent", AllGroups(), false ) );
    CHECK( gen[ "settings" ][ "zoom" ][ "factor" ] == 3.0 );
}

TEST_CASE( "apply: an inheriting game profile stays sparse", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Parent", Modified() );
    ProfileMeta meta = MakeGame( "GameG", "Parent", Modified() );
    MakeGeneral( "Src", Modified() );

    const std::string sText = R"({"gamescope_ritz_export":1,"settings":{"framegen":{"multiplier":5}}})";
    pe::ParsedExport p = pe::ParseExport( sText );
    pe::ApplyResult r = pe::ApplyImport( p, "GameG", false, AllGroups() );
    REQUIRE( r.success );

    nlohmann::json jFile = ReadFile( ProfilePath( "GameG" ) );
    CHECK( jFile[ "inherits" ] == "Parent" );
    REQUIRE( jFile.contains( "framegen" ) );
    CHECK( jFile[ "framegen" ][ "multiplier" ] == 5 );
    CHECK( jFile[ "framegen" ].size() == 1 );
    CHECK( !jFile.contains( "zoom" ) );
    CHECK( LoadProfile( "GameG" )->framegen.multiplier == 5 );
    CHECK( LoadProfile( "GameG" )->zoom.factor == 3.0f ); // still follows the parent
    CHECK( LoadProfileMeta( "GameG" )->app_id == meta.app_id );
}

TEST_CASE( "apply: pasted numbers must match what ConfigManager reads", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Dest", Modified() ); // fps_limit 90, zoom.factor 3.0
    auto paste = []( const char *pszSettings )
    {
        return pe::ParseExport( std::string( R"({"gamescope_ritz_export":1,"settings":)" ) + pszSettings + "}" );
    };

    // A decimal into an integer setting: unreadable, skipped, the destination keeps 90.
    pe::ParsedExport p = paste( R"({"gamescope":{"fps_limit":144.5}})" );
    REQUIRE( p.valid );
    CHECK( p.unknown_count == 1 );
    pe::ApplyResult r = pe::ApplyImport( p, "Dest", false, AllGroups() );
    REQUIRE( r.success );
    CHECK( LoadProfile( "Dest" )->gamescope.fps_limit == 90 );

    // Too big for an int, either sign.
    for ( const char *psz : { R"({"gamescope":{"fps_limit":3000000000}})", R"({"gamescope":{"fps_limit":-3000000000}})" } )
    {
        p = paste( psz );
        REQUIRE( p.valid );
        CHECK( p.unknown_count == 1 );
        REQUIRE( pe::ApplyImport( p, "Dest", false, AllGroups() ).success );
        CHECK( LoadProfile( "Dest" )->gamescope.fps_limit == 90 );
    }

    // An integer into a float setting is read by ConfigManager, so it applies.
    p = paste( R"({"zoom":{"factor":2}})" );
    REQUIRE( p.valid );
    CHECK( p.unknown_count == 0 );
    REQUIRE( pe::ApplyImport( p, "Dest", false, AllGroups() ).success );
    CHECK( LoadProfile( "Dest" )->zoom.factor == 2.0f );

    // A plain integer into an integer setting applies.
    p = paste( R"({"gamescope":{"fps_limit":144}})" );
    CHECK( p.unknown_count == 0 );
    REQUIRE( pe::ApplyImport( p, "Dest", false, AllGroups() ).success );
    CHECK( LoadProfile( "Dest" )->gamescope.fps_limit == 144 );

    // A bool or string where a number is expected stays unreadable.
    p = paste( R"({"gamescope":{"fps_limit":true},"zoom":{"factor":"2"}})" );
    CHECK( p.unknown_count == 2 );
}

TEST_CASE( "parse: size and nesting limits", "[profile_export]" )
{
    TempHome home;

    // 100000 levels: refused up front, no crash.
    std::string sDeep( 100000, '[' );
    sDeep += std::string( 100000, ']' );
    pe::ParsedExport p = pe::ParseExport( sDeep );
    CHECK( !p.valid );
    CHECK( p.error.find( "too large or too deeply nested" ) != std::string::npos );

    std::string sObj;
    for ( int i = 0; i < 100000; i++ )
        sObj += R"({"a":)";
    sObj += "1" + std::string( 100000, '}' );
    CHECK( !pe::ParseExport( sObj ).valid );

    // Over 1 MB.
    std::string sBig = R"({"gamescope_ritz_export":1,"profile":")" + std::string( 1100000, 'x' ) + R"(","settings":{}})";
    p = pe::ParseExport( sBig );
    CHECK( !p.valid );
    CHECK( p.error.find( "too large" ) != std::string::npos );

    // Brackets inside strings do not count.
    p = pe::ParseExport( R"({"gamescope_ritz_export":1,"profile":"[[[[[[[[[[[[[[[[[[[[\"[[","settings":{}})" );
    CHECK( p.valid );

    // A normal full export stays valid.
    MakeGeneral( "Full", Modified() );
    p = pe::ParseExport( ExportOf( "Full", AllGroups() ) );
    CHECK( p.valid );

    // The deepest real key nests this far (plus the export wrapper's two levels).
    size_t nDots = 0;
    for ( const std::string &s : pe::AllSettingsKeys() )
        nDots = std::max<size_t>( nDots, size_t( std::count( s.begin(), s.end(), '.' ) ) );
    WARN( "max dots in a settings key: " << nDots );
    CHECK( nDots + 2 <= 16 );
}

TEST_CASE( "check: the dry run refuses exactly what apply refuses and writes nothing", "[profile_export]" )
{
    TempHome home;
    MakeGeneral( "Dest", Modified() );
    const std::string sBefore = Slurp( ProfilePath( "Dest" ) );
    MakeGeneral( "Src", Settings{} );
    pe::ParsedExport good = pe::ParseExport( ExportOf( "Src", AllGroups() ) );
    REQUIRE( good.valid );

    CHECK( pe::CheckImport( good, "Dest", false, AllGroups() ).empty() );
    CHECK( !pe::CheckImport( good, "Dest", false, {} ).empty() );
    CHECK( !pe::CheckImport( good, "Ghost", false, AllGroups() ).empty() );
    CHECK( !pe::CheckImport( pe::ParseExport( "nope" ), "Dest", false, AllGroups() ).empty() );
    pe::ParsedExport part = pe::ParseExport( ExportOf( "Src", { "motion.blur" } ) );
    CHECK( !pe::CheckImport( part, "Dest", false, { "motion.framegen" } ).empty() );

    CHECK( Slurp( ProfilePath( "Dest" ) ) == sBefore );
    CHECK( !std::filesystem::exists( ProfilePath( "Dest" ) + ".bak" ) );
}
